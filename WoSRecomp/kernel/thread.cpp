// Thread-local storage and critical sections.
//
// Both are small, both are on the boot path, and both currently return
// garbage. The CRT cannot initialise without working TLS, and every
// RtlEnterCriticalSection that returns without doing anything is a lock the
// game believes it holds.
//
// These use host primitives rather than emulating the guest structures.
// Critical sections are keyed by their guest address, so the guest's own
// RTL_CRITICAL_SECTION memory is left untouched — we never have to match its
// layout, only its identity.

#include "ppc_recomp_shared.h"
#include "import_log.h"
#include "kernel_overrides.h"
#include "guest.h"
#include "object.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <chrono>
// (std::recursive_timed_mutex lives in <mutex>)

namespace wos
{

// A guest thread is a real host thread running recompiled guest code.
//
// That works because recompiled functions are ordinary C++ functions: give one
// a fresh PPCContext and a fresh guest stack and it runs independently. What
// it does *not* give us is the guest's scheduling model — affinity, priorities
// and the hardware thread layout are all ignored, which is fine until the game
// depends on a specific interleaving.
struct ThreadObject : KernelObject
{
    std::thread host;
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};

    std::mutex startMutex;
    std::condition_variable startCv;
    bool released = false;         // false while created-suspended

    // A thread handle is a waitable object: waiting on it means "block until
    // this thread exits", and it stays signalled forever afterwards. We had no
    // way to express that, so such waits fell through to the unknown-object
    // path in sync.cpp and returned success immediately — telling the caller a
    // thread had exited while it was still starting up.
    std::mutex exitMutex;
    std::condition_variable exitCv;

    uint32_t entryPoint = 0;
    uint32_t startContext = 0;
    uint32_t stackBase = 0;
    uint32_t stackSize = 0;
    uint32_t threadId = 0;

    ThreadObject() { type = "thread"; }

    void WaitForRelease()
    {
        std::unique_lock<std::mutex> lock(startMutex);
        startCv.wait(lock, [this] { return released; });
    }

    void Release()
    {
        {
            std::lock_guard<std::mutex> lock(startMutex);
            released = true;
        }
        startCv.notify_all();
    }

    // Publish exit under the mutex so a waiter that has already evaluated the
    // predicate and not yet slept cannot miss the notify.
    void MarkFinished()
    {
        {
            std::lock_guard<std::mutex> lock(exitMutex);
            finished = true;
        }
        exitCv.notify_all();
    }

    // true if the thread has exited, false on timeout. A negative timeout is
    // INFINITE, matching the convention the wait imports use.
    bool WaitForExit(int64_t timeoutMs)
    {
        std::unique_lock<std::mutex> lock(exitMutex);
        if (timeoutMs < 0)
        {
            exitCv.wait(lock, [this] { return finished.load(); });
            return true;
        }
        return exitCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
            [this] { return finished.load(); });
    }
};

} // namespace wos

