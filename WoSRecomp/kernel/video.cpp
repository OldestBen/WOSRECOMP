// The video driver (Vd*) — enough of it to keep the game running.
//
// There is no GPU here and nothing is drawn. What this provides is the shape
// of one: a video mode to query, a ring buffer to accept, and — the part that
// actually matters — a **vblank interrupt**.
//
// The console's graphics driver calls back into the title on every vertical
// blank, and the game's render loop waits on that. With the callback never
// firing, the last run reached VdInitializeRingBuffer and then simply stopped,
// waiting forever for a frame boundary that could not arrive. So a host thread
// stands in for the display's 60 Hz heartbeat.

#include "ppc_recomp_shared.h"
#include "import_log.h"
#include "kernel_overrides.h"
#include "guest.h"
#include "object.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace
{

// 720p, progressive, widescreen — the mode a 360 title is happiest with, and
// the one least likely to send it down an interlaced or SD code path we have
// even less chance of satisfying.
constexpr uint32_t kDisplayWidth  = 1280;
constexpr uint32_t kDisplayHeight = 720;
constexpr float    kRefreshRate   = 60.0f;

std::atomic<uint32_t> g_interruptCallback{0};   // guest function address
std::atomic<uint32_t> g_interruptUserData{0};
std::atomic<bool> g_vblankRunning{false};
std::atomic<uint64_t> g_vblankCount{0};

// Where the GPU would write the ring buffer read pointer back to. The game
// polls this to find out how much of its command buffer has been consumed;
// leaving it frozen is another way to hang forever, so we advance it to
// wherever the write pointer is — i.e. "the GPU has caught up".
std::atomic<uint32_t> g_rptrWriteBackPtr{0};

// Xbox 360 GPU register window, and the ring buffer write pointer within it.
// Confirmed by the game itself: its only write during init landed on
// 0x7FC80714.
constexpr uint32_t kGpuRegisterBase = 0x7FC80000;
constexpr uint32_t kGpuWritePointerReg = kGpuRegisterBase + 0x714;

// X_VIDEO_MODE, 0x30 bytes, big-endian throughout.
void WriteVideoMode(uint8_t* base, uint32_t out)
{
    if (out == 0)
        return;

    wos::StoreU32(base, out + 0x00, kDisplayWidth);
    wos::StoreU32(base, out + 0x04, kDisplayHeight);
    wos::StoreU32(base, out + 0x08, 0);            // is_interlaced
    wos::StoreU32(base, out + 0x0C, 1);            // is_widescreen
    wos::StoreU32(base, out + 0x10, 1);            // is_hi_def

    // refresh_rate is a float in guest byte order.
    uint32_t refreshBits;
    static_assert(sizeof(refreshBits) == sizeof(kRefreshRate));
    std::memcpy(&refreshBits, &kRefreshRate, sizeof(refreshBits));
    wos::StoreU32(base, out + 0x14, refreshBits);

    wos::StoreU32(base, out + 0x18, 1);            // video_standard: NTSC
    wos::StoreU32(base, out + 0x1C, 0);
    wos::StoreU32(base, out + 0x20, 0);
    wos::StoreU32(base, out + 0x24, 0);
    wos::StoreU32(base, out + 0x28, 0);
    wos::StoreU32(base, out + 0x2C, 0);
}

// Report whether the ring buffer is actually moving.
//
// Reading the game's own code settled what it is waiting for. The watchdog at
// guest 0x82AC0C10 does this, once per spin:
//
//     r29 = the graphics context
//     if ([r29+0x2ABD] & 2) return 0          // aborted
//     progress = *(uint32_t*)[r29+0x2A90]     // GPU-written progress word
//     now      = [[r13+0x100]+0x58]           // tick count
//     if (progress != last) { deadline = now; last = progress; }
//     if (now - deadline < 5000) return 1     // caller loops -> keep waiting
//     ... timeout handler; if it returns 0, deadline = now, return 1
//
// So it waits on *change* in one word, and the only paths out are an abort
// flag or a timeout handler that declines to continue. A word that never
// changes means it never leaves — which is the spin we have been watching.
//
// [r29+0x2A90] is the address handed to VdEnableRingBufferRPtrWriteBack, and
// we mirror CP_RB_WPTR into it. That is only progress if CP_RB_WPTR itself
// moves. Nothing so far proves it does: the game wrote that register once
// during init and we have never seen it written since.
//
// Rather than guess a third time, print both words and let a run say which is
// stuck. Every ~2 s, and only when something changed or every 30 s otherwise,
// so a long run does not drown in identical lines.
void ReportRingProgress(uint8_t* base, uint32_t rptrPtr)
{
    static uint32_t s_lastWptr = 0;
    static uint32_t s_lastRptr = 0;
    static uint64_t s_lastReport = 0;
    static bool s_first = true;

    const uint64_t frame = g_vblankCount.load(std::memory_order_relaxed);
    if (frame % 120 != 0)
        return;

    const uint32_t wptr = wos::LoadU32(base, kGpuWritePointerReg);
    const uint32_t rptr = (rptrPtr != 0) ? wos::LoadU32(base, rptrPtr) : 0;

    const bool changed = (wptr != s_lastWptr) || (rptr != s_lastRptr);
    if (!s_first && !changed && frame - s_lastReport < 1800)
        return;

    printf("[video] ring: CP_RB_WPTR=0x%08X rptr_writeback[0x%08X]=0x%08X  %s\n",
        wptr, rptrPtr, rptr,
        changed ? "moved" : "UNCHANGED - the game's GPU watchdog sees no progress");

    s_lastWptr = wptr;
    s_lastRptr = rptr;
    s_lastReport = frame;
    s_first = false;
}

// Stands in for the display's vertical blank.
//
// Source 0 is vblank, 1 is a buffer swap. Firing only vblank is the
// conservative choice: a swap notification the game did not ask for could make
// it believe a frame it never submitted has completed.
void VblankThread(uint8_t* base)
{
    using clock = std::chrono::steady_clock;
    auto next = clock::now();

    while (g_vblankRunning.load(std::memory_order_relaxed))
    {
        next += std::chrono::microseconds(16667);   // ~60 Hz
        std::this_thread::sleep_until(next);

        // Tell the game the GPU has consumed everything it submitted.
        //
        // Holding this at 0 is wrong in a way that hangs: for a ring buffer,
        // read == 0 means "the GPU is still at the start", so once the game
        // advances its write pointer it waits for a read pointer that never
        // moves. Mirroring the write pointer instead models a GPU that
        // consumes commands instantly, which is exactly what a run with no
        // rendering should look like.
        //
        // The write pointer lives in the GPU register window. The game wrote
        // to guest 0x7FC80714 once during init, which is CP_RB_WPTR.
        const uint32_t rptrPtr = g_rptrWriteBackPtr.load(std::memory_order_relaxed);
        if (rptrPtr != 0)
            wos::StoreU32(base, rptrPtr, wos::LoadU32(base, kGpuWritePointerReg));

        ReportRingProgress(base, rptrPtr);

        const uint32_t callback = g_interruptCallback.load(std::memory_order_relaxed);
        if (callback == 0)
            continue;

        PPCFunc* fn = PPC_LOOKUP_FUNC(base, callback);
        if (fn == nullptr)
            continue;

        PPCContext ctx{};
        ctx.fpscr.loadFromHost();
        // The callback runs on the guest's interrupt context. It is short, but
        // it is guest code, so it needs a real stack of its own.
        static uint32_t s_interruptStack = 0;
        if (s_interruptStack == 0)
            s_interruptStack = wos::GuestAlloc(base, 0, 0x20000, 0x10000);
        if (s_interruptStack == 0)
            continue;

        ctx.r1.u64 = s_interruptStack + 0x20000 - 0x100;

        // The callback is guest code, so it needs r13 like any other guest
        // thread. Allocated once and reused: this is logically one thread
        // servicing every interrupt, and allocating per firing would leak a
        // block 60 times a second.
        static uint32_t s_interruptPcr = 0;
        if (s_interruptPcr == 0)
            s_interruptPcr = wos::CreateThreadPcr(base);
        ctx.r13.u64 = s_interruptPcr;

        // THREE arguments, not two: (source, cpu, userdata).
        //
        // This was passing userdata in r4 — the *cpu* slot — leaving r5
        // holding whatever was there before. The callback would then use a
        // garbage pointer as its context, which is almost certainly what the
        // unexplained reads of guest 0x59000000 and 0x66020000 were: a
        // dereference of a value that was never a pointer.
        //
        // The consequence is that the callback did nothing useful, so the two
        // graphics threads waiting on their ctx+0x20 events were never woken,
        // and the main thread spun in the graphics layer waiting on them.
        ctx.r3.u64 = 0;                                             // source: vblank
        ctx.r4.u64 = 0;                                             // cpu number
        ctx.r5.u64 = g_interruptUserData.load(std::memory_order_relaxed);

        fn(ctx, base);

        const uint64_t n = g_vblankCount.fetch_add(1) + 1;
        if (n == 1)
            printf("[video] first vblank interrupt delivered\n");
        else if (n == 600)
            printf("[video] 600 vblanks (~10 s) — the render loop is turning over\n");
    }
}

} // namespace

#ifdef WOS_IMPL_VdSetGraphicsInterruptCallback
// VOID VdSetGraphicsInterruptCallback(PVOID callback, PVOID userData);
PPC_FUNC(__imp__VdSetGraphicsInterruptCallback)
{
    WOS_IMPORT_STUB("VdSetGraphicsInterruptCallback");

    g_interruptCallback = ctx.r3.u32;
    g_interruptUserData = ctx.r4.u32;

    printf("[video] graphics interrupt callback at guest 0x%08X (user data 0x%08X)\n",
        ctx.r3.u32, ctx.r4.u32);

    if (ctx.r3.u32 != 0 && !g_vblankRunning.exchange(true))
    {
        std::thread(VblankThread, base).detach();
        printf("[video] vblank thread started at ~60 Hz\n");
    }
}
#endif

#ifdef WOS_IMPL_VdInitializeEngines
PPC_FUNC(__imp__VdInitializeEngines)
{
    WOS_IMPORT_STUB("VdInitializeEngines");
    ctx.r3.u64 = 0;
}
#endif

#ifdef WOS_IMPL_VdShutdownEngines
PPC_FUNC(__imp__VdShutdownEngines)
{
    WOS_IMPORT_STUB("VdShutdownEngines");
    g_vblankRunning = false;
}
#endif

#ifdef WOS_IMPL_VdQueryVideoMode
PPC_FUNC(__imp__VdQueryVideoMode)
{
    WOS_IMPORT_STUB("VdQueryVideoMode");
    WriteVideoMode(base, ctx.r3.u32);
}
#endif

#ifdef WOS_IMPL_XGetVideoMode
PPC_FUNC(__imp__XGetVideoMode)
{
    WOS_IMPORT_STUB("XGetVideoMode");
    WriteVideoMode(base, ctx.r3.u32);
}
#endif

#ifdef WOS_IMPL_VdQueryVideoFlags
PPC_FUNC(__imp__VdQueryVideoFlags)
{
    WOS_IMPORT_STUB("VdQueryVideoFlags");
    // 0x1 widescreen | 0x2 HD | 0x4 progressive.
    ctx.r3.u64 = 0x00000007;
}
#endif

#ifdef WOS_IMPL_VdGetCurrentDisplayGamma
// VOID VdGetCurrentDisplayGamma(DWORD* type, float* gamma);
PPC_FUNC(__imp__VdGetCurrentDisplayGamma)
{
    WOS_IMPORT_STUB("VdGetCurrentDisplayGamma");

    if (ctx.r3.u32 != 0)
        wos::StoreU32(base, ctx.r3.u32, 2);

    if (ctx.r4.u32 != 0)
    {
        constexpr float gamma = 2.222222f;
        uint32_t bits;
        std::memcpy(&bits, &gamma, sizeof(bits));
        wos::StoreU32(base, ctx.r4.u32, bits);
    }
}
#endif

