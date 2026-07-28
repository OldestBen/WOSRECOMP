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
#include <algorithm>

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

// The ring buffer itself, so its contents can be read rather than guessed at.
std::atomic<uint32_t> g_ringPhysical{0};
std::atomic<uint32_t> g_ringVirtual{0};
std::atomic<uint32_t> g_ringSize{0};

// Xbox 360 GPU register window, and the ring buffer write pointer within it.
// Confirmed by the game itself: its only write during init landed on
// 0x7FC80714.
constexpr uint32_t kGpuRegisterBase = 0x7FC80000;
constexpr uint32_t kGpuWritePointerReg = kGpuRegisterBase + 0x714;
constexpr uint32_t kGpuReadPointerReg  = kGpuRegisterBase + 0x710;

// The register the graphics interrupt callback tests before doing anything on
// a vblank. Found by disassembling the callback rather than by guessing —
// see the comment at the call site.
constexpr uint32_t kGpuVblankStatusReg = kGpuRegisterBase + 0x6544;

// The system command buffer handed back by VdGetSystemCommandBuffer, and the
// descriptor that accompanies it. The descriptor size is generous rather than
// exact — the caller reads +4 and +8, and over-allocating costs nothing while
// a short block would let an unknown field scribble past the end.
constexpr uint32_t kSystemCommandBufferSize = 0x10000;
constexpr uint32_t kSystemDescriptorSize    = 0x20;

// Every address the game hands the video driver is a PHYSICAL address.
//
// MmGetPhysicalAddress is `addr & 0x1FFFFFFF` — it strips the alias window and
// keeps the offset — and the game calls it before VdInitializeRingBuffer and
// VdEnableRingBufferRPtrWriteBack. So the 0x0006023C we were given is physical,
// and the memory it refers to is the physical allocation the game made at
// virtual 0xA006023C.
//
// We were storing the read pointer to guest 0x0006023C directly, i.e. to an
// unrelated page near the bottom of the address space — which the harness then
// helpfully committed, making the mistake look deliberate. The game read its
// own virtual alias and saw nothing change, forever.
//
// The evidence is in the game's own hang dump, now that it formats properly:
// it reports Snooped 0xa0060200 and NonSnooped 0xa0030200, both inside
// physical allocations we handed out at 0xA0030000 and 0xA0060000. Its view of
// this memory is the 0xA0000000 alias, so that is where writes have to land.
constexpr uint32_t kPhysicalAlias = 0xA0000000;

uint32_t PhysicalToVirtual(uint32_t physical)
{
    // Already an alias address (the game sometimes passes one through
    // unconverted); leave it alone rather than aliasing it twice.
    if (physical >= 0x80000000u)
        return physical;
    return kPhysicalAlias + physical;
}

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

// Walk the ring buffer as PM4 packets and report what is in it.
//
// The encoding is now established rather than assumed. The first dump gave:
//
//     +0000: C0114800 000003FF 00000000 ... (18 payload dwords)
//     +004C: C0013F00 009C0140 0000000B
//     +0058: C0013F00 00940100 00000040
//
// Reading the header as [31:30] type, [29:16] count-1, [15:8] opcode predicts
// the next packet starts at +0x4C, and it does — a type-3 header sits exactly
// there. That arithmetic checking out twice in a row is what makes this a
// decode rather than a guess.
//
// Opcode 0x3F is an indirect buffer: address then dword count. Both addresses
// land inside physical allocations the game made, once put through the
// 0xA0000000 alias. So the ring is nearly empty by design and the actual
// commands — including whatever advances the GPU fence — live in the buffers
// it points at. Hence the recursion.
//
// This only reports. Executing the packets is the next step, and it needs to
// know which opcode carries the fence write, which is precisely what this
// prints.
constexpr uint32_t kPm4IndirectBuffer = 0x3F;

// Opcodes seen so far, so each one is reported the first time only.
//
// The one-shot dump caught the first 31 dwords, by which point the CPU fence
// was already at 7 — so the packet that advances the GPU fence had already
// gone past, or had not been submitted yet. Reporting first sightings across
// every frame gives a complete inventory of what the game actually uses,
// without a per-frame flood.
bool g_seenType3[128] = {};

