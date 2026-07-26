// Runtime-library odds and ends.

#include "ppc_recomp_shared.h"
#include "import_log.h"
#include "kernel_overrides.h"
#include "guest.h"

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
