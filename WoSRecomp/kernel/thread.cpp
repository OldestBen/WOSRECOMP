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

#include <cstdio>
#include <map>
#include <mutex>
// (std::recursive_mutex lives in <mutex>)

namespace
{

// --- TLS ------------------------------------------------------------------
//
// Slot values are per-thread; the slot *allocation* is global. thread_local
// storage on the host gives us the per-thread part for free.
constexpr size_t kMaxTlsSlots = 128;

std::mutex g_tlsMutex;
bool g_tlsSlotUsed[kMaxTlsSlots] = {};

thread_local uint32_t t_tlsValues[kMaxTlsSlots] = {};

// --- Critical sections ----------------------------------------------------

std::mutex g_csMapMutex;
std::map<uint32_t, std::recursive_mutex> g_criticalSections;

std::recursive_mutex& CriticalSectionFor(uint32_t guestAddr)
{
    std::lock_guard<std::mutex> lock(g_csMapMutex);
    // std::map never invalidates references to existing elements, which is why
    // it is used here rather than unordered_map — a rehash would move the
    // mutex out from under a thread that is currently blocked on it.
    return g_criticalSections[guestAddr];
}

} // namespace

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
    CriticalSectionFor(ctx.r3.u32).lock();
}
#endif

#ifdef WOS_IMPL_RtlLeaveCriticalSection
PPC_FUNC(__imp__RtlLeaveCriticalSection)
{
    WOS_IMPORT_STUB("RtlLeaveCriticalSection");
    CriticalSectionFor(ctx.r3.u32).unlock();
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