void ExecutePackets(uint8_t* base, uint32_t bufferVirtual, uint32_t dwordCount, unsigned depth)
{
    for (uint32_t i = 0; i < dwordCount; )
    {
        const uint32_t header = wos::LoadU32(base, bufferVirtual + i * 4);
        const uint32_t type = header >> 30;

        // Type 2 is a single-dword filler used to pad to the end of the ring.
        // It has no count field, so treating it like the others desyncs the
        // walk and turns everything after it into noise.
        if (type == 2)
        {
            ++i;
            continue;
        }

        const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
        // Header at i, payload at i+1..i+count, so the last word read is
        // i+count and it has to stay inside the buffer.
        if (i + count >= dwordCount)
            break;   // truncated packet — stop rather than read past the end

        if (type == 0)
        {
            // A type-0 packet is a run of register writes starting at the
            // register in the header. Performing them is unambiguous, and it
            // matters: the game emits a wait-on-register (op 0x3C) against
            // register 0x0A31 right after writing it, so register state has to
            // be live rather than permanently zero.
            const uint32_t firstReg = header & 0x7FFF;
            for (uint32_t j = 0; j < count && i + 1 + j < dwordCount; ++j)
            {
                const uint32_t value = wos::LoadU32(base, bufferVirtual + (i + 1 + j) * 4);
                wos::StoreU32(base, kGpuRegisterBase + (firstReg + j) * 4, value);
            }
        }
        else if (type == 3)
        {
            const uint32_t opcode = (header >> 8) & 0x7F;

            if (!g_seenType3[opcode & 0x7F])
            {
                g_seenType3[opcode & 0x7F] = true;
                printf("[gpu] first sighting: type3 op=0x%02X count=%u depth=%u:",
                    opcode, count, depth);
                for (uint32_t j = 1; j <= count && j <= 8 && i + j < dwordCount; ++j)
                    printf(" %08X", wos::LoadU32(base, bufferVirtual + (i + j) * 4));
                if (count > 8)
                    printf(" ...");
                printf("\n");
            }

            // Write-to-memory packets. This is the mechanism the GPU fence
            // rides on, and executing it is the whole point of the walk.
            //
            // The evidence, from the first sighting of op 0x58:
            //
            //     00000003 00060206 A09401D4
            //
            // Three dwords: an initiator, an address, a value. Masking the low
            // two selector bits off 0x00060206 gives physical 0x00060204,
            // which aliases to virtual 0xA0060204 — and the game's own hang
            // dump reports "Snooped 0xa0060200". The packet targets the block
            // the game reads its GPU fence from, four bytes in.
            //
            // Stated as a hypothesis because it is one: the value in that
            // first packet (0xA09401D4) points into the command buffer the
            // packet itself lives in, which reads more like a progress marker
            // than a fence counter. Both are things the game polls, and both
            // are stuck for the same reason, so executing the write is worth
            // doing either way. Every write is logged for the first few so a
            // run says plainly whether the values look like a fence.
            //
            // Bounded to the physical alias window: a misparsed packet must
            // not be able to scribble on the image or the heap.
            if ((opcode == 0x58 || opcode == 0x46 || opcode == 0x5A) && count >= 3)
            {
                const uint32_t rawAddr = wos::LoadU32(base, bufferVirtual + (i + 2) * 4);
                const uint32_t value = wos::LoadU32(base, bufferVirtual + (i + 3) * 4);
                const uint32_t target = PhysicalToVirtual(rawAddr & ~3u);

                if (target >= kPhysicalAlias && target < 0xC0000000u)
                {
                    wos::StoreU32(base, target, value);

                    // The cap needs to announce itself. Without the notice
                    // below, a log that simply stops mentioning these writes
                    // reads exactly like a fence that stopped advancing —
                    // which is precisely the wrong conclusion this log talked
                    // me into once already. The device probe reports the live
                    // value, so silence here costs nothing.
                    static unsigned s_logged = 0;
                    if (s_logged < 12)
                    {
                        ++s_logged;
                        printf("[gpu] op=0x%02X write: raw 0x%08X -> virtual 0x%08X = 0x%08X\n",
                            opcode, rawAddr, target, value);
                        if (s_logged == 12)
                            printf("[gpu] (further memory-write packets suppressed — "
                                   "they continue; watch the [d3d] fence value)\n");
                    }
                }
                else
                {
                    printf("[gpu] op=0x%02X write REFUSED: raw 0x%08X resolves to 0x%08X, "
                           "outside the physical alias window\n", opcode, rawAddr, target);
                }
            }

            if (opcode == kPm4IndirectBuffer && count >= 2 && depth < 4)
            {
                const uint32_t ibPhysical = wos::LoadU32(base, bufferVirtual + (i + 1) * 4);
                const uint32_t ibWords = wos::LoadU32(base, bufferVirtual + (i + 2) * 4);
                const uint32_t ibVirtual = PhysicalToVirtual(ibPhysical);
                if (ibVirtual != 0 && ibWords > 0 && ibWords < 0x40000)
                    ExecutePackets(base, ibVirtual, ibWords, depth + 1);
            }
        }

        i += count + 1;
    }
}

