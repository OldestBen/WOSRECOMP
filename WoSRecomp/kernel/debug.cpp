// The game's own debug output.
//
// Web of Shadows calls _vsnprintf to format a message and then DbgPrint to
// emit it. Getting these working means the engine tells us what it is doing in
// its own words, which is worth more than any amount of inference from the
// import trace.

#include "ppc_recomp_shared.h"
#include "import_log.h"
#include "kernel_overrides.h"
#include "guest.h"
#include "format.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <algorithm>

namespace
{

std::mutex g_printMutex;

// Print a guest string, trimming the trailing newline so our own framing
// stays tidy, and bounding the length so a missing NUL can't run away
// through the whole address space.
void PrintGuestString(uint8_t* base, uint32_t addr, const char* tag)
{
    if (addr == 0)
        return;

    const char* s = wos::GuestPtr(base, addr);
    const size_t max = 4096;
    size_t len = 0;
    while (len < max && s[len] != '\0')
        ++len;

    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
        --len;

    if (len == 0)
        return;

    std::lock_guard<std::mutex> lock(g_printMutex);
    printf("[%s] %.*s\n", tag, int(len), s);
}

// Print an already-formatted string, same trimming and bounding as above.
void PrintFormatted(const std::string& text, const char* tag)
{
    size_t len = text.size();
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r'))
        --len;
    if (len == 0)
        return;

    std::lock_guard<std::mutex> lock(g_printMutex);
    printf("[%s] %.*s\n", tag, int(len), text.c_str());
}

} // namespace

#ifdef WOS_IMPL_DbgPrint
// VOID DbgPrint(const char* format, ...);
//   r3 = format, r4..r10 = the first seven arguments
//
// This is variadic, not a pre-formatted string. Printing r3 raw was throwing
// away every number the game reports — including the "CPU fence 0x%x, GPU
// fence 0x%x" line in its GPU-hang dump, which is the one that says whether
// the GPU is behind and by how much.
PPC_FUNC(__imp__DbgPrint)
{
    WOS_IMPORT_STUB("DbgPrint");

    wos::RegisterArgs args;
    args.add(ctx.r4.u64);
    args.add(ctx.r5.u64);
    args.add(ctx.r6.u64);
    args.add(ctx.r7.u64);
    args.add(ctx.r8.u64);
    args.add(ctx.r9.u64);
    args.add(ctx.r10.u64);

    PrintFormatted(wos::FormatGuest(base, ctx.r3.u32, args), "game");
}
#endif

#ifdef WOS_IMPL_sprintf
// int sprintf(char* buffer, const char* format, ...);
//   r3 = buffer, r4 = format, r5..r10 = the first six arguments
PPC_FUNC(__imp__sprintf)
{
    WOS_IMPORT_STUB("sprintf");

    const uint32_t buffer = ctx.r3.u32;
    if (buffer == 0 || ctx.r4.u32 == 0)
    {
        ctx.r3.u64 = uint32_t(-1);
        return;
    }

    wos::RegisterArgs args;
    args.add(ctx.r5.u64);
    args.add(ctx.r6.u64);
    args.add(ctx.r7.u64);
    args.add(ctx.r8.u64);
    args.add(ctx.r9.u64);
    args.add(ctx.r10.u64);

    const std::string text = wos::FormatGuest(base, ctx.r4.u32, args);

    // sprintf has no bound. The guest chose the buffer size and we cannot see
    // it, so this writes what the guest asked for — the same exposure the game
    // has on real hardware.
    char* dst = wos::GuestPtr(base, buffer);
    std::memcpy(dst, text.c_str(), text.size() + 1);

    ctx.r3.u64 = uint32_t(text.size());
}
#endif

#ifdef WOS_IMPL_OutputDebugStringA
PPC_FUNC(__imp__OutputDebugStringA)
{
    WOS_IMPORT_STUB("OutputDebugStringA");
    PrintGuestString(base, ctx.r3.u32, "game");
}
#endif

#ifdef WOS_IMPL__vsnprintf
// int _vsnprintf(char* buffer, size_t count, const char* format, va_list args);
//   r3 = buffer, r4 = count, r5 = format, r6 = va_list
//
// The va_list case is the well-defined one: on this ABI it is a pointer to the
// next 8-byte argument slot, so the arguments really can be walked rather than
// guessed at. This used to copy the format string through verbatim.
PPC_FUNC(__imp___vsnprintf)
{
    WOS_IMPORT_STUB("_vsnprintf");

    const uint32_t buffer = ctx.r3.u32;
    const uint32_t count = ctx.r4.u32;
    const uint32_t format = ctx.r5.u32;

    if (buffer == 0 || count == 0 || format == 0)
    {
        ctx.r3.u64 = uint32_t(-1);
        return;
    }

    wos::GuestVaListArgs args(base, ctx.r6.u32);
    const std::string text = wos::FormatGuest(base, format, args);

    char* dst = wos::GuestPtr(base, buffer);
    const size_t n = std::min<size_t>(text.size(), count - 1);
    std::memcpy(dst, text.c_str(), n);
    dst[n] = '\0';

    // The real _vsnprintf returns -1 when the text did not fit.
    ctx.r3.u64 = (text.size() >= count) ? uint32_t(-1) : uint32_t(n);
}
#endif
