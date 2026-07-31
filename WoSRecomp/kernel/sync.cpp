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

// A wake channel shared by every event, so a wait spanning several objects can
// be woken by whichever one signals.
//
// Each EventObject has its own condition variable, which is exactly right for
// waiting on that object and useless for waiting on a set of them: there is no
// way to block on several condition variables at once. Rather than give every
// wait a bespoke shared state, every Set() bumps one global generation counter
// and notifies one global variable. A WaitAny sleeps on that, wakes on any
// signal anywhere, and re-checks its own objects.
//
// The cost is that an unrelated event's signal wakes a WaitAny spuriously.
// That is a re-check of a handful of booleans, and signals run in the low
// thousands per second, so it is not a rate worth engineering around.
std::mutex g_anySignalMutex;
std::condition_variable g_anySignalCv;
uint64_t g_anySignalGeneration = 0;

void NotifyAnyWaiters()
{
    {
        std::lock_guard<std::mutex> lock(g_anySignalMutex);
        ++g_anySignalGeneration;
    }
    g_anySignalCv.notify_all();
}

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

    // Guest call sites for the object's lifecycle. An event that is waited on
    // and never set is only half a diagnosis — the other half is who was
    // supposed to set it, and the creation site is the thread that owns the
    // protocol. Both are ctx.lr at the relevant call, so they cost nothing.
    uint32_t createSite = 0;
    std::atomic<uint32_t> lastSetSite{0};

    EventObject() { type = "event"; }

    void Set(uint32_t site = 0)
    {
        signals.fetch_add(1, std::memory_order_relaxed);
        if (site != 0)
            lastSetSite.store(site, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(m);
            signalled = true;
        }
        // notify_all even for auto-reset: exactly one waiter will consume the
        // signal (they re-check `signalled` under the lock), the rest go back
        // to sleep. notify_one would be enough but is easier to get subtly
        // wrong when a waiter times out between the notify and the wake.
        cv.notify_all();
        NotifyAnyWaiters();
    }

    // Take the signal if there is one, without blocking. Used by WaitAny,
    // which cannot simply wait on one object's condition variable because it
    // has to be woken by whichever of several objects signals first.
    bool TryConsume()
    {
        std::lock_guard<std::mutex> lock(m);
        if (!signalled)
            return false;
        if (!manualReset)
            signalled = false;
        return true;
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
    struct Row { int index; uint32_t handle; uint32_t ptr; uint64_t waits, timeouts, signals;
                 bool manual; uint32_t createSite; uint32_t setSite; };
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
                rows.push_back({ ev->index, ev->handle, ev->guestPtr, w,
                                 ev->timeouts.load(std::memory_order_relaxed),
                                 ev->signals.load(std::memory_order_relaxed),
                                 ev->manualReset, ev->createSite,
                                 ev->lastSetSite.load(std::memory_order_relaxed) });
            }
        }
    }

    if (rows.empty())
    {
        ReportWaitSites();
        return;
    }

    std::sort(rows.begin(), rows.end(),
        [](const Row& a, const Row& b) { return a.waits > b.waits; });

    // Every event that has been waited on, not the busiest handful. The ones
    // that matter are precisely the quiet ones — an event waited on once and
    // never signalled is a deadlock, and it sorts last by every measure that
    // was being used to truncate this list.
    printf("[waits] %zu event(s) waited on  [waits/timeouts/signals]:\n", rows.size());
    for (const auto& r : rows)
    {
        printf("    ev%-2d handle 0x%08X ptr 0x%08X %-7s %llu/%llu/%llu",
            r.index, r.handle, r.ptr, r.manual ? "manual" : "auto",
            (unsigned long long)r.waits, (unsigned long long)r.timeouts,
            (unsigned long long)r.signals);
        if (r.createSite != 0)
            printf("  created bl@0x%08X", r.createSite - 4);
        if (r.setSite != 0)
            printf("  last set bl@0x%08X", r.setSite - 4);
        else if (r.signals == 0)
            printf("  NEVER SET BY ANYONE");
        printf("\n");
    }

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

