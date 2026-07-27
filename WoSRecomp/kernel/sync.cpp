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

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>

namespace wos
{

struct EventObject : KernelObject
{
    std::mutex m;
    std::condition_variable cv;
    bool signalled = false;
    bool manualReset = false;

    EventObject() { type = "event"; }

    void Set()
    {
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
        std::unique_lock<std::mutex> lock(m);

        auto ready = [this] { return signalled; };

        if (timeoutMs < 0)
            cv.wait(lock, ready);
        else if (!cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready))
            return false;

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

    auto obj = wos::ObjectFromAny(ctx.r3.u32);
    const int64_t timeoutMs = TimeoutToMillis(base, ctx.r6.u32);

    if (auto* ev = dynamic_cast<wos::EventObject*>(obj.get()))
    {
        ctx.r3.u64 = ev->Wait(timeoutMs) ? wos::kStatusSuccess : wos::kStatusTimeout;
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

    const uint32_t objectOut = ctx.r5.u32;
    auto obj = wos::ObjectFromAny(ctx.r3.u32);

    if (obj == nullptr)
    {
        printf("[obj] ObReferenceObjectByHandle: unknown handle 0x%08X\n", ctx.r3.u32);
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
