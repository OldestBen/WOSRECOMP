#pragma once
// Helpers for touching guest memory from host code.
//
// Guest memory is a flat mapping: guest address `x` lives at `base + x`. The
// guest is big-endian, so every multi-byte access has to be swapped. Getting
// that wrong is silent — the value is merely nonsense — so all guest access
// from kernel code should go through here rather than being open-coded.

#include <cstdint>
#include <cstring>
#include <mutex>

namespace wos
{

inline uint32_t LoadU32(uint8_t* base, uint32_t addr)
{
    uint32_t v;
    std::memcpy(&v, base + addr, sizeof(v));
    return __builtin_bswap32(v);
}

inline void StoreU32(uint8_t* base, uint32_t addr, uint32_t value)
{
    const uint32_t v = __builtin_bswap32(value);
    std::memcpy(base + addr, &v, sizeof(v));
}

inline uint64_t LoadU64(uint8_t* base, uint32_t addr)
{
    uint64_t v;
    std::memcpy(&v, base + addr, sizeof(v));
    return __builtin_bswap64(v);
}

inline void StoreU64(uint8_t* base, uint32_t addr, uint64_t value)
{
    const uint64_t v = __builtin_bswap64(value);
    std::memcpy(base + addr, &v, sizeof(v));
}

inline uint16_t LoadU16(uint8_t* base, uint32_t addr)
{
    uint16_t v;
    std::memcpy(&v, base + addr, sizeof(v));
    return __builtin_bswap16(v);
}

inline void StoreU16(uint8_t* base, uint32_t addr, uint16_t value)
{
    const uint16_t v = __builtin_bswap16(value);
    std::memcpy(base + addr, &v, sizeof(v));
}

inline char* GuestPtr(uint8_t* base, uint32_t addr)
{
    return reinterpret_cast<char*>(base + addr);
}

// Xbox 360 NTSTATUS values we actually return.
constexpr uint32_t kStatusSuccess           = 0x00000000u;
constexpr uint32_t kStatusTimeout           = 0x00000102u;
constexpr uint32_t kStatusNoMemory          = 0xC0000017u;
constexpr uint32_t kStatusInvalidParameter  = 0xC000000Du;
constexpr uint32_t kStatusInvalidHandle     = 0xC0000008u;
constexpr uint32_t kStatusNoSuchFile        = 0xC000000Fu;
constexpr uint32_t kStatusObjectPathNotFound = 0xC000003Au;

// Allocate the per-thread block r13 points at, and start the tick thread that
// keeps its clock field advancing. Returns the guest KPCR address to load into
// r13, or 0 on failure. Implemented in kernel/pcr.cpp — see the comment there
// for why guest code cannot run correctly without this.
uint32_t CreateThreadPcr(uint8_t* base);

// Reserve-and-commit a guest range. Returns the guest base, or 0 on failure.
// Implemented in kernel/memory.cpp.
uint32_t GuestAlloc(uint8_t* base, uint32_t requestedBase, uint32_t size, uint32_t alignment);
bool GuestCommit(uint8_t* base, uint32_t addr, uint32_t size);

// End of the physical allocation containing `addr`, or 0 if it is not inside
// one. Implemented in kernel/system.cpp. Lets host code bound a read against
// the block it was actually given rather than against the whole alias window.
uint32_t PhysicalBlockEnd(uint32_t addr);

// Watch the D3D device fields that gate the frame loop. Implemented in
// kernel/d3d_probe.cpp — see the comment there for which fields and why.
// The probe samples at 200 us so a flag set and cleared within one frame is
// still observed; the report is called from the heartbeat.
void StartD3DProbe(uint8_t* base);
void ReportD3DProbe(uint8_t* base);

// Serialises diagnostic output.
//
// The all-thread stack dumper and the blocked-wait reporter run on different
// threads and both print multi-line blocks. Without a shared lock their lines
// shred each other — a real run produced a stack dump with three threads'
// frames interleaved and renumbered, which is worse than no dump at all.
//
// Recursive because the wait reporter prints a header and then calls
// PrintGuestStack, which takes the same lock.
std::recursive_mutex& DiagnosticLock();

// Print the current guest call stack and terminate. Implemented in main.cpp,
// declared here so kernel code can end a run without unwinding through
// recompiled frames that have no idea how to handle it.
[[noreturn]] void FatalGuestStop(const char* reason);

// Print the recompiled functions on the *calling* thread's stack. Implemented
// in main.cpp via dbghelp. Safe to call from any guest thread — each one has
// its own host stack, so this names whatever that thread is actually doing.
void PrintGuestStack(unsigned frames);

// Register the calling thread so it can be backtraced later by any other
// thread. Called by the main thread and by each ExCreateThread trampoline.
void RegisterThreadForBacktrace(const char* label);

// Suspend every registered thread except the caller and print its stack. This
// is the only way to see a thread that is running guest code and calling no
// imports at all — which is exactly the state that has been hardest to
// diagnose.
void DumpAllThreadStacks(unsigned frames);

} // namespace wos
