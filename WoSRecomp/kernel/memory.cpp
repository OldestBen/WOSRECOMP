// Guest virtual memory.
//
// This is the first thing Web of Shadows asks the kernel for, and until now it
// got nothing back: the stub logged the call and returned, leaving r3 holding
// whatever the caller happened to leave there. Everything downstream — the
// bugcheck cascade, the reads of guest 0x14 and 0xFFFFFFFD — follows from
// that.
//
// The allocator is a bump allocator. Freeing is accepted and ignored. That is
// enough to boot and not enough to run a game for long; it is deliberately
// the simplest thing that lets us see what the game asks for next.

#include "ppc_recomp_shared.h"
#include "import_log.h"
#include "kernel_overrides.h"
#include "guest.h"

#include <cstdio>
#include <mutex>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <sys/mman.h>
#endif

namespace
{

std::mutex g_mutex;

// Xbox 360 user address space. 64 KiB-page allocations live from 0x40000000
// upward; below that is the 4 KiB-page region. Staying in the 64 KiB region
// keeps everything aligned with the granularity the harness commits at.
constexpr uint32_t kHeapBase = 0x40000000u;
constexpr uint32_t kHeapEnd  = 0x7F000000u;

uint32_t g_next = kHeapBase;
uint64_t g_totalAllocated = 0;
uint64_t g_allocCount = 0;

} // namespace

namespace wos
{

bool GuestCommit(uint8_t* base, uint32_t addr, uint32_t size)
{
#ifdef _WIN32
    return VirtualAlloc(base + addr, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
    return mprotect(base + addr, size, PROT_READ | PROT_WRITE) == 0;
#endif
}

uint32_t GuestAlloc(uint8_t* base, uint32_t requestedBase, uint32_t size, uint32_t alignment)
{
    if (size == 0)
        return 0;

    if (alignment < 0x1000)
        alignment = 0x1000;

    std::lock_guard<std::mutex> lock(g_mutex);

    uint32_t addr;
    if (requestedBase != 0)
    {
        // The guest asked for a specific address. Honour it — the harness has
        // the whole 32-bit space reserved, so any address can be committed.
        addr = requestedBase & ~(alignment - 1);
    }
    else
    {
        addr = (g_next + alignment - 1) & ~(alignment - 1);

        const uint64_t end = uint64_t(addr) + size;
        if (end > kHeapEnd)
        {
            printf("[mem] OUT OF GUEST ADDRESS SPACE: wanted 0x%X bytes, bump "
                   "pointer at 0x%08X, limit 0x%08X\n", size, g_next, kHeapEnd);
            return 0;
        }

        g_next = uint32_t(end);
    }

    // Round the commit out to the harness's 64 KiB granularity so partial
    // regions don't fight with the on-demand committer.
    const uint32_t commitStart = addr & ~0xFFFFu;
    const uint64_t commitEnd = (uint64_t(addr) + size + 0xFFFF) & ~uint64_t(0xFFFF);

    if (!GuestCommit(base, commitStart, uint32_t(commitEnd - commitStart)))
    {
        printf("[mem] commit failed for guest 0x%08X (0x%X bytes)\n", addr, size);
        return 0;
    }

    g_totalAllocated += size;
    ++g_allocCount;

    return addr;
}

} // namespace wos

#ifdef WOS_IMPL_NtAllocateVirtualMemory
// NTSTATUS NtAllocateVirtualMemory(
//     PVOID*  BaseAddress,     // r3 — in/out, guest pointer to a guest pointer
//     SIZE_T* RegionSize,      // r4 — in/out, guest pointer to a size
//     ULONG   AllocationType,  // r5
//     ULONG   Protect,         // r6
//     ULONG   Unknown);        // r7
//
// Note there is no ProcessHandle: the Xbox 360 form differs from desktop NT.
PPC_FUNC(__imp__NtAllocateVirtualMemory)
{
    WOS_IMPORT_STUB("NtAllocateVirtualMemory");

    const uint32_t baseAddressPtr = ctx.r3.u32;
    const uint32_t regionSizePtr = ctx.r4.u32;
    const uint32_t allocationType = ctx.r5.u32;

    if (baseAddressPtr == 0 || regionSizePtr == 0)
    {
        ctx.r3.u64 = wos::kStatusInvalidParameter;
        return;
    }

    const uint32_t requested = wos::LoadU32(base, baseAddressPtr);
    const uint32_t size = wos::LoadU32(base, regionSizePtr);

    // MEM_LARGE_PAGES (0x20000000) means 64 KiB pages on this hardware.
    const uint32_t alignment = (allocationType & 0x20000000u) ? 0x10000u : 0x1000u;

    const uint32_t addr = wos::GuestAlloc(base, requested, size, alignment);

    if (addr == 0)
    {
        printf("[mem] NtAllocateVirtualMemory FAILED: requested base 0x%08X, "
               "size 0x%X, type 0x%08X\n", requested, size, allocationType);
        ctx.r3.u64 = wos::kStatusNoMemory;
        return;
    }

    const uint32_t granted = (uint32_t(size) + alignment - 1) & ~(alignment - 1);

    wos::StoreU32(base, baseAddressPtr, addr);
    wos::StoreU32(base, regionSizePtr, granted);

    printf("[mem] alloc guest 0x%08X, 0x%X bytes (asked 0x%X at 0x%08X, type 0x%08X)\n",
        addr, granted, size, requested, allocationType);

    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif

#ifdef WOS_IMPL_NtFreeVirtualMemory
// Accepted and ignored: a bump allocator cannot reuse anything, and pretending
// to fail would be worse than leaking. Revisit when the game runs long enough
// for the leak to matter.
PPC_FUNC(__imp__NtFreeVirtualMemory)
{
    WOS_IMPORT_STUB("NtFreeVirtualMemory");
    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif
