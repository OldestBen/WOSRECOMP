// printf-family formatting for guest calls.
//
// The game's D3D layer prints a full diagnostic when it decides the GPU has
// hung, and the useful part of it is the numbers:
//
//     D3D version %i.%i %s, kernel %i, frame %i,
//     Device 0x%x, Snooped 0x%x, NonSnooped 0x%x,
//     CPU fence 0x%x, GPU fence 0x%x
//
// Copying the format string through verbatim — which is what we did — throws
// away exactly the part that says what went wrong. "CPU fence 0x%x, GPU fence
// 0x%x" tells us nothing; the two actual values tell us whether the GPU is
// behind the CPU and by how much.
//
// Argument passing on the Xbox 360 (64-bit PowerPC) is uniform enough to make
// this tractable: every variadic argument occupies one 8-byte doubleword slot,
// integers right-aligned within it. A va_list is a plain pointer to the next
// slot, so walking it is just reading 8 bytes at a time.
//
// Two ways in, so the formatter takes its arguments through a callback rather
// than assuming either:
//
//   * _vsnprintf receives a va_list, so slots come from guest memory.
//   * DbgPrint and sprintf are variadic themselves, so the first arguments
//     arrive in registers and are read out of the context.
//
// The register path is capped at what the registers hold. Beyond that the
// arguments continue in the caller's parameter save area, and the offset of
// that area is not something this code has established — so rather than guess
// at a stack layout and emit numbers that look authoritative and are wrong,
// it stops substituting and says so inline. A visible "<...>" in a log line is
// a limitation; a plausible wrong fence value would be a lie.

