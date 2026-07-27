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

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

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

} // namespace

#ifdef WOS_IMPL_DbgPrint
PPC_FUNC(__imp__DbgPrint)
{
    WOS_IMPORT_STUB("DbgPrint");
    // r3 is the format string. The game formats with _vsnprintf first in the
    // common case, so by the time it reaches here the text is usually already
    // complete. Where it is not, the conversion specifiers show through
    // unsubstituted — visible and obviously wrong, rather than silently
    // dropped.
    PrintGuestString(base, ctx.r3.u32, "game");
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
// LIMITATION, stated plainly: the arguments are not substituted. Doing that
// properly means walking a PowerPC va_list — eight GPR slots, then the stack,
// with separate float registers and its own alignment rules — and getting it
// subtly wrong would produce plausible-looking but false log messages, which
// is worse than none.
//
// So the format string is copied through verbatim. Literal messages (most of
// them) come out perfectly; the rest arrive with their %s and %d intact, which
// is unmistakably a limitation rather than a lie.
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

    const char* src = wos::GuestPtr(base, format);
    char* dst = wos::GuestPtr(base, buffer);

    size_t i = 0;
    while (i + 1 < count && src[i] != '\0')
    {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';

    ctx.r3.u64 = uint32_t(i);
}
#endif