// Consume everything the game has submitted since last time.
//
// This is the shape of a command processor, not one yet: it follows indirect
// buffers and performs register writes, and reports every other opcode once so
// the list of what still needs handling comes from the game rather than from
// guesswork. The GPU fence is the outstanding one — the CPU fence climbs by
// two per frame while the GPU fence stays at 1 — and the packet that carries
// it will name itself here the first time it is submitted.
void ConsumeRing(uint8_t* base, uint32_t wptr)
{
    static uint32_t s_consumed = 0;

    const uint32_t ring = g_ringVirtual.load(std::memory_order_relaxed);
    const uint32_t size = g_ringSize.load(std::memory_order_relaxed);
    if (ring == 0 || size == 0)
        return;

    // g_ringSize is a dword count. This used to divide it by four, treating the
    // size as bytes, which put the capacity at 0x1000 — and the game drove the
    // write pointer to 0x1003. The guard below then returned silently, so
    // consumption stopped dead while the read pointer kept being published as
    // caught up. That is why the fence froze at exactly 0x54f in two
    // consecutive runs regardless of how often the ring was polled.
    const uint32_t capacity = size;
    if (wptr > capacity)
    {
        // Never fail silently here again.
        static bool s_warned = false;
        if (!s_warned)
        {
            s_warned = true;
            printf("[gpu] write pointer 0x%X is past the ring capacity 0x%X — "
                   "not consuming. The ring size is being misread.\n", wptr, capacity);
        }
        return;
    }
    if (wptr == s_consumed)
        return;

    // The ring wraps. Handle the wrapped case as two straight runs rather than
    // one that reads off the end.
    if (wptr > s_consumed)
    {
        ExecutePackets(base, ring + s_consumed * 4, wptr - s_consumed, 0);
    }
    else
    {
        ExecutePackets(base, ring + s_consumed * 4, capacity - s_consumed, 0);
        ExecutePackets(base, ring, wptr, 0);
    }

    s_consumed = wptr;
}

