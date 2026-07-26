// Bugcheck and shutdown paths.
//
// WHY THIS FILE EXISTS: on the real console `KeBugCheck` is
// DECLSPEC_NORETURN — it halts the machine. Stubbed as an ordinary function
// that returns, the game's panic handler returns to the code that panicked,
// which panics again. The first run to reach the kernel produced ~19,000
// nested bugchecks and a runaway stack from exactly this.
//
// The counts made it unmistakable: per iteration, exactly 1x
// KeGetCurrentProcessType, 2x KeBugCheck, 6x RtlInitAnsiString — a panic
// handler formatting a message, forever.
//
// So these terminate. Stopping at the first panic and printing its code is
// strictly more useful than running on through undefined behaviour.

#include "ppc_recomp_shared.h"
#include "import_log.h"
#include "kernel_overrides.h"
#include "guest.h"

#include <cstdio>
#include <cinttypes>

namespace
{

// Bugcheck codes the Xbox 360 kernel uses. Not exhaustive — enough to make
// the common ones readable instead of a bare hex number.
const char* BugCheckName(uint32_t code)
{
    switch (code)
    {
    case 0x00000001: return "APC_INDEX_MISMATCH";
    case 0x0000000A: return "IRQL_NOT_LESS_OR_EQUAL";
    case 0x0000001E: return "KMODE_EXCEPTION_NOT_HANDLED";
    case 0x00000024: return "NTFS_FILE_SYSTEM";
    case 0x0000002E: return "DATA_BUS_ERROR";
    case 0x0000003B: return "SYSTEM_SERVICE_EXCEPTION";
    case 0x0000004E: return "PFN_LIST_CORRUPT";
    case 0x0000007F: return "UNEXPECTED_KERNEL_MODE_TRAP";
    case 0x000000C4: return "DRIVER_VERIFIER_DETECTED_VIOLATION";
    case 0x000000C5: return "DRIVER_CORRUPTED_EXPOOL";
    case 0x000000EF: return "CRITICAL_PROCESS_DIED";
    default:         return nullptr;
    }
}

[[noreturn]] void Bugcheck(PPCContext& ctx, bool extended)
{
    const uint32_t code = ctx.r3.u32;

    printf("\n=== GUEST BUGCHECK ===\n");

    if (const char* name = BugCheckName(code))
        printf("code 0x%08X (%s)\n", code, name);
    else
        printf("code 0x%08X\n", code);

    if (extended)
    {
        printf("params: 0x%08X 0x%08X 0x%08X 0x%08X\n",
            ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    }

    printf("\nThe game called KeBugCheck — it decided it could not continue.\n");
    printf("On hardware this halts the console, so it does NOT return here\n");
    printf("either; returning is what previously produced ~19,000 nested\n");
    printf("bugchecks and a runaway stack.\n");

    wos::FatalGuestStop("guest bugcheck");
}

} // namespace

#ifdef WOS_IMPL_KeBugCheck
PPC_FUNC(__imp__KeBugCheck)
{
    WOS_IMPORT_STUB("KeBugCheck");
    Bugcheck(ctx, false);
}
#endif

#ifdef WOS_IMPL_KeBugCheckEx
PPC_FUNC(__imp__KeBugCheckEx)
{
    WOS_IMPORT_STUB("KeBugCheckEx");
    Bugcheck(ctx, true);
}
#endif

#ifdef WOS_IMPL_HalReturnToFirmware
PPC_FUNC(__imp__HalReturnToFirmware)
{
    WOS_IMPORT_STUB("HalReturnToFirmware");

    // Reboot / return to dashboard. Also noreturn on hardware.
    //
    // NOTE: the first traced run reached this as import #2, immediately after
    // NtAllocateVirtualMemory returned nothing. If it still fires that early
    // now that allocation works, the argument is worth reading — 0 is
    // typically "reboot to dashboard", and reaching it during startup means
    // the game gave up rather than that it finished.
    printf("\n=== GUEST REQUESTED SHUTDOWN ===\n");
    printf("HalReturnToFirmware(0x%08X)\n", ctx.r3.u32);
    printf("The game asked to return to firmware/dashboard. During startup\n");
    printf("that means it gave up, not that it finished.\n");

    wos::FatalGuestStop("guest requested shutdown");
}
#endif
