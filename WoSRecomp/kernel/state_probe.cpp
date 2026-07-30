// A raw dump of the loader's own globals.
//
// Every diagnostic in this project so far reports something the HOST did: an
// import was called, a wait blocked, an APC was delivered. None of them report
// what the GUEST thinks the state of the world is, and that is now the gap.
// The I/O completion chain is mapped as far as sub_82968498, which runs exactly
// once with correct-looking arguments and does not go on to signal anything.
// Either the request object it looks up is missing, or it is in a state whose
// branch returns silently.
//
// Both of those are readable. The addresses below all came out of
// disassembly — they are not guesses — but the LAYOUT of what lives at them is
// only partly known, so this prints raw words rather than named fields. Naming
// a field you have not confirmed is how the last four wrong turns started.
//
//   0x82F71ACC   the request block. 0x829688C0 reads [+0x18] and passes it to
//                sub_82968498, then writes 0 to [+0x10]. The Game Master takes
//                its first wait handle from [+0x14].
//   0x82F719DC   the handle the signaller at 0x82965534 loads immediately
//                before tail-calling the set wrapper at 0x82B16C48.
//   0x82F719EC   the handle-validation table sub_82965488 checks r3 against —
//                base/stride/count/mask, in an order not yet confirmed.
//   0x82F71A64   a pointer to the array of 112-byte request objects. State is
//                at [obj+0x34]; sub_82965488 signals only when it reads 1, and
//                returns silently on 2 and 3.
//   0x82F71ADC   written 0 by 0x829688C0's caller after completion.
//   0x82F71AE4   the value 0x829688C0 loads into r3 for sub_82968498.
//
// Printed on the first heartbeat and again later, because a value that is
// wrong from the start and a value that changes once and then sticks need
// completely different fixes, and a single sample cannot tell them apart.

#include "guest.h"

#include <cstdint>
#include <cstdio>

namespace
{

constexpr uint32_t kRequestBlock = 0x82F71ACC;
constexpr uint32_t kSignalHandle = 0x82F719DC;
constexpr uint32_t kHandleTable  = 0x82F719EC;
constexpr uint32_t kObjectArray  = 0x82F71A64;
constexpr uint32_t kObjectStride = 112;

// .data runs 0x82BF0000..0x82F851FC and the image ends at 0x83000000. Anything
// outside that is not a static pointer, and dereferencing it would read whatever
// the 4 GiB reservation happens to have committed there — which is not an error,
// just silently meaningless. Refuse instead.
bool PlausibleImagePointer(uint32_t addr)
{
    return addr >= 0x82000000u && addr < 0x83000000u;
}

// Guest heap and physical alias windows, for pointers that are not in the image.
bool PlausibleGuestPointer(uint32_t addr)
{
    if (PlausibleImagePointer(addr))
        return true;
    if (addr >= 0x40000000u && addr < 0x50000000u)
        return true;
    if (addr >= 0xA0000000u && addr < 0xC0000000u)
        return true;
    return false;
}

void DumpWords(uint8_t* base, uint32_t addr, unsigned words, const char* what)
{
    printf("[state] %s @ 0x%08X:\n", what, addr);
    for (unsigned i = 0; i < words; i += 8)
    {
        printf("[state]   +%04X:", i * 4);
        for (unsigned j = 0; j < 8 && i + j < words; ++j)
            printf(" %08X", wos::LoadU32(base, addr + (i + j) * 4));
        printf("\n");
    }
}

} // namespace

namespace wos
{

void ReportLoaderState(uint8_t* base)
{
    // Twice, spaced apart: once early, once after the run has clearly settled.
    static unsigned s_calls = 0;
    ++s_calls;
    if (s_calls != 1 && s_calls != 4)
        return;

    printf("\n[state] --- loader globals, sample %u ---\n", s_calls);

    DumpWords(base, kRequestBlock, 16, "request block");
    DumpWords(base, kSignalHandle & ~0xFu, 16, "signal handle / handle table");

    const uint32_t arrayPtr = LoadU32(base, kObjectArray);
    printf("[state] object array pointer [0x%08X] = 0x%08X\n", kObjectArray, arrayPtr);

    if (!PlausibleGuestPointer(arrayPtr))
    {
        // This is the single most informative outcome available here: if the
        // array was never allocated then sub_82965488 cannot find any object,
        // and "the completion runs but signals nothing" is fully explained
        // without needing to read another instruction.
        printf("[state]   NOT a plausible guest pointer — the request object "
               "array does not exist yet.\n");
    }
    else
    {
        // Stride is confirmed (112 bytes); the meaning of most fields is not,
        // so print the two words the disassembly did name (+0x34 state, +0x48
        // the counter the `bgt` at 0x82965504 tests) plus the head of the
        // object, and leave interpretation to the reader.
        printf("[state]   obj    +0x00     +0x34(state) +0x48\n");
        for (unsigned i = 0; i < 8; ++i)
        {
            const uint32_t obj = arrayPtr + i * kObjectStride;
            printf("[state]   [%u] 0x%08X  %08X     %08X   %08X\n",
                i, obj,
                LoadU32(base, obj + 0x00),
                LoadU32(base, obj + 0x34),
                LoadU32(base, obj + 0x48));
        }
    }

    // The value handed to sub_82968498 on the one completion that ran. It came
    // from [0x82F71ACC + 0x18] and was 0x82D468C8 — an address in .data, not a
    // small numeric handle, which matters because sub_82965488 validates its
    // argument against a handle table rather than dereferencing it.
    const uint32_t completionArg = LoadU32(base, kRequestBlock + 0x18);
    if (PlausibleImagePointer(completionArg))
        DumpWords(base, completionArg, 16, "completion argument object");
    else
        printf("[state] completion argument 0x%08X is not an image address; "
               "not dereferenced.\n", completionArg);

    printf("[state] [0x82F71ADC]=0x%08X  [0x82F71AE4]=0x%08X\n",
        LoadU32(base, 0x82F71ADC), LoadU32(base, 0x82F71AE4));
    printf("[state] --- end of sample %u ---\n\n", s_calls);
}

} // namespace wos
