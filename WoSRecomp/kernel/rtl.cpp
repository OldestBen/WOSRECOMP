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

// ---------------------------------------------------------------------------
// String conversion.
//
// Both of these were unimplemented, and their failure mode is the one this
// project keeps meeting: an out-parameter stub that returns and writes nothing,
// leaving the caller to read whatever was already in the destination buffer.
// The run shows it directly —
//
//     [import 63] RtlMultiByteToUnicodeN
//     [import 64] RtlUnicodeToMultiByteN
//     [file] open FAILED "B<garbage>" (not found on the host)
//
// — a filename assembled out of uninitialised stack, immediately after the two
// conversions are first reached. The thread that wanted that file then blocks
// forever, and so does everything waiting on it.
//
// Two details worth stating because getting either wrong is silent:
//
//   * Every length in these APIs is in BYTES, never characters. A UTF-16
//     destination of N bytes holds N/2 code units.
//   * The guest is big-endian, so each UTF-16 unit has to be byte-swapped on
//     the way in and out. Writing them natively would produce text that looks
//     plausible in a hex dump and matches nothing.
//
// The conversion itself is Latin-1 <-> UTF-16: correct for ASCII, which is what
// asset paths are, and honest about what it does rather than pretending to
// implement a codepage we have no table for.
// ---------------------------------------------------------------------------

#ifdef WOS_IMPL_RtlMultiByteToUnicodeN
// NTSTATUS RtlMultiByteToUnicodeN(PWCH   UnicodeString,             // r3
//                                 ULONG  MaxBytesInUnicodeString,   // r4
//                                 PULONG BytesInUnicodeString,      // r5, optional
//                                 PCSTR  MultiByteString,           // r6
//                                 ULONG  BytesInMultiByteString);   // r7
PPC_FUNC(__imp__RtlMultiByteToUnicodeN)
{
    WOS_IMPORT_STUB("RtlMultiByteToUnicodeN");

    const uint32_t dest = ctx.r3.u32;
    const uint32_t destBytes = ctx.r4.u32;
    const uint32_t writtenOut = ctx.r5.u32;
    const uint32_t src = ctx.r6.u32;
    const uint32_t srcBytes = ctx.r7.u32;

    if (dest == 0 || src == 0)
    {
        ctx.r3.u64 = wos::kStatusInvalidParameter;
        return;
    }

    // One source byte becomes one UTF-16 unit, so the destination bounds the
    // conversion at half its byte count.
    const uint32_t maxChars = destBytes / 2;
    const uint32_t count = (srcBytes < maxChars) ? srcBytes : maxChars;

    for (uint32_t i = 0; i < count; ++i)
    {
        const uint8_t ch = *reinterpret_cast<const uint8_t*>(base + src + i);
        wos::StoreU16(base, dest + i * 2, ch);
    }

    if (writtenOut != 0)
        wos::StoreU32(base, writtenOut, count * 2);

    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif

#ifdef WOS_IMPL_RtlUnicodeToMultiByteN
// NTSTATUS RtlUnicodeToMultiByteN(PCHAR  MultiByteString,           // r3
//                                 ULONG  MaxBytesInMultiByteString, // r4
//                                 PULONG BytesInMultiByteString,    // r5, optional
//                                 PCWCH  UnicodeString,             // r6
//                                 ULONG  BytesInUnicodeString);     // r7
PPC_FUNC(__imp__RtlUnicodeToMultiByteN)
{
    WOS_IMPORT_STUB("RtlUnicodeToMultiByteN");

    const uint32_t dest = ctx.r3.u32;
    const uint32_t destBytes = ctx.r4.u32;
    const uint32_t writtenOut = ctx.r5.u32;
    const uint32_t src = ctx.r6.u32;
    const uint32_t srcBytes = ctx.r7.u32;

    if (dest == 0 || src == 0)
    {
        ctx.r3.u64 = wos::kStatusInvalidParameter;
        return;
    }

    const uint32_t srcChars = srcBytes / 2;
    const uint32_t count = (srcChars < destBytes) ? srcChars : destBytes;

    for (uint32_t i = 0; i < count; ++i)
    {
        const uint16_t unit = wos::LoadU16(base, src + i * 2);
        // Anything outside Latin-1 has no single-byte form. '?' is what the
        // real API substitutes, and it keeps the length exact.
        *reinterpret_cast<uint8_t*>(base + dest + i) =
            (unit <= 0xFF) ? uint8_t(unit) : uint8_t('?');
    }

    if (writtenOut != 0)
        wos::StoreU32(base, writtenOut, count);

    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif
