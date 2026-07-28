// Physical memory, memory statistics, and system clocks.
//
// Every function here was previously a stub that returned zero. Four of them
// take out-parameters and filled in nothing, which is the same failure that
// broke NtQueryInformationFile: the caller reads uninitialised guest memory
// as the answer and has no way to tell it was never written.
//
// The physical ones matter for a second reason. The Xbox 360 aliases physical
// RAM into the 0xA0000000-0xBFFFFFFF range, and the last run read
// 0xAD000010, 0xAE010000 and 0x59000000 — addresses no allocation ever
// returned, because MmAllocatePhysicalMemoryEx handed back NULL every time.

#include "ppc_recomp_shared.h"
#include "import_log.h"
#include "kernel_overrides.h"
#include "guest.h"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <vector>
#include <thread>

namespace
{

// The console has 512 MiB of unified memory, 4 KiB pages.
constexpr uint32_t kPageSize = 0x1000;
constexpr uint64_t kTotalPhysicalBytes = 512ull << 20;
constexpr uint32_t kTotalPhysicalPages = uint32_t(kTotalPhysicalBytes / kPageSize);

// Physical allocations are handed out from the uncached alias window. Keeping
// them well away from the 0x40000000 virtual heap means a stray pointer from
// one region can never be mistaken for the other while reading a log.
constexpr uint32_t kPhysicalBase = 0xA0000000u;
// The console's uncached physical alias is 0xA0000000..0xBFFFFFFF — the full
// 512 MiB of unified memory. Stopping at 0xB0000000 modelled a 256 MiB machine
// and refused a 0x12160000 (290 MiB) request during archive loading, which the
// game handled by falling back to a smaller allocation rather than failing
// loudly. That is the kind of shortfall that surfaces much later as missing
// content, so size the window to the hardware.
constexpr uint32_t kPhysicalEnd  = 0xC0000000u;

std::mutex g_physMutex;
uint32_t g_physNext = kPhysicalBase;
uint64_t g_physAllocated = 0;

// Every physical block handed out, so host code can ask how far it may safely
// read from a guest pointer it was given.
//
// This exists because of the ring buffer. Its size argument has an encoding we
// have not confirmed, and the recovery for "the write pointer went past our
// modelled capacity" is to grow the capacity — which, unbounded, walks the
// command processor out of the ring and into whatever was allocated next.
// Trading a visible hang for silent memory corruption is the wrong trade, and
// the allocation edge is a bound we actually know.
struct PhysBlock { uint32_t begin; uint32_t end; };
std::vector<PhysBlock> g_physBlocks;

// The 360's timebase runs at 50 MHz. Games divide by this, so returning the
// stub's zero risks a divide-by-zero rather than merely a wrong timestamp.
constexpr uint64_t kTimebaseFrequency = 50000000ull;

// Windows epoch (1601) to Unix epoch (1970), in 100 ns ticks.
constexpr uint64_t kUnixToWindowsEpoch = 116444736000000000ull;

uint64_t SystemTime100ns()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    return kUnixToWindowsEpoch + uint64_t(ns) / 100;
}

} // namespace

#ifdef WOS_IMPL_MmAllocatePhysicalMemoryEx
// PVOID MmAllocatePhysicalMemoryEx(DWORD flags,        // r3
//                                  DWORD regionSize,   // r4
//                                  DWORD protect,      // r5
//                                  DWORD minAddress,   // r6
//                                  DWORD maxAddress,   // r7
//                                  DWORD alignment);   // r8
//
// Returns a guest pointer, not an NTSTATUS. Returning 0 means "out of memory",
// which is what the stub was saying four times per run.
PPC_FUNC(__imp__MmAllocatePhysicalMemoryEx)
{
    WOS_IMPORT_STUB("MmAllocatePhysicalMemoryEx");

    const uint32_t size = ctx.r4.u32;
    uint32_t alignment = ctx.r8.u32;

    if (size == 0)
    {
        ctx.r3.u64 = 0;
        return;
    }

    if (alignment < kPageSize)
        alignment = kPageSize;

    std::lock_guard<std::mutex> lock(g_physMutex);

    const uint32_t addr = (g_physNext + alignment - 1) & ~(alignment - 1);
    const uint64_t end = uint64_t(addr) + size;

    if (end > kPhysicalEnd)
    {
        printf("[phys] OUT OF PHYSICAL MEMORY: wanted 0x%X bytes at 0x%08X, limit 0x%08X\n",
            size, addr, kPhysicalEnd);
        ctx.r3.u64 = 0;
        return;
    }

    // Commit rounded out to the harness's 64 KiB granularity.
    const uint32_t commitStart = addr & ~0xFFFFu;
    const uint64_t commitEnd = (end + 0xFFFF) & ~uint64_t(0xFFFF);

    if (!wos::GuestCommit(base, commitStart, uint32_t(commitEnd - commitStart)))
    {
        printf("[phys] commit failed at guest 0x%08X (0x%X bytes)\n", addr, size);
        ctx.r3.u64 = 0;
        return;
    }

    g_physNext = uint32_t(end);
    g_physAllocated += size;
    g_physBlocks.push_back({ addr, uint32_t(end) });

    printf("[phys] alloc guest 0x%08X, 0x%X bytes (align 0x%X, flags 0x%X)\n",
        addr, size, alignment, ctx.r3.u32);

    ctx.r3.u64 = addr;
}
#endif

#ifdef WOS_IMPL_MmFreePhysicalMemory
PPC_FUNC(__imp__MmFreePhysicalMemory)
{
    WOS_IMPORT_STUB("MmFreePhysicalMemory");
    // Bump allocator: nothing to reclaim. Leaking is preferable to handing the
    // same address out twice.
    ctx.r3.u64 = 0;
}
#endif

#ifdef WOS_IMPL_MmGetPhysicalAddress
// The guest passes these to the GPU. With no GPU, all that matters is that the
// mapping is consistent and reversible: strip the alias window, keep the
// offset.
PPC_FUNC(__imp__MmGetPhysicalAddress)
{
    WOS_IMPORT_STUB("MmGetPhysicalAddress");
    ctx.r3.u64 = ctx.r3.u32 & 0x1FFFFFFFu;
}
#endif

#ifdef WOS_IMPL_MmQueryStatistics
// NTSTATUS MmQueryStatistics(PMM_STATISTICS stats);   // r3
//
// MM_STATISTICS is 0x68 bytes and begins with its own Length, which the caller
// fills in and the kernel validates. Writing nothing — as the stub did — left
// the game reading whatever happened to be in that buffer.
PPC_FUNC(__imp__MmQueryStatistics)
{
    WOS_IMPORT_STUB("MmQueryStatistics");

    const uint32_t stats = ctx.r3.u32;
    if (stats == 0)
    {
        ctx.r3.u64 = wos::kStatusInvalidParameter;
        return;
    }

    const uint32_t length = wos::LoadU32(base, stats + 0x00);
    if (length < 0x68)
    {
        printf("[mem] MmQueryStatistics: caller says Length=%u, expected >= 0x68\n", length);
        ctx.r3.u64 = 0xC0000004u;   // STATUS_INFO_LENGTH_MISMATCH
        return;
    }

    // Report the title as owning most of the machine, with a healthy amount
    // free. These are plausible rather than measured: the numbers the game
    // actually cares about are "how much is left", and being generous keeps it
    // from deciding it cannot proceed.
    const uint32_t titleTotal = kTotalPhysicalPages * 3 / 4;
    const uint32_t titleUsed = uint32_t((g_physAllocated + kPageSize - 1) / kPageSize);
    const uint32_t titleAvailable = titleTotal > titleUsed ? titleTotal - titleUsed : 0;

    wos::StoreU32(base, stats + 0x04, kTotalPhysicalPages);          // TotalPhysicalPages
    wos::StoreU32(base, stats + 0x08, kTotalPhysicalPages / 16);     // KernelPages
    wos::StoreU32(base, stats + 0x0C, titleAvailable);               // TitleTotalAvailablePages
    wos::StoreU32(base, stats + 0x10, uint32_t(titleTotal) * kPageSize); // TitleTotalVirtualMemoryBytes
    wos::StoreU32(base, stats + 0x14, 0);                            // TitleReservedVirtualMemoryBytes
    wos::StoreU32(base, stats + 0x18, titleUsed);                    // TitlePhysicalPages
    wos::StoreU32(base, stats + 0x1C, 0);                            // TitlePoolPages
    wos::StoreU32(base, stats + 0x20, 16);                           // TitleStackPages
    wos::StoreU32(base, stats + 0x24, 0x1000 / 16);                  // TitleImagePages
    wos::StoreU32(base, stats + 0x28, titleUsed);                    // TitleHeapPages
    wos::StoreU32(base, stats + 0x2C, titleUsed);                    // TitleVirtualPages
    wos::StoreU32(base, stats + 0x30, 0);                            // TitlePageTablePages
    wos::StoreU32(base, stats + 0x34, 0);                            // TitleCachePages

    wos::StoreU32(base, stats + 0x38, kTotalPhysicalPages / 4);      // SystemTotalAvailablePages
    wos::StoreU32(base, stats + 0x3C, (kTotalPhysicalPages / 4) * kPageSize);
    wos::StoreU32(base, stats + 0x40, 0);
    wos::StoreU32(base, stats + 0x44, kTotalPhysicalPages / 8);      // SystemPhysicalPages
    wos::StoreU32(base, stats + 0x48, 0);
    wos::StoreU32(base, stats + 0x4C, 0);
    wos::StoreU32(base, stats + 0x50, 0);
    wos::StoreU32(base, stats + 0x54, 0);
    wos::StoreU32(base, stats + 0x58, 0);
    wos::StoreU32(base, stats + 0x5C, 0);
    wos::StoreU32(base, stats + 0x60, 0);
    wos::StoreU32(base, stats + 0x64, kTotalPhysicalPages - 1);      // HighestPhysicalPage

    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif

#ifdef WOS_IMPL_KeQuerySystemTime
// VOID KeQuerySystemTime(PLARGE_INTEGER CurrentTime);   // r3
//
// Another out-parameter the stub never wrote. A game reading an uninitialised
// timestamp can compute a wildly negative delta, which tends to surface as
// something far away from here.
PPC_FUNC(__imp__KeQuerySystemTime)
{
    WOS_IMPORT_STUB("KeQuerySystemTime");
    if (ctx.r3.u32 != 0)
        wos::StoreU64(base, ctx.r3.u32, SystemTime100ns());
}
#endif

#ifdef WOS_IMPL_KeQueryPerformanceFrequency
PPC_FUNC(__imp__KeQueryPerformanceFrequency)
{
    WOS_IMPORT_STUB("KeQueryPerformanceFrequency");
    // Returned in r3 as a 64-bit value. Zero here is a divisor of zero in the
    // caller, which is a considerably worse outcome than a wrong clock rate.
    ctx.r3.u64 = kTimebaseFrequency;
}
#endif

#ifdef WOS_IMPL_KeGetCurrentProcessType
PPC_FUNC(__imp__KeGetCurrentProcessType)
{
    WOS_IMPORT_STUB("KeGetCurrentProcessType");
    // 0 = idle, 1 = user (title), 2 = system. We are the title.
    ctx.r3.u64 = 1;
}
#endif

#ifdef WOS_IMPL_KeDelayExecutionThread
// NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE mode, BOOLEAN alertable,
//                                 PLARGE_INTEGER interval);
//   r3 = mode, r4 = alertable, r5 = interval
//
// This was an unimplemented stub, so it returned instantly — and a run logged
// 45,304,664 calls in five seconds. Every Sleep() in the game was a no-op,
// which turned its retry loops into busy-waits burning a core and made the
// import histogram useless (one entry drowning out everything else).
//
// The interval is a LARGE_INTEGER in 100 ns units: negative means relative,
// which is what a sleep uses. Positive means an absolute time, and since this
// runtime has no meaningful absolute system clock to wait against, that case
// yields rather than inventing a deadline.
PPC_FUNC(__imp__KeDelayExecutionThread)
{
    WOS_IMPORT_STUB("KeDelayExecutionThread");

    // Attributed by call site because implementing it properly did not end the
    // problem, it moved it. The stub's 45 M calls per five seconds became a
    // sustained 34 M — a real sleep, called in a loop by something the
    // all-thread dump cannot see, since every thread it *can* see is parked in
    // a genuine wait. The duration is carried through as the detail: a spin on
    // Sleep(0) and a spin on Sleep(1ms) are different bugs.
    const uint32_t callSite = uint32_t(ctx.lr);

    const uint32_t intervalPtr = ctx.r5.u32;
    if (intervalPtr == 0)
    {
        wos::LogCallSite("KeDelayExecutionThread", callSite, 0);
        std::this_thread::yield();
        ctx.r3.u64 = wos::kStatusSuccess;
        return;
    }

    const int64_t interval = static_cast<int64_t>(wos::LoadU64(base, intervalPtr));
    if (interval < 0)
    {
        // Relative. A zero-length relative wait is a yield, not a sleep.
        const int64_t hundredNs = -interval;
        wos::LogCallSite("KeDelayExecutionThread", callSite,
            uint32_t(hundredNs / 10000));   // report in milliseconds
        if (hundredNs == 0)
            std::this_thread::yield();
        else
            std::this_thread::sleep_for(std::chrono::nanoseconds(hundredNs * 100));
    }
    else
    {
        wos::LogCallSite("KeDelayExecutionThread", callSite, 0xFFFFFFFFu);
        std::this_thread::yield();
    }

    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif

namespace wos
{

// End of the physical block containing `addr`, or 0 if it is not in one.
// See the comment on g_physBlocks for why this is here.
uint32_t PhysicalBlockEnd(uint32_t addr)
{
    std::lock_guard<std::mutex> lock(g_physMutex);
    for (const auto& b : g_physBlocks)
        if (addr >= b.begin && addr < b.end)
            return b.end;
    return 0;
}

} // namespace wos