#include "ppc_recomp_shared.h"
#include "guest.h"
#include "format.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace wos
{

namespace
{

// Everything a single conversion needs, rebuilt as a host format string so the
// host's own printf does the actual padding, width and precision work.
// Reimplementing "%-25.8s" by hand would be a second source of bugs.
struct Spec
{
    char text[64];
    size_t length = 0;

    void push(char c)
    {
        if (length + 1 < sizeof(text))
            text[length++] = c;
        text[length] = '\0';
    }
};

bool IsLengthModifier(char c)
{
    return c == 'h' || c == 'l' || c == 'L' || c == 'w' || c == 'I' ||
           c == 'j' || c == 'z' || c == 't' || c == '3' || c == '2' || c == '6' || c == '4';
}

} // namespace

std::string FormatGuest(uint8_t* base, uint32_t formatAddr, ArgSource& args)
{
    std::string out;
    if (formatAddr == 0)
        return out;

    const char* fmt = GuestPtr(base, formatAddr);

    // A format string with no terminator would otherwise walk the address
    // space; the guest's own buffers are far smaller than this.
    constexpr size_t kMaxFormat = 4096;

    for (size_t i = 0; i < kMaxFormat && fmt[i] != '\0'; ++i)
    {
        if (fmt[i] != '%')
        {
            out.push_back(fmt[i]);
            continue;
        }

        if (fmt[i + 1] == '%')
        {
            out.push_back('%');
            ++i;
            continue;
        }

        Spec spec;
        spec.push('%');

        size_t j = i + 1;

        // Flags, then width, then precision. A '*' takes its value from an
        // argument, so it consumes a slot before the conversion does.
        while (fmt[j] == '-' || fmt[j] == '+' || fmt[j] == ' ' ||
               fmt[j] == '#' || fmt[j] == '0')
        {
            spec.push(fmt[j]);
            ++j;
        }

        auto starArgument = [&]() -> bool {
            uint64_t slot;
            if (!args.next(slot))
                return false;
            char n[16];
            snprintf(n, sizeof(n), "%d", int32_t(uint32_t(slot)));
            for (const char* p = n; *p; ++p)
                spec.push(*p);
            return true;
        };

        if (fmt[j] == '*')
        {
            if (!starArgument())
            {
                out += "<no more args>";
                break;
            }
            ++j;
        }
        else
        {
            while (fmt[j] >= '0' && fmt[j] <= '9')
            {
                spec.push(fmt[j]);
                ++j;
            }
        }

        if (fmt[j] == '.')
        {
            spec.push('.');
            ++j;
            if (fmt[j] == '*')
            {
                if (!starArgument())
                {
                    out += "<no more args>";
                    break;
                }
                ++j;
            }
            else
            {
                while (fmt[j] >= '0' && fmt[j] <= '9')
                {
                    spec.push(fmt[j]);
                    ++j;
                }
            }
        }

        // Length modifiers are parsed but not copied: the guest's widths are
        // not the host's, so the conversion below picks an explicit host type
        // instead. Copying "%l" through would make "%ld" mean 64-bit on Linux
        // and 32-bit on Windows.
        bool isLongLong = false;
        while (IsLengthModifier(fmt[j]))
        {
            if (fmt[j] == 'l' && fmt[j + 1] == 'l')
                isLongLong = true;
            if (fmt[j] == 'I' && fmt[j + 1] == '6' && fmt[j + 2] == '4')
                isLongLong = true;
            ++j;
        }

        const char conv = fmt[j];
        if (conv == '\0')
        {
            out += "<truncated format>";
            break;
        }

        uint64_t slot = 0;
        if (!args.next(slot))
        {
            // Out of readable arguments. Say which conversion was dropped so
            // the line stays honest and diagnosable.
            out += "<%";
            out.push_back(conv);
            out += " unavailable>";
            i = j;
            continue;
        }

        char rendered[1024];
        switch (conv)
        {
        case 'd':
        case 'i':
            if (isLongLong)
            {
                spec.push('l'); spec.push('l'); spec.push('d');
                snprintf(rendered, sizeof(rendered), spec.text, (long long)int64_t(slot));
            }
            else
            {
                spec.push('d');
                snprintf(rendered, sizeof(rendered), spec.text, int32_t(uint32_t(slot)));
            }
            break;

        case 'u':
        case 'o':
        case 'x':
        case 'X':
            if (isLongLong)
            {
                spec.push('l'); spec.push('l'); spec.push(conv);
                snprintf(rendered, sizeof(rendered), spec.text, (unsigned long long)slot);
            }
            else
            {
                spec.push(conv);
                snprintf(rendered, sizeof(rendered), spec.text, uint32_t(slot));
            }
            break;

        case 'c':
            spec.push('c');
            snprintf(rendered, sizeof(rendered), spec.text, int(uint32_t(slot) & 0xFF));
            break;

        case 'p':
            // Print the guest pointer, not the host one it would map to — the
            // guest address is what every other log line and the disassembly
            // talk about.
            spec.length = 0;
            spec.text[0] = '\0';
            spec.push('0'); spec.push('x'); spec.push('%'); spec.push('0');
            spec.push('8'); spec.push('X');
            snprintf(rendered, sizeof(rendered), spec.text, uint32_t(slot));
            break;

        case 's':
        {
            spec.push('s');
            const uint32_t addr = uint32_t(slot);
            if (addr == 0)
            {
                snprintf(rendered, sizeof(rendered), spec.text, "(null)");
            }
            else
            {
                // Bound the copy: a bad pointer here would otherwise read
                // until it happened to find a zero byte.
                const char* s = GuestPtr(base, addr);
                char bounded[512];
                size_t n = 0;
                while (n + 1 < sizeof(bounded) && s[n] != '\0')
                {
                    bounded[n] = s[n];
                    ++n;
                }
                bounded[n] = '\0';
                snprintf(rendered, sizeof(rendered), spec.text, bounded);
            }
            break;
        }

        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        {
            spec.push(conv);
            double d;
            static_assert(sizeof(d) == sizeof(slot));
            std::memcpy(&d, &slot, sizeof(d));
            snprintf(rendered, sizeof(rendered), spec.text, d);
            break;
        }

        default:
            // Unknown conversion: emit it literally rather than inventing a
            // value, and do not treat the slot as consumed correctly — say so.
            snprintf(rendered, sizeof(rendered), "<%%%c?>", conv);
            break;
        }

        out += rendered;
        i = j;
    }

    return out;
}

// Arguments from a guest va_list: consecutive 8-byte slots.
bool GuestVaListArgs::next(uint64_t& slot)
{
    if (cursor == 0)
        return false;
    slot = LoadU64(base, cursor);
    cursor += 8;
    return true;
}

// Arguments from the register set, in ABI order. Runs out when the registers
// do — see the note at the top of this file about why it stops there.
bool RegisterArgs::next(uint64_t& slot)
{
    if (index >= count)
        return false;
    slot = values[index++];
    return true;
}

} // namespace wos
