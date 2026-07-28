// Events, mutants (mutexes) and waiting.
//
// The game created five events and a mutant before it gave up. With stubs
// these all returned garbage handles, so every subsequent operation on them
// was nonsense.

#include "ppc_recomp_shared.h"
#include "import_log.h"
#include "kernel_overrides.h"
#include "guest.h"
#include "object.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <vector>
#include <cstdio>
#include <mutex>
#include <thread>

namespace wos
{

struct EventObject : KernelObject
{
    std::mutex m;
    std::condition_variable cv;
    bool signalled = false;
    bool manualReset = false;

    // Per-object statistics. The interesting pattern is an object waited on
    // constantly, timing out every time, and never signalled by anyone —
    // that names the missing piece of the runtime exactly.
    std::atomic<uint64_t> waits{0};
    std::atomic<uint64_t> timeouts{0};
    std::atomic<uint64_t> signals{0};
    int index = 0;              // creation order, for readable reporting

    EventObject() { type = "event"; }

    void Set()
    {
        signals.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(m);
            signalled = true;
        }
        // notify_all even for auto-reset: exactly one waiter will consume the
        // signal (they re-check `signalled` under the lock), the rest go back
        // to sleep. notify_one would be enough but is easier to get subtly
        // wrong when a waiter times out between the notify and the wake.
        cv.notify_all();
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lock(m);
        signalled = false;
    }

    // Returns true if the wait was satisfied, false on timeout.
    bool Wait(int64_t timeoutMs)
    {
        waits.fetch_add(1, std::memory_order_relaxed);

        std::unique_lock<std::mutex> lock(m);

        auto ready = [this] { return signalled; };

        if (timeoutMs < 0)
        {
            cv.wait(lock, ready);
        }
        else if (!cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready))
        {
            timeouts.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        if (!manualReset)
            signalled = false;   // auto-reset consumes the signal
        return true;
    }
};

struct MutantObject : KernelObject
{
    std::recursive_mutex m;
    MutantObject() { type = "mutant"; }
};

// All events ever created, for reporting. Weak, so the report never keeps an
// object alive past its natural life.
std::mutex g_eventListMutex;
std::vector<std::weak_ptr<EventObject>> g_allEvents;

// ---------------------------------------------------------------------------
// Wait call-site census.
//
// The per-object counters above answer "what is being waited on"; they cannot
// answer "by whom, from where, and with what timeout". That distinction is now
// the thing standing between us and a frame.
//
// sub_82ACECF0 — the function both graphics threads sit in — contains more than
// one wait, and which one a thread is in decides everything: one waits with a
// ~30 ms relative timeout (0xFFFFFFFFFFFB6C20 in 100 ns units), the other waits
// INFINITE, and the frame-present branch is taken on the *timeout* result, not
// on a signal. Guessing which site a given thread reached has already cost this
// project several wrong turns.
//
// XenonRecomp writes the guest return address into ctx.lr immediately before
// every `bl`, so inside an import implementation ctx.lr is the guest call site
// plus four — exact attribution, for free. Key on (call site, object) so the
// same wait site used against two different objects shows up as two rows, which
// is precisely the case here.
// ---------------------------------------------------------------------------

struct WaitSite
{
    uint32_t callSite = 0;      // guest address of the instruction after the bl
    uint32_t object = 0;
    int64_t timeoutMs = 0;      // last timeout seen; -1 = infinite
    uint64_t calls = 0;
    uint64_t timeouts = 0;
};

std::mutex g_waitSiteMutex;
// Bounded on purpose: this runs on every wait, and an unbounded map keyed by a
// value the guest controls is a slow memory leak waiting to happen.
constexpr size_t kMaxWaitSites = 64;
std::vector<WaitSite> g_waitSites;