#ifdef WOS_IMPL_VdGetCurrentDisplayInformation
PPC_FUNC(__imp__VdGetCurrentDisplayInformation)
{
    WOS_IMPORT_STUB("VdGetCurrentDisplayInformation");
    // The first fields overlap X_VIDEO_MODE closely enough for the game's
    // purposes; anything it reads beyond that is zeroed by the allocator.
    WriteVideoMode(base, ctx.r3.u32);
}
#endif

#ifdef WOS_IMPL_VdInitializeRingBuffer
// VOID VdInitializeRingBuffer(PVOID ptr, DWORD sizeLog2);
PPC_FUNC(__imp__VdInitializeRingBuffer)
{
    WOS_IMPORT_STUB("VdInitializeRingBuffer");
    printf("[video] ring buffer at guest 0x%08X, size 2^%u bytes\n",
        ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = 0;
}
#endif

#ifdef WOS_IMPL_VdEnableRingBufferRPtrWriteBack
// VOID VdEnableRingBufferRPtrWriteBack(PVOID ptr, DWORD blockSize);
//
// The GPU writes its read pointer here so the game can tell how far it has
// got. Nothing consumes the ring buffer, so the vblank thread keeps this at
// zero — "fully caught up" — rather than leaving the game to conclude the GPU
// has stalled.
PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack)
{
    WOS_IMPORT_STUB("VdEnableRingBufferRPtrWriteBack");
    g_rptrWriteBackPtr = ctx.r3.u32;
    printf("[video] ring buffer read-pointer writeback at guest 0x%08X\n", ctx.r3.u32);
    if (ctx.r3.u32 != 0)
        wos::StoreU32(base, ctx.r3.u32, 0);
}
#endif

#ifdef WOS_IMPL_VdSetSystemCommandBufferGpuIdentifierAddress
PPC_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress)
{
    WOS_IMPORT_STUB("VdSetSystemCommandBufferGpuIdentifierAddress");
    ctx.r3.u64 = 0;
}
#endif

#ifdef WOS_IMPL_VdCallGraphicsNotificationRoutines
PPC_FUNC(__imp__VdCallGraphicsNotificationRoutines)
{
    WOS_IMPORT_STUB("VdCallGraphicsNotificationRoutines");
    ctx.r3.u64 = 0;
}
#endif

#ifdef WOS_IMPL_VdIsHSIOTrainingSucceeded
PPC_FUNC(__imp__VdIsHSIOTrainingSucceeded)
{
    WOS_IMPORT_STUB("VdIsHSIOTrainingSucceeded");
    ctx.r3.u64 = 1;   // yes — there is no real link to train
}
#endif

#ifdef WOS_IMPL_VdRetrainEDRAM
PPC_FUNC(__imp__VdRetrainEDRAM)
{
    WOS_IMPORT_STUB("VdRetrainEDRAM");
    ctx.r3.u64 = 0;
}
#endif

#ifdef WOS_IMPL_VdRetrainEDRAMWorker
PPC_FUNC(__imp__VdRetrainEDRAMWorker)
{
    WOS_IMPORT_STUB("VdRetrainEDRAMWorker");
    ctx.r3.u64 = 0;
}
#endif

#ifdef WOS_IMPL_VdPersistDisplay
PPC_FUNC(__imp__VdPersistDisplay)
{
    WOS_IMPORT_STUB("VdPersistDisplay");
    ctx.r3.u64 = 1;
}
#endif

#ifdef WOS_IMPL_VdSwap
PPC_FUNC(__imp__VdSwap)
{
    WOS_IMPORT_STUB("VdSwap");
    // A frame would be presented here. Counted by the vblank thread instead.
    ctx.r3.u64 = 0;
}
#endif