namespace
{

std::atomic<uint32_t> g_nextThreadId{0x1000};
std::atomic<int> g_liveGuestThreads{0};

// Identifies the calling guest thread in diagnostics. 0 means the main thread,
// which never goes through ExCreateThread and so never sets this.
thread_local uint32_t t_guestThreadId = 0;

const char* ThreadLabel(char* buf, size_t n)
{
    if (t_guestThreadId == 0)
        snprintf(buf, n, "main thread");
    else
        snprintf(buf, n, "guest thread %u", t_guestThreadId);
    return buf;
}

// --- TLS ------------------------------------------------------------------
//
// Slot values are per-thread; the slot *allocation* is global. thread_local
// storage on the host gives us the per-thread part for free.
constexpr size_t kMaxTlsSlots = 128;

std::mutex g_tlsMutex;
bool g_tlsSlotUsed[kMaxTlsSlots] = {};

thread_local uint32_t t_tlsValues[kMaxTlsSlots] = {};

// --- Critical sections ----------------------------------------------------

// Critical sections, with deadlock detection.
//
// WHY TIMED: a thread blocked here makes no import call while it waits, so it
// is completely invisible to the heartbeat — which is exactly the state the
// main thread is in. A plain lock() would hide a deadlock forever; a bounded
// attempt turns it into a report naming both the blocked thread and the
// holder.
struct CriticalSection
{
    std::recursive_timed_mutex m;
    std::atomic<uint32_t> owner{0};      // guest thread id, 0 = free
    std::atomic<int> depth{0};
};

std::mutex g_csMapMutex;
// std::map never invalidates references to existing elements, which is why it
// is used rather than unordered_map — a rehash would move a mutex out from
// under a thread currently blocked on it.
std::map<uint32_t, CriticalSection> g_criticalSections;

CriticalSection& CriticalSectionFor(uint32_t guestAddr)
{
    std::lock_guard<std::mutex> lock(g_csMapMutex);
    return g_criticalSections[guestAddr];
}

// Entry trampoline for a guest thread.
//
// Note ctx.fpscr.loadFromHost(): every thread has its own MXCSR, so a context
// left value-initialised here reproduces exactly the FP-exception-mask bug the
// main thread hit — with the added misery of only happening on worker threads.
void GuestThreadMain(uint8_t* base, std::shared_ptr<wos::ThreadObject> self)
{
    self->WaitForRelease();

    PPCFunc* fn = PPC_LOOKUP_FUNC(base, self->entryPoint);
    if (fn == nullptr)
    {
        printf("[thread] no recompiled function at guest 0x%08X — thread %u does nothing\n",
            self->entryPoint, self->threadId);
        self->MarkFinished();
        return;
    }

    PPCContext ctx{};
    ctx.fpscr.loadFromHost();
    ctx.r1.u64 = self->stackBase + self->stackSize - 0x100;   // leave a little headroom
    ctx.r3.u64 = self->startContext;

    // Every guest thread needs its own block through r13 — the fields the game
    // reads there (a clock, a thread identity) are per-thread by definition,
    // so sharing one would be as wrong as leaving it null. See kernel/pcr.cpp.
    ctx.r13.u64 = wos::CreateThreadPcr(base);

    printf("[thread] %u starting at guest 0x%08X (stack 0x%08X + 0x%X)\n",
        self->threadId, self->entryPoint, self->stackBase, self->stackSize);

    t_guestThreadId = self->threadId;

    char label[64];
    snprintf(label, sizeof(label), "guest thread %u (entry 0x%08X)",
        self->threadId, self->entryPoint);
    wos::RegisterThreadForBacktrace(label);

    self->started = true;
    ++g_liveGuestThreads;

    fn(ctx, base);

    --g_liveGuestThreads;
    self->MarkFinished();
    printf("[thread] %u returned\n", self->threadId);
}

} // namespace

#ifdef WOS_IMPL_ExCreateThread
// NTSTATUS ExCreateThread(
//     PHANDLE handle,          // r3
//     DWORD   stackSize,       // r4
//     LPDWORD threadId,        // r5
//     PVOID   xapiThreadStartup, // r6 — XAPI wrapper, ignored
//     PVOID   startAddress,    // r7 — the guest function we must run
//     PVOID   startContext,    // r8 — its argument
//     DWORD   creationFlags);  // r9 — bit 0 = created suspended
PPC_FUNC(__imp__ExCreateThread)
{
    WOS_IMPORT_STUB("ExCreateThread");

    const uint32_t handleOut = ctx.r3.u32;
    uint32_t stackSize = ctx.r4.u32;
    const uint32_t threadIdOut = ctx.r5.u32;
    const uint32_t startAddress = ctx.r7.u32;
    const uint32_t startContext = ctx.r8.u32;
    const uint32_t creationFlags = ctx.r9.u32;

    if (stackSize < 0x10000)
        stackSize = 0x10000;

    auto thread = std::make_shared<wos::ThreadObject>();
    thread->entryPoint = startAddress;
    thread->startContext = startContext;
    thread->stackSize = stackSize;
    thread->threadId = g_nextThreadId.fetch_add(1);
    thread->stackBase = wos::GuestAlloc(base, 0, stackSize, 0x10000);

    if (thread->stackBase == 0)
    {
        printf("[thread] could not allocate a 0x%X byte guest stack\n", stackSize);
        ctx.r3.u64 = wos::kStatusNoMemory;
        return;
    }

    const uint32_t handle = wos::RegisterObject(base, thread);
    if (handle == 0)
    {
        ctx.r3.u64 = wos::kStatusNoMemory;
        return;
    }

    // CREATE_SUSPENDED: start the host thread anyway but park it on the
    // release gate, so NtResumeThread is a signal rather than a thread launch.
    // Simpler than deferring creation, and it keeps handle validity immediate.
    if ((creationFlags & 1) == 0)
        thread->Release();

    thread->host = std::thread(GuestThreadMain, base, thread);
    thread->host.detach();

    if (handleOut != 0)
        wos::StoreU32(base, handleOut, handle);
    if (threadIdOut != 0)
        wos::StoreU32(base, threadIdOut, thread->threadId);

    printf("[thread] created %u: entry 0x%08X, ctx 0x%08X, stack 0x%X, flags 0x%X%s\n",
        thread->threadId, startAddress, startContext, stackSize, creationFlags,
        (creationFlags & 1) ? " (suspended)" : "");

    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif

#ifdef WOS_IMPL_NtResumeThread
// NTSTATUS NtResumeThread(HANDLE, PULONG SuspendCount)
PPC_FUNC(__imp__NtResumeThread)
{
    WOS_IMPORT_STUB("NtResumeThread");

    auto obj = wos::ObjectFromAny(ctx.r3.u32);
    auto* thread = dynamic_cast<wos::ThreadObject*>(obj.get());

    if (thread == nullptr)
    {
        printf("[thread] NtResumeThread on unknown handle 0x%08X\n", ctx.r3.u32);
        ctx.r3.u64 = wos::kStatusInvalidHandle;
        return;
    }

    if (ctx.r4.u32 != 0)
        wos::StoreU32(base, ctx.r4.u32, 1);   // previous suspend count

    thread->Release();
    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif

#ifdef WOS_IMPL_KeResumeThread
// ULONG KeResumeThread(PKTHREAD thread);   // r3
//
// The kernel-mode counterpart of NtResumeThread: it takes an object pointer
// rather than a handle, and returns the previous suspend count instead of an
// NTSTATUS.
//
// This was unimplemented, and it cost an entire thread. A run showed:
//
//     [thread] created 4106: entry 0x829F4C80, ... flags 0x10000001 (suspended)
//     [import  67] KeResumeThread
//
// and then no "[thread] 4106 starting" line, with thread 4106 absent from all
// eleven stacks in the watchdog dump. Created suspended, resumed through a
// stub that did nothing, never ran. Four of the game's thirteen KeSetEvent
// call sites sit around that thread's entry point, so a thread that never
// starts is a plausible reason events go unsignalled.
//
// ObjectFromAny resolves either a handle or a guest-visible object address,
// which is what makes the pointer form work here.
PPC_FUNC(__imp__KeResumeThread)
{
    WOS_IMPORT_STUB("KeResumeThread");

    auto obj = wos::ObjectFromAny(ctx.r3.u32);
    auto* thread = dynamic_cast<wos::ThreadObject*>(obj.get());

    if (thread == nullptr)
    {
        printf("[thread] KeResumeThread on unknown object 0x%08X — nothing resumed\n",
            ctx.r3.u32);
        ctx.r3.u64 = 0;
        return;
    }

    thread->Release();
    ctx.r3.u64 = 1;   // previous suspend count
}
#endif

#ifdef WOS_IMPL_NtSuspendThread
PPC_FUNC(__imp__NtSuspendThread)
{
    WOS_IMPORT_STUB("NtSuspendThread");
    // Suspending a running host thread safely is not something we can do yet,
    // and pretending otherwise would deadlock rather than misbehave quietly.
    printf("[thread] NtSuspendThread is not implemented — ignoring\n");
    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif

#ifdef WOS_IMPL_KeSetAffinityThread
PPC_FUNC(__imp__KeSetAffinityThread)
{
    WOS_IMPORT_STUB("KeSetAffinityThread");
    // The guest is pinning to one of six hardware threads. Host scheduling
    // does not map onto that, and honouring it would only reduce parallelism.
    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif

#ifdef WOS_IMPL_KeSetBasePriorityThread
PPC_FUNC(__imp__KeSetBasePriorityThread)
{
    WOS_IMPORT_STUB("KeSetBasePriorityThread");
    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif

#ifdef WOS_IMPL_KeQueryBasePriorityThread
PPC_FUNC(__imp__KeQueryBasePriorityThread)
{
    WOS_IMPORT_STUB("KeQueryBasePriorityThread");
    ctx.r3.u64 = 0;
}
#endif

#ifdef WOS_IMPL_KeTlsAlloc
PPC_FUNC(__imp__KeTlsAlloc)
{
    WOS_IMPORT_STUB("KeTlsAlloc");

    std::lock_guard<std::mutex> lock(g_tlsMutex);
    for (size_t i = 0; i < kMaxTlsSlots; ++i)
    {
        if (!g_tlsSlotUsed[i])
        {
            g_tlsSlotUsed[i] = true;
            ctx.r3.u64 = uint32_t(i);
            return;
        }
    }

    printf("[tls] out of TLS slots (max %zu)\n", kMaxTlsSlots);
    ctx.r3.u64 = 0xFFFFFFFFu;   // TLS_OUT_OF_INDEXES
}
#endif

#ifdef WOS_IMPL_KeTlsFree
PPC_FUNC(__imp__KeTlsFree)
{
    WOS_IMPORT_STUB("KeTlsFree");

    const uint32_t slot = ctx.r3.u32;
    if (slot < kMaxTlsSlots)
    {
        std::lock_guard<std::mutex> lock(g_tlsMutex);
        g_tlsSlotUsed[slot] = false;
    }
    ctx.r3.u64 = 1;   // TRUE
}
#endif

#ifdef WOS_IMPL_KeTlsGetValue
PPC_FUNC(__imp__KeTlsGetValue)
{
    WOS_IMPORT_STUB("KeTlsGetValue");

    const uint32_t slot = ctx.r3.u32;
    ctx.r3.u64 = slot < kMaxTlsSlots ? t_tlsValues[slot] : 0;
}
#endif

#ifdef WOS_IMPL_KeTlsSetValue
PPC_FUNC(__imp__KeTlsSetValue)
{
    WOS_IMPORT_STUB("KeTlsSetValue");

    const uint32_t slot = ctx.r3.u32;
    if (slot < kMaxTlsSlots)
    {
        t_tlsValues[slot] = ctx.r4.u32;
        ctx.r3.u64 = 1;   // TRUE
    }
    else
    {
        ctx.r3.u64 = 0;
    }
}
#endif

#ifdef WOS_IMPL_RtlInitializeCriticalSection
PPC_FUNC(__imp__RtlInitializeCriticalSection)
{
    WOS_IMPORT_STUB("RtlInitializeCriticalSection");
    CriticalSectionFor(ctx.r3.u32);   // create on first reference
    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif

#ifdef WOS_IMPL_RtlInitializeCriticalSectionAndSpinCount
PPC_FUNC(__imp__RtlInitializeCriticalSectionAndSpinCount)
{
    WOS_IMPORT_STUB("RtlInitializeCriticalSectionAndSpinCount");
    CriticalSectionFor(ctx.r3.u32);   // spin count is a hint; ignore it
    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif

#ifdef WOS_IMPL_RtlEnterCriticalSection
PPC_FUNC(__imp__RtlEnterCriticalSection)
{
    WOS_IMPORT_STUB("RtlEnterCriticalSection");

    // Recursive: the guest may enter the same section more than once on one
    // thread, which is legal for RTL_CRITICAL_SECTION and would deadlock a
    // plain mutex.
    auto& cs = CriticalSectionFor(ctx.r3.u32);

    if (!cs.m.try_lock_for(std::chrono::seconds(5)))
    {
        char label[32];
        printf("[lock] %s: blocked entering critical section 0x%08X for 5s, "
               "held by guest thread %u. Blocked in:\n",
            ThreadLabel(label, sizeof(label)), ctx.r3.u32,
            cs.owner.load(std::memory_order_relaxed));
        wos::PrintGuestStack(14);

        cs.m.lock();   // keep waiting; the report is the point, not giving up
    }

    cs.owner.store(t_guestThreadId, std::memory_order_relaxed);
    cs.depth.fetch_add(1, std::memory_order_relaxed);
}
#endif

#ifdef WOS_IMPL_RtlLeaveCriticalSection
PPC_FUNC(__imp__RtlLeaveCriticalSection)
{
    WOS_IMPORT_STUB("RtlLeaveCriticalSection");

    auto& cs = CriticalSectionFor(ctx.r3.u32);
    if (cs.depth.fetch_sub(1, std::memory_order_relaxed) <= 1)
        cs.owner.store(0, std::memory_order_relaxed);
    cs.m.unlock();
}
#endif

#ifdef WOS_IMPL_RtlDeleteCriticalSection
PPC_FUNC(__imp__RtlDeleteCriticalSection)
{
    WOS_IMPORT_STUB("RtlDeleteCriticalSection");
    // Left in the map deliberately. Erasing it would destroy a mutex another
    // thread might still be blocked on, and leaking one entry per critical
    // section is cheap.
    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif

// The seam declared in object.h. sync.cpp cannot see ThreadObject — it is
// defined in this file — so the dynamic_cast has to happen on this side.
//
// This exists because a wait on a thread handle used to fall through to the
// unknown-object path in NtWaitForSingleObjectEx, which returns success
// immediately. That told the caller a thread had already exited while it was
// still starting up, which is the same class of bug as every other "stub
// returns success to a wait" this project has hit: it does not fail, it
// silently inverts the timing the caller is relying on.
namespace wos
{

int WaitForThreadExit(uint32_t handleOrPtr, int64_t timeoutMs)
{
    auto obj = ObjectFromAny(handleOrPtr);
    auto* thread = dynamic_cast<ThreadObject*>(obj.get());
    if (thread == nullptr)
        return -1;

    // Hold the shared_ptr for the duration: the wait can outlive whatever else
    // referenced the handle, and the condition variable lives in the object.
    return thread->WaitForExit(timeoutMs) ? 1 : 0;
}

} // namespace wos
