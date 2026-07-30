// A raw dump of the loader's own globals.
//
// Every other diagnostic in this project reports something the HOST did: an
// import was called, a wait blocked, an APC was delivered. None of them report
// what the GUEST believes the state of the world is, and that turned out to be
// exactly the gap.
//
// The first version of this file printed raw words because the layout was only
// half known. It is now fully decoded from sub_82965488, so the fields below
// are named — every offset here is read straight out of that disassembly:
//
//     82965488  rlwinm r11,r3,0,0,0   ; handle & 0x80000000 must be zero
//     82965494  lis    r11,0x82F7
//     82965498  addi   r11,r11,0x19EC ; r11 = 0x82F719EC, the descriptor
//     8296549C  lwz    r8,36(r11)     ; [desc+0x24] = mask
//     829654A0  lwz    r9,32(r11)     ; [desc+0x20] = count
//     829654A4  and    r10,r8,r3      ; slot = handle & mask
//     829654A8  cmplw  cr6,r10,r9
//     829654AC  bgelr  cr6            ; slot >= count -> reject
//     829654B0  lwz    r9,28(r11)     ; [desc+0x1C] = stride
//     829654B4  lwz    r11,0(r11)     ; [desc+0x00] = table base
//     829654B8  mullw  r10,r9,r10
//     829654BC  add    r11,r10,r11    ; entry = base + slot*stride
//     829654C0  lwz    r11,8(r11)     ; [entry+0x08] = the stored handle
//     829654C4  cmpw   cr6,r3,r11
//     829654C8  bnelr  cr6            ; stored != given -> reject
//     829654CC  and    r11,r11,r8     ; index = stored & mask
//     829654D0  cmpwi  cr6,r11,-1
//     829654D4  beqlr  cr6
//     829654DC  mulli  r10,r11,112
//     829654E0  lwz    r11,6756(r9)   ; [0x82F71A64] = request-object array
//     829654E4  add    r11,r10,r11    ; obj = array + index*112
//     829654F0  lwz    r10,52(r11)    ; [obj+0x34] = state
//     829654F4  cmpwi  cr6,r10,1
//     829654F8  beq    cr6,0x82965534 ; state == 1 -> SIGNAL
//     829654FC  lwz    r9,72(r11)     ; [obj+0x48]
//     82965504  bgt    cr6,0x82965534 ; [obj+0x48] > 0 -> SIGNAL
//
// and the signaller itself:
//
//     82965534  lis  r9,0x82F7
//     8296553C  stw  r10,52(r11)      ; state = 3
//     82965540  lwz  r3,6620(r9)      ; [0x82F719DC] = the handle to set
//     82965544  b    0x82b16c48       ; tail-call the set wrapper
//
// So [0x82F719DC] names the event that unblocks the Game Master, and a request
// object sitting in state 1 is one that would signal the instant anything
// called sub_82965488 with its handle. Both are now printed.
//
// Separately, the FILE REQUEST at [0x82F71ACC+0x18] is a different object with
// a different layout, tracked by sub_82968498 through its own state field at
// +0x48 (1 -> 2 on success, 1 -> 5 on error, 3 -> 4). It is dumped far enough
// to include that field, because "did the completion actually land" is only
// answerable by reading it.

#include "guest.h"

#include <cstdint>
#include <cstdio>

namespace
{

constexpr uint32_t kRequestBlock  = 0x82F71ACC;   // the file-request block
constexpr uint32_t kSignalHandle  = 0x82F719DC;   // event the signaller sets
constexpr uint32_t kDescriptor    = 0x82F719EC;   // handle-table descriptor
constexpr uint32_t kObjectArray   = 0x82F71A64;   // -> array of 112-byte objects
constexpr uint32_t kObjectStride  = 112;

// .data runs 0x82BF0000..0x82F851FC; the image ends at 0x83000000.
bool PlausibleImagePointer(uint32_t addr)
{
    return addr >= 0x82000000u && addr < 0x83000000u;
}

bool PlausibleGuestPointer(uint32_t addr)
{
    if (PlausibleImagePointer(addr))
        return true;
    if (addr >= 0x40000000u && addr < 0x50000000u)   // virtual heap
        return true;
    if (addr >= 0xA0000000u && addr < 0xC0000000u)   // physical alias window
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

// The state values sub_82965488 branches on, so the dump reads as a decision
// rather than as a number needing a lookup.
const char* StateName(uint32_t state)
{
    switch (state)
    {
    case 0: return "idle";
    case 1: return "READY TO SIGNAL";
    case 2: return "-";
    case 3: return "signalled (returns silently)";
    case 4: return "-";
    case 6:
    case 7: return "-> 4 on completion";
    default: return "?";
    }
}

} // namespace

namespace wos
{

void ReportLoaderState(uint8_t* base)
{
    // Twice, spaced apart: a value that is wrong from the start and a value
    // that changes once and then sticks need different fixes, and one sample
    // cannot tell them apart.
    static unsigned s_calls = 0;
    ++s_calls;
    if (s_calls != 1 && s_calls != 4)
        return;

    printf("\n[state] --- loader globals, sample %u ---\n", s_calls);

    // --- the handle table the completion validates against -----------------
    const uint32_t tableBase = LoadU32(base, kDescriptor + 0x00);
    const uint32_t stride    = LoadU32(base, kDescriptor + 0x1C);
    const uint32_t count     = LoadU32(base, kDescriptor + 0x20);
    const uint32_t mask      = LoadU32(base, kDescriptor + 0x24);
    printf("[state] handle table: base 0x%08X stride %u count %u mask 0x%08X\n",
        tableBase, stride, count, mask);

    const uint32_t signalHandle = LoadU32(base, kSignalHandle);
    printf("[state] signal handle [0x%08X] = 0x%08X  "
           "(this is the event the signaller sets)\n",
        kSignalHandle, signalHandle);

    // --- the request objects ----------------------------------------------
    const uint32_t arrayPtr = LoadU32(base, kObjectArray);
    printf("[state] request-object array [0x%08X] = 0x%08X\n", kObjectArray, arrayPtr);

    if (!PlausibleGuestPointer(arrayPtr))
    {
        printf("[state]   NOT a plausible guest pointer — the array does not "
               "exist, so nothing could be completed even if it were called.\n");
    }
    else
    {
        printf("[state]   obj  address     state  handle[+0x64]  [+0x48]  meaning\n");
        for (unsigned i = 0; i < 8; ++i)
        {
            const uint32_t obj    = arrayPtr + i * kObjectStride;
            const uint32_t state  = LoadU32(base, obj + 0x34);
            const uint32_t handle = LoadU32(base, obj + 0x64);
            const uint32_t plus48 = LoadU32(base, obj + 0x48);

            // Skip the long tail of untouched slots — an all-zero object says
            // nothing and eight lines of zeros hide the one that matters.
            if (state == 0 && handle == 0 && plus48 == 0)
                continue;

            printf("[state]   [%u] 0x%08X  %5u  0x%08X     %8u  %s\n",
                i, obj, state, handle, plus48, StateName(state));
        }
        // The full first object, since it is the one that has ever been used.
        DumpWords(base, arrayPtr, 28, "request object [0]");
    }

    // --- the file request the APC completed --------------------------------
    //
    // 28 words rather than 16: sub_82968498's state field is at +0x48, which
    // the first version of this dump stopped four bytes short of. That is the
    // single field that says whether the completion landed.
    const uint32_t fileRequest = LoadU32(base, kRequestBlock + 0x18);
    printf("[state] file request [0x%08X+0x18] = 0x%08X\n", kRequestBlock, fileRequest);
    if (PlausibleImagePointer(fileRequest))
    {
        const uint32_t st = LoadU32(base, fileRequest + 0x48);
        printf("[state]   [+0x48] = %u  (1 = issued, 2 = COMPLETED OK, "
               "4 = acknowledged, 5 = completed with error)\n", st);
        printf("[state]   [+0x20] = 0x%08X  (bytes transferred, written by "
               "sub_82968498 on success)\n", LoadU32(base, fileRequest + 0x20));
        DumpWords(base, fileRequest, 28, "file request object");
    }
    else
    {
        printf("[state]   not an image address; not dereferenced.\n");
    }

    DumpWords(base, kRequestBlock, 16, "request block");

    printf("[state] --- end of sample %u ---\n\n", s_calls);
}

} // namespace wos