// Deliver pending APCs, and if any ran on an ALERTABLE wait, return
// STATUS_USER_APC instead of blocking.
//
// This is the half of APC semantics we were missing, and it may be the whole
// missing completion mechanism.
//
// An alertable wait has two ways to end: the object signals, or an APC is
// delivered. In the second case the wait does NOT go on to block — it returns
// STATUS_USER_APC (0xC0) so the caller can look at whatever the APC changed and
// decide what to do next. That is the entire point of alertability; a caller
// that did not want to be interrupted would pass Alertable = FALSE.
//
// We were delivering the APC and then blocking anyway, which silently deletes
// the notification. The observed behaviour matches exactly: the loader issues
// the read, the APC runs and moves the file request to state 2, and the loader
// — never told anything happened — carries on waiting on events that only the
// completion it was supposed to run would signal.
//
// Returns true if the caller should return immediately, with r3 already set.
bool AlertableReturn(PPCContext& ctx, uint8_t* base, uint32_t alertable, const char* who)
{
    if (!wos::DeliverPendingApcs(ctx, base))
        return false;
    if (alertable == 0)
        return false;

    // Loud for the first few, because if this fires and nothing improves, the
    // next question is whether the guest's wrapper handles 0xC0 at all — and
    // that is only answerable if we know it was returned.
    static std::atomic<uint64_t> s_count{0};
    if (const uint64_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1; n <= 8)
        printf("[sync] %s: APC delivered on an alertable wait -> STATUS_USER_APC "
               "(#%llu, guest 0x%08X)\n",
            who, (unsigned long long)n, uint32_t(ctx.lr) - 4);

    ctx.r3.u64 = wos::kStatusUserApc;
    return true;
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
    ev->createSite = uint32_t(ctx.lr);

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
        ev->Set(uint32_t(ctx.lr));
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

    // A pending completion APC runs before the thread blocks — this is the
    // alertable-wait boundary the real kernel delivers them at. And once one
    // HAS run, an alertable wait does not go on to block: it returns
    // STATUS_USER_APC so the caller can re-examine whatever the APC changed.
    // See AlertableReturn below for why that matters here.
    if (AlertableReturn(ctx, base, ctx.r5.u32, "NtWaitForSingleObjectEx"))
        return;

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

    // A thread handle is waitable: it means "block until this thread exits".
    // This used to reach the unknown-object path below and return success
    // immediately, which told the caller a thread had exited while it was
    // still starting up.
    if (const int r = wos::WaitForThreadExit(ctx.r3.u32, timeoutMs); r >= 0)
    {
        wos::RecordWaitSite(callSite, ctx.r3.u32, timeoutMs, r == 1);
        ctx.r3.u64 = (r == 1) ? wos::kStatusSuccess : wos::kStatusTimeout;
        return;
    }

    // Unknown object: returning success rather than blocking forever keeps
    // bring-up moving. It is a lie, but a loud one — the object type is
    // printed so it shows up rather than hanging silently.
    printf("[sync] wait on unknown object 0x%08X — returning success "
           "(called from guest 0x%08X)\n", ctx.r3.u32, callSite - 4);
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

// Wait until ANY of the objects is signalled, or the timeout expires.
// Returns the index that satisfied the wait, or -1 on timeout.
//
// This replaces a stated shortcut that waited on the first object only. The
// comment on it said "if something starts behaving as though the wrong object
// signalled it, look here first", and that is exactly what happened: two loader
// threads sat in NtWaitForMultipleObjectsEx for entire runs while the events
// around them were signalled sixteen hundred times. Waiting on the first handle
// of a WaitAny is not a lateness bug, it is a permanent block whenever the
// signalling object is not the one at index zero.
//
// Correct within the model we have: consume-if-signalled across the whole set,
// then sleep on the shared wake channel until any event anywhere signals, then
// re-check. The generation counter closes the race between the check and the
// sleep — a Set() landing in that window bumps it, so the wait returns at once
// rather than missing the wake.
int WaitAnyOf(uint8_t* base, uint32_t handleArray, uint32_t count, int64_t timeoutMs,
              const char* who)
{
    std::vector<wos::EventObject*> events(count, nullptr);
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t h = wos::LoadU32(base, handleArray + i * 4);
        auto obj = wos::ObjectFromAny(h);
        auto* ev = dynamic_cast<wos::EventObject*>(obj.get());
        if (ev == nullptr)
            ev = EmbeddedEvent(h, who);
        events[i] = ev;
        if (ev != nullptr)
            ev->waits.fetch_add(1, std::memory_order_relaxed);
    }

    const auto started = std::chrono::steady_clock::now();
    const bool infinite = (timeoutMs < 0);
    const auto deadline = started + std::chrono::milliseconds(infinite ? 0 : timeoutMs);
    bool reported = false;

    // One loop for both cases rather than a branch per timeout kind.
    //
    // The previous shape put the "still blocked after 5s" report inside the
    // INFINITE branch only, so a wait with a large *finite* timeout — hours,
    // which is what these callers actually pass — sat there silently and could
    // not be seen at all. Two threads did exactly that for entire runs. The
    // report now depends on elapsed time, not on which kind of wait it is,
    // which is the property that was wanted in the first place.
    for (;;)
    {
        uint64_t generation;
        {
            std::lock_guard<std::mutex> lock(wos::g_anySignalMutex);
            generation = wos::g_anySignalGeneration;
        }

        for (uint32_t i = 0; i < count; ++i)
            if (events[i] != nullptr && events[i]->TryConsume())
                return int(i);

        const auto now = std::chrono::steady_clock::now();
        if (!infinite && now >= deadline)
        {
            for (uint32_t i = 0; i < count; ++i)
                if (events[i] != nullptr)
                    events[i]->timeouts.fetch_add(1, std::memory_order_relaxed);
            return -1;
        }

        if (!reported && now - started >= std::chrono::seconds(5))
        {
            reported = true;
            std::lock_guard<std::recursive_mutex> diag(wos::DiagnosticLock());
            printf("[sync] %s: none of %u object(s) signalled in 5s (timeout %s).\n",
                who, count, infinite ? "INFINITE" : "finite");
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t h = wos::LoadU32(base, handleArray + i * 4);
                printf("[sync]   [%u] handle 0x%08X -> %s\n", i, h,
                    events[i] != nullptr ? events[i]->type : "NOT AN EVENT");
            }
            printf("[sync]   blocked in:\n");
            wos::PrintGuestStack(12);
        }

        // Wake at least every second even with nothing to do, so the report
        // above cannot be starved by a quiet period.
        const auto wakeBy = now + std::chrono::seconds(1);
        std::unique_lock<std::mutex> lock(wos::g_anySignalMutex);
        wos::g_anySignalCv.wait_until(lock,
            (infinite || deadline > wakeBy) ? wakeBy : deadline,
            [&] { return wos::g_anySignalGeneration != generation; });
    }
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

    if (AlertableReturn(ctx, base, ctx.r6.u32, "KeWaitForSingleObject"))
        return;

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
        ev->Set(uint32_t(ctx.lr));
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

    if (AlertableReturn(ctx, base, ctx.r8.u32, "KeWaitForMultipleObjects"))
        return;

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

    if (AlertableReturn(ctx, base, ctx.r7.u32, "NtWaitForMultipleObjectsEx"))
        return;

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
        ctx.r3.u64 = wos::kStatusSuccess;
        return;
    }

    const int index = WaitAnyOf(base, handleArray, bounded, timeoutMs,
        "NtWaitForMultipleObjectsEx");

    // Attribute to the object that actually satisfied the wait, not to the
    // first one — the census is what would show a recurrence of the old bug.
    const uint32_t satisfied = wos::LoadU32(base,
        handleArray + (index >= 0 ? uint32_t(index) : 0u) * 4);
    wos::RecordWaitSite(callSite, satisfied, timeoutMs, index >= 0);

    // WAIT_OBJECT_0 + index is the documented return for WaitAny, and the
    // caller uses it to tell which handle woke it. Returning 0 unconditionally
    // — as this did — tells every caller it was the first one.
    ctx.r3.u64 = (index >= 0) ? uint32_t(index) : wos::kStatusTimeout;
}
#endif