void RecordWaitSite(uint32_t callSite, uint32_t object, int64_t timeoutMs, bool signalled)
{
    std::lock_guard<std::mutex> lock(g_waitSiteMutex);

    for (auto& site : g_waitSites)
    {
        if (site.callSite == callSite && site.object == object)
        {
            site.timeoutMs = timeoutMs;
            ++site.calls;
            if (!signalled)
                ++site.timeouts;
            return;
        }
    }

    if (g_waitSites.size() >= kMaxWaitSites)
        return;

    g_waitSites.push_back({ callSite, object, timeoutMs, 1, signalled ? 0ull : 1ull });
}

void ReportWaitSites()
{
    std::vector<WaitSite> rows;
    {
        std::lock_guard<std::mutex> lock(g_waitSiteMutex);
        rows = g_waitSites;
    }

    if (rows.empty())
        return;

    std::sort(rows.begin(), rows.end(),
        [](const WaitSite& a, const WaitSite& b) { return a.calls > b.calls; });

    printf("[waitsites] %zu site(s):\n", rows.size());
    for (size_t i = 0; i < rows.size() && i < 8; ++i)
    {
        char timeout[32];
        if (rows[i].timeoutMs < 0)
            snprintf(timeout, sizeof(timeout), "INFINITE");
        else
            snprintf(timeout, sizeof(timeout), "%lldms", (long long)rows[i].timeoutMs);

        // callSite is the return address; the `bl` itself is four bytes back,
        // which is the address to feed to --disasm.
        printf("    bl@0x%08X -> obj 0x%08X  %-8s  %llu call(s), %llu timeout(s)\n",
            rows[i].callSite - 4, rows[i].object, timeout,
            (unsigned long long)rows[i].calls,
            (unsigned long long)rows[i].timeouts);
    }
}

void ReportWaitActivity()
{
    struct Row { int index; uint32_t ptr; uint64_t waits, timeouts, signals; bool manual; };
    std::vector<Row> rows;

    {
        std::lock_guard<std::mutex> lock(g_eventListMutex);
        for (auto& weak : g_allEvents)
        {
            if (auto ev = weak.lock())
            {
                const uint64_t w = ev->waits.load(std::memory_order_relaxed);
                if (w == 0)
                    continue;
                rows.push_back({ ev->index, ev->guestPtr, w,
                                 ev->timeouts.load(std::memory_order_relaxed),
                                 ev->signals.load(std::memory_order_relaxed),
                                 ev->manualReset });
            }
        }
    }

    if (rows.empty())
    {
        // Call sites are recorded even for objects that are not EventObjects
        // (mutants, adopted-then-freed blocks), so this half of the report can
        // have something to say when the other half does not.
        ReportWaitSites();
        return;
    }

    std::sort(rows.begin(), rows.end(),
        [](const Row& a, const Row& b) { return a.waits > b.waits; });

    printf("[waits]");
    for (size_t i = 0; i < rows.size() && i < 5; ++i)
    {
        // "waits/timeouts/signals" — an object with waits ~= timeouts and
        // zero signals is being waited on by someone nothing ever wakes.
        printf("  ev%d(%s) %llu/%llu/%llu", rows[i].index,
            rows[i].manual ? "manual" : "auto",
            (unsigned long long)rows[i].waits,
            (unsigned long long)rows[i].timeouts,
            (unsigned long long)rows[i].signals);
    }
    printf("   [waits/timeouts/signals]\n");

    ReportWaitSites();
}

void SignalEventIfAny(uint32_t handleOrPtr)
{
    if (handleOrPtr == 0)
        return;
    auto obj = ObjectFromAny(handleOrPtr);
    if (auto* ev = dynamic_cast<EventObject*>(obj.get()))
        ev->Set();
}

} // namespace wos

namespace
{

// The guest passes timeouts as a pointer to a LARGE_INTEGER in 100ns units:
// negative means relative, positive means absolute, null means wait forever.
// Only the relative form matters during startup.
int64_t TimeoutToMillis(uint8_t* base, uint32_t timeoutPtr)
{
    if (timeoutPtr == 0)
        return -1;   // infinite

    const uint64_t hi = wos::LoadU32(base, timeoutPtr);
    const uint64_t lo = wos::LoadU32(base, timeoutPtr + 4);
    const int64_t ticks = int64_t((hi << 32) | lo);

    if (ticks < 0)
        return -ticks / 10000;   // relative, 100ns -> ms

    // An absolute deadline needs a real system clock to compare against. We
    // don't have one wired up yet, so treat it as "don't block indefinitely"
    // rather than pretending to honour it.
    return 0;
}

} // namespace