// Dump the command packets the game has actually written into the ring.
//
// The remaining problem is the GPU fence. The game's hang dump reports:
//
//     CPU fence 0x7, GPU fence 0x1        (then 0x9, 0xb, 0xd, 0xf, 0x11 ...)
//
// The CPU fence advances by two per frame and the GPU fence never moves off 1.
// The game submits a command that means "write this value to this address when
// you get here", and waits for the value to appear. Nothing here executes
// commands, so it never appears.
//
// Making that work means interpreting the ring, and the packet encoding is not
// something this code has established. So dump the words once and read them,
// rather than writing a parser from memory and getting a fence protocol subtly
// wrong — the failure mode of a wrong parser is a game that renders garbage
// intermittently, which is far harder to diagnose than one that does not
// render at all.
//
// Written as a one-shot: the ring is small and the interesting part is the
// first submission.
void DumpRingOnce(uint8_t* base, uint32_t wptr)
{
    static bool s_done = false;
    if (s_done)
        return;

    const uint32_t ring = g_ringVirtual.load(std::memory_order_relaxed);
    const uint32_t size = g_ringSize.load(std::memory_order_relaxed);
    if (ring == 0 || size == 0 || wptr == 0)
        return;

    s_done = true;

    // wptr is a dword index into the ring, not a byte offset — it tracked the
    // packet count exactly (0x1F, 0x25, 0x2B: six dwords per frame).
    const uint32_t words = std::min<uint32_t>(wptr, size);

    printf("[video] ring contents, %u dword(s) at virtual 0x%08X:\n", words, ring);
    for (uint32_t i = 0; i < words; i += 8)
    {
        printf("[video]   +%04X:", i * 4);
        for (uint32_t j = i; j < i + 8 && j < words; ++j)
            printf(" %08X", wos::LoadU32(base, ring + j * 4));
        printf("\n");
    }
    printf("[video] (dumped once — this is what has to be interpreted for the\n"
           "        GPU fence to advance; see the comment in video.cpp)\n");
}

// The command processor: consume the ring as fast as the game fills it.
//
// This used to run on the vblank thread, which made the GPU exactly as fast as
// the display — 60 consumptions a second. Executing the fence writes proved
// the mechanism works (the fence went from stuck at 1 to tracking the CPU
// four behind, 0x54f against 0x553), but four behind is still behind, and the
// game's D3D layer measures that gap and calls it a hang.
//
// A real GPU does not wait for vblank to read its ring, so neither should
// this. Polling the write pointer on its own thread decouples the two: the
// display heartbeat stays at 60 Hz for the interrupt callback, and command
// consumption runs at whatever rate the game submits.
//
// The poll interval is a compromise. Spinning would close the gap fastest and
// burn a core doing it; 200 microseconds is roughly 80x more responsive than
// a vblank tick while still sleeping most of the time.
void CommandProcessorThread(uint8_t* base)
{
    while (g_vblankRunning.load(std::memory_order_relaxed))
    {
        const uint32_t wptr = wos::LoadU32(base, kGpuWritePointerReg);

        // Consume before publishing the read pointer, so the two agree: the
        // read pointer we publish means "everything up to here has been
        // processed", not "will be shortly".
        ConsumeRing(base, wptr);

        const uint32_t rptrPtr = g_rptrWriteBackPtr.load(std::memory_order_relaxed);
        if (rptrPtr != 0)
            wos::StoreU32(base, rptrPtr, wptr);

        // The game reads CP_RB_RPTR out of the register window too — its hang
        // dump printed "CP_RB_RPTR: 0x00000000" against a non-zero write
        // pointer, which is exactly the picture of a GPU that has consumed
        // nothing.
        wos::StoreU32(base, kGpuReadPointerReg, wptr);

        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
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
        ReportRingProgress(base, g_rptrWriteBackPtr.load(std::memory_order_relaxed));
        DumpRingOnce(base, wos::LoadU32(base, kGpuWritePointerReg));

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

        // TWO arguments: (source, context). Context goes in r4.
        //
        // I had this as three arguments — (source, cpu, userdata) — on the
        // theory that the middle slot was a CPU number. Reading guest
        // 0x82AB9840 settles it:
        //
        //     82AB984C  mr r31,r4         ; context
        //     82AB9850  cmplwi cr6,r3,1   ; source
        //
        // r4 is the context and there is no cpu argument. The three-argument
        // version put zero in r4, so every access through r31 in the callback
        // — [r31+0x2A94], [r31+0x2A98] — read the zero page, and the vblank
        // handler was invoked with a null device. That was my error, and it
        // silently disabled the callback for every run since.
        ctx.r3.u64 = 0;                                             // source: vblank
        ctx.r4.u64 = g_interruptUserData.load(std::memory_order_relaxed);

        // The source-0 path is gated on a GPU register:
        //
        //     82AB98D8  lis r11,32712        ; 0x7FC80000
        //     82AB98DC  lwz r11,25924(r11)   ; register at 0x7FC86544
        //     82AB98E0  clrlwi. r11,r11,31   ; bit 0
        //     82AB98E4  beq  -> return       ; clear means "not for me"
        //
        // Nothing ever wrote that register, so it read 0 and the callback
        // returned immediately every single time — which is why ev5 and ev6
        // have never been signalled in any run. Set bit 0 to say a vblank is
        // pending; the game's own handler clears what it needs.
        wos::StoreU32(base, kGpuVblankStatusReg,
            wos::LoadU32(base, kGpuVblankStatusReg) | 1u);

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
        std::thread(CommandProcessorThread, base).detach();
        printf("[video] vblank thread started at ~60 Hz\n");
        printf("[video] command processor started (200 us poll)\n");
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
    const uint32_t physical = ctx.r3.u32;
    const uint32_t virt = (physical != 0) ? PhysicalToVirtual(physical) : 0;
    // The size argument is a log2 DWORD count, not bytes. Reading it as bytes
    // put the capacity at a quarter of its real value, and the game drove the
    // write pointer past it — proof enough, since a write pointer cannot
    // exceed the buffer it indexes.
    const uint32_t sizeDwords = (ctx.r4.u32 < 32) ? (1u << ctx.r4.u32) : 0;

    g_ringPhysical = physical;
    g_ringVirtual = virt;
    g_ringSize = sizeDwords;

    printf("[video] ring buffer: physical 0x%08X -> virtual 0x%08X, size 2^%u = 0x%X dword(s)\n",
        physical, virt, ctx.r4.u32, sizeDwords);
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
    const uint32_t physical = ctx.r3.u32;
    const uint32_t virt = (physical != 0) ? PhysicalToVirtual(physical) : 0;
    g_rptrWriteBackPtr = virt;
    printf("[video] ring buffer read-pointer writeback: physical 0x%08X -> virtual 0x%08X\n",
        physical, virt);
    if (virt != 0)
        wos::StoreU32(base, virt, 0);
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

#ifdef WOS_IMPL_VdGetSystemCommandBuffer
// VOID VdGetSystemCommandBuffer(VOID* outDescriptor, DWORD* outValue);
//   r3 = descriptor out, r4 = single dword out
//
// Unimplemented until now, which made it a generated stub that wrote nothing
// to either out-parameter — the same failure as NtQueryInformationFile early
// on. Its caller, guest 0x82AC4E48, does:
//
//     82AC5030  bl VdGetSystemCommandBuffer   ; r3 = &sp[208], r4 = &sp[116]
//     82AC5040  lwz r11,116(r1)               ; the r4 out-value
//     82AC5048  stw r11,8(r10)                ; -> [ctx+0x2A90]+8
//     82AC5088  addi r6,r1,208                ; the r3 struct, passed on to...
//     82AC5094  bl VdSwap
//
// so it read four uninitialised stack slots and handed them to VdSwap — the
// call that has never fired in any run.
//
// The r4 value is a pointer to the system command buffer; hand back a real
// allocation so the store into the graphics context is meaningful. The r3
// descriptor's layout is NOT established: the caller reads +4 and +8 from it
// and passes the whole thing to VdSwap. Zero it rather than invent fields —
// the caller's own test is `if ([r3+8] != 0) store it`, so zero takes the
// conservative branch instead of committing to a guess. If a run shows it
// needs real contents, that is a measurement away.
PPC_FUNC(__imp__VdGetSystemCommandBuffer)
{
    WOS_IMPORT_STUB("VdGetSystemCommandBuffer");

    static uint32_t s_commandBuffer = 0;
    if (s_commandBuffer == 0)
    {
        s_commandBuffer = wos::GuestAlloc(base, 0, kSystemCommandBufferSize, 0x1000);
        printf("[video] system command buffer at guest 0x%08X (0x%X bytes)\n",
            s_commandBuffer, kSystemCommandBufferSize);
    }

    if (ctx.r3.u32 != 0)
    {
        for (uint32_t i = 0; i < kSystemDescriptorSize; i += 4)
            wos::StoreU32(base, ctx.r3.u32 + i, 0);
    }

    if (ctx.r4.u32 != 0)
        wos::StoreU32(base, ctx.r4.u32, s_commandBuffer);
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
