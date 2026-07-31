#pragma once
// A stand-in for XenonRecomp's generated header, for SYNTAX CHECKING ONLY.
//
// tools/check_syntax.sh compiles the hand-written kernel/*.cpp against this so
// a typo is caught in seconds on any machine, without the 4 GiB of generated
// translation units the real build needs — and, more to the point, without
// needing the game's XEX present at all.
//
// It is NOT a functional implementation and must never be on the real include
// path. It exists because two builds in a row were broken by edits that would
// have failed instantly under a compiler.
//
// Keep it minimally faithful: same member names, same signatures. If the real
// header changes shape, this has to follow, and check_syntax.sh will say so by
// failing on code that is actually fine.

#include <cstdint>
#include <cstring>

union PPCRegister
{
    uint64_t u64;
    int64_t  s64;
    uint32_t u32;
    int32_t  s32;
    uint16_t u16;
    uint8_t  u8;
    double   f64;
};

struct PPCFPSCR { void loadFromHost() {} };

struct PPCContext
{
    PPCRegister r0,  r1,  r2,  r3,  r4,  r5,  r6,  r7;
    PPCRegister r8,  r9,  r10, r11, r12, r13, r14, r15;
    PPCRegister r16, r17, r18, r19, r20, r21, r22, r23;
    PPCRegister r24, r25, r26, r27, r28, r29, r30, r31;
    PPCFPSCR fpscr;
    uint64_t lr = 0;
    uint64_t ctr = 0;
};

using PPCFunc = void(PPCContext& ctx, uint8_t* base);

#define PPC_FUNC(name)      void name(PPCContext& ctx, uint8_t* base)
#define PPC_FUNC_IMPL(name) void name(PPCContext& ctx, uint8_t* base)

// Always "not found": the syntax check never runs guest code, and returning
// null exercises the error path in every caller, which is the branch most
// likely to contain an untested typo.
inline PPCFunc* PPCLookupFunc(uint8_t*, uint32_t) { return nullptr; }
#define PPC_LOOKUP_FUNC(base, addr) PPCLookupFunc(base, addr)

#define PPC_MEMORY_SIZE 0x100000000ull
#define PPC_CODE_BASE   0x82000000u
#define PPC_IMAGE_BASE  0x82000000u