#ifdef WOS_IMPL_NtCreateEvent
// NTSTATUS NtCreateEvent(PHANDLE, POBJECT_ATTRIBUTES, EVENT_TYPE, BOOLEAN InitialState)
// EVENT_TYPE: 0 = NotificationEvent (manual reset), 1 = SynchronizationEvent.
PPC_FUNC(__imp__NtCreateEvent)
{
    WOS_IMPORT_STUB("NtCreateEvent");

    const uint32_t handleOut = ctx.r3.u32;
    const uint32_t eventType = ctx.r5.u32;
    const uint32_t initialState = ctx.r6.u32;

    auto ev = std::make_shared<wos::EventObject>();
    ev->manualReset = (eventType == 0);
    ev->signalled = (initialState != 0);

    {
        std::lock_guard<std::mutex> lock(wos::g_eventListMutex);
        ev->index = int(wos::g_allEvents.size());
        wos::g_allEvents.push_back(ev);
    }

    const uint32_t handle = wos::RegisterObject(base, ev);
    if (handle == 0)
    {
        ctx.r3.u64 = wos::kStatusNoMemory;
        return;
    }

    if (handleOut != 0)
        wos::StoreU32(base, handleOut, handle);

    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif

#ifdef WOS_IMPL_NtSetEvent
PPC_FUNC(__imp__NtSetEvent)
{
    WOS_IMPORT_STUB("NtSetEvent");

    auto obj = wos::ObjectFromAny(ctx.r3.u32);
    if (auto* ev = dynamic_cast<wos::EventObject*>(obj.get()))
    {
        ev->Set();
        ctx.r3.u64 = wos::kStatusSuccess;
    }
    else
    {
        ctx.r3.u64 = wos::kStatusInvalidParameter;
    }
}
#endif

#ifdef WOS_IMPL_NtClearEvent
PPC_FUNC(__imp__NtClearEvent)
{
    WOS_IMPORT_STUB("NtClearEvent");

    auto obj = wos::ObjectFromAny(ctx.r3.u32);
    if (auto* ev = dynamic_cast<wos::EventObject*>(obj.get()))
    {
        ev->Clear();
        ctx.r3.u64 = wos::kStatusSuccess;
    }
    else
    {
        ctx.r3.u64 = wos::kStatusInvalidParameter;
    }
}
#endif

#ifdef WOS_IMPL_NtPulseEvent
PPC_FUNC(__imp__NtPulseEvent)
{
    WOS_IMPORT_STUB("NtPulseEvent");

    auto obj = wos::ObjectFromAny(ctx.r3.u32);
    if (auto* ev = dynamic_cast<wos::EventObject*>(obj.get()))
    {
        ev->Set();
        ev->Clear();
        ctx.r3.u64 = wos::kStatusSuccess;
    }
    else
    {
        ctx.r3.u64 = wos::kStatusInvalidParameter;
    }
}
#endif

#ifdef WOS_IMPL_NtCreateMutant
// NTSTATUS NtCreateMutant(PHANDLE, POBJECT_ATTRIBUTES, BOOLEAN InitialOwner)
PPC_FUNC(__imp__NtCreateMutant)
{
    WOS_IMPORT_STUB("NtCreateMutant");

    const uint32_t handleOut = ctx.r3.u32;
    const uint32_t initialOwner = ctx.r5.u32;

    auto mutant = std::make_shared<wos::MutantObject>();
    const uint32_t handle = wos::RegisterObject(base, mutant);
    if (handle == 0)
    {
        ctx.r3.u64 = wos::kStatusNoMemory;
        return;
    }

    if (initialOwner != 0)
        mutant->m.lock();

    if (handleOut != 0)
        wos::StoreU32(base, handleOut, handle);

    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif

#ifdef WOS_IMPL_NtReleaseMutant
PPC_FUNC(__imp__NtReleaseMutant)
{
    WOS_IMPORT_STUB("NtReleaseMutant");

    auto obj = wos::ObjectFromAny(ctx.r3.u32);
    if (auto* mutant = dynamic_cast<wos::MutantObject*>(obj.get()))
    {
        mutant->m.unlock();
        ctx.r3.u64 = wos::kStatusSuccess;
    }
    else
    {
        ctx.r3.u64 = wos::kStatusInvalidParameter;
    }
}
#endif

#ifdef WOS_IMPL_NtWaitForSingleObjectEx
// NTSTATUS NtWaitForSingleObjectEx(HANDLE, WaitMode, Alertable, PLARGE_INTEGER Timeout)
PPC_FUNC(__imp__NtWaitForSingleObjectEx)
{
    WOS_IMPORT_STUB("NtWaitForSingleObjectEx");

    const uint32_t callSite = uint32_t(ctx.lr);
    auto obj = wos::ObjectFromAny(ctx.r3.u32);
    const int64_t timeoutMs = TimeoutToMillis(base, ctx.r6.u32);

    if (auto* ev = dynamic_cast<wos::EventObject*>(obj.get()))
    {
        const bool signalled = ev->Wait(timeoutMs);
        wos::RecordWaitSite(callSite, ctx.r3.u32, timeoutMs, signalled);
        ctx.r3.u64 = signalled ? wos::kStatusSuccess : wos::kStatusTimeout;
        return;
    }

    if (auto* mutant = dynamic_cast<wos::MutantObject*>(obj.get()))
    {
        mutant->m.lock();
        ctx.r3.u64 = wos::kStatusSuccess;
        return;
    }

    // Unknown object: returning success rather than blocking forever keeps
    // bring-up moving. It is a lie, but a loud one — the object type is
    // printed so it shows up rather than hanging silently.
    printf("[sync] wait on unknown object 0x%08X — returning success\n", ctx.r3.u32);
    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif

#ifdef WOS_IMPL_NtClose
PPC_FUNC(__imp__NtClose)
{
    WOS_IMPORT_STUB("NtClose");
    wos::CloseHandle(ctx.r3.u32);
    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif

#ifdef WOS_IMPL_ObReferenceObjectByHandle
// NTSTATUS ObReferenceObjectByHandle(HANDLE, POBJECT_TYPE, PVOID* Object)
//
// The out-parameter is where the guest gets an object *pointer* from, which is
// exactly why objects carry a guest-visible block.
PPC_FUNC(__imp__ObReferenceObjectByHandle)
{
    WOS_IMPORT_STUB("ObReferenceObjectByHandle");

    const uint32_t handle = ctx.r3.u32;
    const uint32_t objectOut = ctx.r5.u32;
    auto obj = wos::ObjectFromAny(handle);

    // Create the backing object for a pseudo-handle on first reference. The
    // guest asks for "the current thread" long before it would ever have a
    // real handle for it, and it only wants something non-null to hold.
    if (obj == nullptr &&
        (handle == wos::kCurrentProcessHandle || handle == wos::kCurrentThreadHandle))
    {
        const uint32_t ptr = wos::RegisterPseudoHandle(base, handle,
            handle == wos::kCurrentProcessHandle ? "process" : "thread");
        if (ptr != 0)
            obj = wos::ObjectFromGuestPtr(ptr);
    }

    if (obj == nullptr)
    {
        printf("[obj] ObReferenceObjectByHandle: unknown handle 0x%08X\n", handle);
        ctx.r3.u64 = wos::kStatusInvalidHandle;
        return;
    }

    if (objectOut != 0)
        wos::StoreU32(base, objectOut, obj->guestPtr);

    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif

#ifdef WOS_IMPL_ObDereferenceObject
PPC_FUNC(__imp__ObDereferenceObject)
{
    WOS_IMPORT_STUB("ObDereferenceObject");
    // Reference counting is handled by shared_ptr on our side; the guest's
    // notion of a reference count is not something it can observe here.
    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif

// ---------------------------------------------------------------------------
// Kernel-mode wait and event calls (Ke*).
//
// These are the same objects as the Nt* family, reached a different way: Nt*
// takes a HANDLE, Ke* takes an object POINTER — the one
// ObReferenceObjectByHandle handed out. ObjectFromAny accepts both, so the
// implementations are shared.
//
// WHY THIS MATTERS: as stubs these returned success immediately, which turns
// a *blocking* wait into a busy-spin. The heartbeat caught it precisely —
// KeWaitForSingleObject and KeResetEvent called an exactly equal 24,801,146
// times per five seconds, five million loop iterations a second burning a
// core to no purpose, while the properly implemented NtWaitForSingleObjectEx
// sat at a healthy 254/sec.
//
// A stub that returns "success" to a wait is not merely wrong, it inverts the
// function's entire purpose.
// ---------------------------------------------------------------------------

namespace
{

// A wait that never completes is indistinguishable from a hang. Report the
// first time any single wait takes suspiciously long, so a deadlock names
// itself instead of just going quiet.
constexpr int64_t kLongWaitWarningMs = 5000;

// Find the event at a guest address, creating one if the game declared it
// inline in its own memory.
//
// Xbox 360 dispatcher objects do not have to come from NtCreateEvent: a game
// can embed a KEVENT in one of its own structures and initialise it in place,
// after which Ke* calls operate on that address directly. We never see such an
// object created, so a plain lookup fails.
//
// That is what stalled the previous run. KeWaitForSingleObject was reaching
// the unknown-object fallback -- a 1 ms sleep -- 128 times a second forever,
// while the events we *did* know about showed a single wait each. Creating the
// object on first touch means a later KeSetEvent on the same address wakes the
// same object, which is the entire point.
wos::EventObject* EmbeddedEvent(uint32_t guestPtr, const char* who)
{
    if (guestPtr == 0)
        return nullptr;

    auto existing = wos::ObjectFromAny(guestPtr);
    if (auto* ev = dynamic_cast<wos::EventObject*>(existing.get()))
        return ev;
    if (existing != nullptr)
        return nullptr;              // a real object of some other type

    auto ev = std::make_shared<wos::EventObject>();
    ev->type = "event(embedded)";
    // Manual-reset is the safer default: an auto-reset event we invented could
    // silently consume a signal a real waiter needed.
    ev->manualReset = true;

    {
        std::lock_guard<std::mutex> lock(wos::g_eventListMutex);
        ev->index = int(wos::g_allEvents.size());
        wos::g_allEvents.push_back(ev);
        printf("[sync] %s: adopting guest-embedded event at 0x%08X as ev%d\n",
            who, guestPtr, ev->index);
    }

    wos::RegisterObjectAt(guestPtr, ev);
    return ev.get();
}

bool WaitOnObject(uint32_t handleOrPtr, int64_t timeoutMs, const char* who)
{
    auto obj = wos::ObjectFromAny(handleOrPtr);

    if (auto* ev = dynamic_cast<wos::EventObject*>(obj.get()))
    {
        if (timeoutMs < 0)
        {
            // Split an infinite wait into a first bounded attempt so a
            // permanent block can be reported once, then keep waiting.
            if (ev->Wait(kLongWaitWarningMs))
                return true;

            // Naming the object was not enough: it says *what* is blocked but
            // not *who*. Each guest thread runs on its own host stack, so a
            // backtrace taken here names the recompiled functions that led
            // into this wait — which is the question that actually matters.
            {
                std::lock_guard<std::recursive_mutex> diag(wos::DiagnosticLock());
                printf("[sync] %s: still waiting on event 0x%08X after %llds. "
                       "Blocked in:\n",
                    who, handleOrPtr, (long long)(kLongWaitWarningMs / 1000));
                wos::PrintGuestStack(14);
            }

            return ev->Wait(-1);
        }
        return ev->Wait(timeoutMs);
    }

    if (auto* mutant = dynamic_cast<wos::MutantObject*>(obj.get()))
    {
        mutant->m.lock();
        return true;
    }

    // Not a handle we issued: treat it as a dispatcher object the guest
    // declared in its own memory, and adopt it.
    if (auto* ev = EmbeddedEvent(handleOrPtr, who))
        return ev->Wait(timeoutMs < 0 ? 100 : timeoutMs);

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return true;
}

} // namespace

#ifdef WOS_IMPL_KeWaitForSingleObject
// NTSTATUS KeWaitForSingleObject(PVOID Object,        // r3 — object pointer
//                                KWAIT_REASON,        // r4
//                                KPROCESSOR_MODE,     // r5
//                                BOOLEAN Alertable,   // r6
//                                PLARGE_INTEGER Timeout); // r7
PPC_FUNC(__imp__KeWaitForSingleObject)
{
    WOS_IMPORT_STUB("KeWaitForSingleObject");

    // Captured before the wait: ctx is the guest's live register file, and the
    // recompiled code the waking thread runs next will overwrite lr.
    const uint32_t callSite = uint32_t(ctx.lr);
    const uint32_t object = ctx.r3.u32;

    const int64_t timeoutMs = TimeoutToMillis(base, ctx.r7.u32);
    const bool signalled = WaitOnObject(object, timeoutMs, "KeWaitForSingleObject");
    wos::RecordWaitSite(callSite, object, timeoutMs, signalled);
    ctx.r3.u64 = signalled ? wos::kStatusSuccess : wos::kStatusTimeout;
}
#endif

#ifdef WOS_IMPL_KeSetEvent
// LONG KeSetEvent(PRKEVENT Event, KPRIORITY Increment, BOOLEAN Wait);
// Returns the event's PREVIOUS signalled state.
PPC_FUNC(__imp__KeSetEvent)
{
    WOS_IMPORT_STUB("KeSetEvent");

    auto obj = wos::ObjectFromAny(ctx.r3.u32);
    auto* ev = dynamic_cast<wos::EventObject*>(obj.get());
    if (ev == nullptr)
        ev = EmbeddedEvent(ctx.r3.u32, "KeSetEvent");

    if (ev != nullptr)
    {
        const uint32_t previous = ev->signalled ? 1u : 0u;
        ev->Set();
        ctx.r3.u64 = previous;
    }
    else
    {
        ctx.r3.u64 = 0;
    }
}
#endif

#ifdef WOS_IMPL_KeResetEvent
// LONG KeResetEvent(PRKEVENT Event);  — returns the previous state.
PPC_FUNC(__imp__KeResetEvent)
{
    WOS_IMPORT_STUB("KeResetEvent");

    auto obj = wos::ObjectFromAny(ctx.r3.u32);
    auto* ev = dynamic_cast<wos::EventObject*>(obj.get());
    if (ev == nullptr)
        ev = EmbeddedEvent(ctx.r3.u32, "KeResetEvent");

    if (ev != nullptr)
    {
        const uint32_t previous = ev->signalled ? 1u : 0u;
        ev->Clear();
        ctx.r3.u64 = previous;
    }
    else
    {
        ctx.r3.u64 = 0;
    }
}
#endif

#ifdef WOS_IMPL_KePulseEvent
PPC_FUNC(__imp__KePulseEvent)
{
    WOS_IMPORT_STUB("KePulseEvent");

    auto obj = wos::ObjectFromAny(ctx.r3.u32);
    if (auto* ev = dynamic_cast<wos::EventObject*>(obj.get()))
    {
        const uint32_t previous = ev->signalled ? 1u : 0u;
        ev->Set();
        ev->Clear();
        ctx.r3.u64 = previous;
    }
    else
    {
        ctx.r3.u64 = 0;
    }
}
#endif

#ifdef WOS_IMPL_KeWaitForMultipleObjects
// Waits on the first object only, then reports success.
//
// STATED SHORTCUT: a correct implementation needs WaitAll/WaitAny semantics
// across a whole array. Waiting on one of them at least blocks rather than
// spinning, which is the failure that actually mattered here; if the game
// starts behaving as though the wrong object woke it, this is the first place
// to look.
PPC_FUNC(__imp__KeWaitForMultipleObjects)
{
    WOS_IMPORT_STUB("KeWaitForMultipleObjects");

    const uint32_t count = ctx.r3.u32;
    const uint32_t objectArray = ctx.r4.u32;

    if (count == 0 || objectArray == 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ctx.r3.u64 = wos::kStatusSuccess;
        return;
    }

    const uint32_t callSite = uint32_t(ctx.lr);
    const uint32_t first = wos::LoadU32(base, objectArray);
    const bool signalled = WaitOnObject(first, 16, "KeWaitForMultipleObjects");
    wos::RecordWaitSite(callSite, first, 16, signalled);
    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif

#ifdef WOS_IMPL_NtWaitForMultipleObjectsEx
// NTSTATUS NtWaitForMultipleObjectsEx(ULONG count, HANDLE* handles,
//     WAIT_TYPE waitType, KPROCESSOR_MODE mode, BOOLEAN alertable,
//     PLARGE_INTEGER timeout);
//   r3 = count, r4 = handle array, r5 = waitType (0 = WaitAll, 1 = WaitAny),
//   r6 = mode, r7 = alertable, r8 = timeout
//
// This was an unimplemented stub, so it returned success instantly — the same
// failure that has bitten this project more than once: a stub that returns
// success to a *wait* inverts the call's timing semantics and turns a block
// into a busy-spin. A run measured it at 11,720,555 calls in five seconds,
// lockstep with NtSetEvent and NtReleaseMutant, i.e. one loop spinning about
// 4.7 million times a second and doing no work.
//
// WaitAll waits on every object in turn; WaitAny waits on the first. Waiting
// on the first is a stated shortcut, matching KeWaitForMultipleObjects above:
// it blocks, which is the property that actually matters, but a game that
// expects to be woken by the *second* handle will wake late. If something
// starts behaving as though the wrong object signalled it, look here first.
PPC_FUNC(__imp__NtWaitForMultipleObjectsEx)
{
    WOS_IMPORT_STUB("NtWaitForMultipleObjectsEx");

    const uint32_t callSite = uint32_t(ctx.lr);
    const uint32_t count = ctx.r3.u32;
    const uint32_t handleArray = ctx.r4.u32;
    const uint32_t waitType = ctx.r5.u32;
    const int64_t timeoutMs = TimeoutToMillis(base, ctx.r8.u32);

    if (count == 0 || handleArray == 0)
    {
        // Nothing to wait on. Sleep briefly rather than returning instantly,
        // so a caller that loops on this cannot spin a core.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ctx.r3.u64 = wos::kStatusSuccess;
        return;
    }

    // Bound the count: a garbage value here would otherwise walk guest memory.
    const uint32_t bounded = (count > 64) ? 64 : count;

    if (waitType == 0)
    {
        // WaitAll: every object has to be signalled.
        for (uint32_t i = 0; i < bounded; ++i)
        {
            const uint32_t handle = wos::LoadU32(base, handleArray + i * 4);
            const bool signalled =
                WaitOnObject(handle, timeoutMs, "NtWaitForMultipleObjectsEx");
            wos::RecordWaitSite(callSite, handle, timeoutMs, signalled);
        }
    }
    else
    {
        const uint32_t handle = wos::LoadU32(base, handleArray);
        const bool signalled =
            WaitOnObject(handle, timeoutMs, "NtWaitForMultipleObjectsEx");
        wos::RecordWaitSite(callSite, handle, timeoutMs, signalled);
    }

    // The return value for WaitAny is the index that signalled. We waited on
    // the first, so report that rather than inventing an index we did not
    // observe.
    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif
