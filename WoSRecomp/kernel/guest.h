#pragma once
// Helpers for touching guest memory from host code.
//
// Guest memory is a flat mapping: guest address `x` lives at `base + x`. The
// guest is big-endian, so every multi-byte access has to be swapped. Getting
// that wrong is silent — the value is merely nonsense — so all guest access
// from kernel code should go through here rather than being open-coded.

#include <cstdint>
#include <cstring>

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
constexpr uint32_t kStatusNoMemory          = 0xC0000017u;
constexpr uint32_t kStatusInvalidParameter  = 0xC000000Du;

// Reserve-and-commit a guest range. Returns the guest base, or 0 on failure.
// Implemented in kernel/memory.cpp.
uint32_t GuestAlloc(uint8_t* base, uint32_t requestedBase, uint32_t size, uint32_t alignment);
bool GuestCommit(uint8_t* base, uint32_t addr, uint32_t size);

// Print the current guest call stack and terminate. Implemented in main.cpp,
// declared here so kernel code can end a run without unwinding through
// recompiled frames that have no idea how to handle it.
[[noreturn]] void FatalGuestStop(const char* reason);

} // namespace wos
