// Runtime-library odds and ends.

#include "ppc_recomp_shared.h"
#include "import_log.h"
#include "kernel_overrides.h"
#include "guest.h"

#include <cstdio>
#include <cstring>

#ifdef WOS_IMPL_RtlInitAnsiString
// VOID RtlInitAnsiString(PANSI_STRING DestinationString,  // r3
//                        PCSZ         SourceString);      // r4
//
// ANSI_STRING is { USHORT Length; USHORT MaximumLength; PCHAR Buffer; } — 8
// bytes on this target, all big-endian.
//
// Called 57,204 times in the run that ran away, six per bugcheck iteration:
// the panic handler formatting its message. Cheap to do properly.
PPC_FUNC(__imp__RtlInitAnsiString)
{
    WOS_IMPORT_STUB("RtlInitAnsiString");

    const uint32_t dest = ctx.r3.u32;
    const uint32_t src = ctx.r4.u32;

    if (dest == 0)
        return;

    if (src == 0)
    {
        wos::StoreU16(base, dest + 0, 0);
        wos::StoreU16(base, dest + 2, 0);
        wos::StoreU32(base, dest + 4, 0);
        return;
    }

    const size_t len = std::strlen(wos::GuestPtr(base, src));
    const uint16_t length = uint16_t(len > 0xFFFE ? 0xFFFE : len);

    wos::StoreU16(base, dest + 0, length);
    wos::StoreU16(base, dest + 2, uint16_t(length + 1));   // includes the NUL
    wos::StoreU32(base, dest + 4, src);
}
#endif

#ifdef WOS_IMPL_RtlRaiseException
// VOID RtlRaiseException(PEXCEPTION_RECORD ExceptionRecord);   // r3
//
// EXCEPTION_RECORD on the 32-bit guest:
//   0x00 DWORD ExceptionCode
//   0x04 DWORD ExceptionFlags
//   0x08 PTR   ExceptionRecord (nested)
//   0x0C PTR   ExceptionAddress
//   0x10 DWORD NumberParameters
//   0x14 ...   ExceptionInformation[]
//
// The previous run raised exactly six of these — one per thread created — and
// it read as six failures. It is not: 0x406D1388 is the SetThreadName
// convention, where the "exception" is a carrier for a name string that a
// debugger is meant to pick up. Decoding it turns six alarming log lines into
// six thread names, which is genuinely useful for reading the trace.
PPC_FUNC(__imp__RtlRaiseException)
{
    WOS_IMPORT_STUB("RtlRaiseException");

    const uint32_t record = ctx.r3.u32;
    if (record == 0)
        return;

    const uint32_t code = wos::LoadU32(base, record + 0x00);
    const uint32_t numParams = wos::LoadU32(base, record + 0x10);

    if (code == 0x406D1388u && numParams >= 3)
    {
        const uint32_t namePtr = wos::LoadU32(base, record + 0x18);   // Information[1]
        const uint32_t threadId = wos::LoadU32(base, record + 0x1C);  // Information[2]

        if (namePtr != 0)
        {
            printf("[thread] name for 0x%X: \"%s\"\n",
                threadId, wos::GuestPtr(base, namePtr));
            return;
        }
    }

    // Anything else is a real raise. We have no SEH, so it cannot propagate —
    // say so rather than silently swallowing it.
    printf("[rtl] RtlRaiseException code 0x%08X, %u param(s) — no SEH, ignored\n",
        code, numParams);
}
#endif
