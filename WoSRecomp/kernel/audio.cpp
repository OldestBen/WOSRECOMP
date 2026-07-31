// Audio driver registration.
//
// Nothing here plays sound. The point is that the game's audio *thread* is
// gated on this call succeeding, and until it does, a chunk of the loading
// path stays asleep.
//
// The mixer thread at guest 0x829F4C80 loops on:
//
//     829F4CD8  lwz r3,28700(r31)   ; the driver object at [0x82F7701C]
//     829F4CDC  lwz r11,300(r3)     ; [driver+0x12C]
//     829F4CE0  cntlzw r10,r11      ; the is-zero idiom
//     829F4CE4  rlwinm r11,r10,27,31,31
//     829F4D38  beq cr6,0x829F4CC0  ; loop while [driver+0x12C] != 0
//
// so it runs while that field is non-zero and retires the first time it reads
// zero — which is what happened in every run so far. `[driver+0x12C]` is the
// registered render-driver client, and it is written by the guest code around
// XAudioRegisterRenderDriverClient.
//
// WHY A NOP'D STUB IS WORSE THAN NOTHING HERE: XenonUtils rewrites each
// unimplemented import thunk to nop/nop/nop/blr. The call returns with r3
// untouched — still holding the FIRST ARGUMENT, a pointer. Read back as an
// NTSTATUS that is a large non-zero value, i.e. an error. So the game has been
// told registration failed every single run, and correctly declined to start
// its audio pipeline. Returning a real STATUS_SUCCESS is the actual fix, and
// it is one line.
//
// The logging exists because the client structure's layout is not confirmed.
// Rather than guess at it, print what the game passes and let the next run say
// what it actually is.

#include "ppc_recomp_shared.h"
#include "import_log.h"
#include "kernel_overrides.h"
#include "guest.h"

#include <cstdio>

#ifdef WOS_IMPL_XAudioRegisterRenderDriverClient
// NTSTATUS XAudioRegisterRenderDriverClient(PXAUDIO_CLIENT client,  // r3
//                                           PDWORD driverHandle);   // r4
//
// `client` points at a small structure holding the callback and its context;
// `driverHandle` is an out-parameter the game keeps and passes back later.
PPC_FUNC(__imp__XAudioRegisterRenderDriverClient)
{
    WOS_IMPORT_STUB("XAudioRegisterRenderDriverClient");

    const uint32_t client = ctx.r3.u32;
    const uint32_t handleOut = ctx.r4.u32;

    printf("[audio] XAudioRegisterRenderDriverClient(client 0x%08X, out 0x%08X) "
           "from guest 0x%08X\n", client, handleOut, uint32_t(ctx.lr) - 4);

    // The first few words of the client structure. Two of them should be a
    // guest code address (the callback) and a context pointer; printing them
    // is how we find out which, without inventing a struct definition.
    if (client >= 0x1000 && client < 0xC0000000u)
    {
        printf("[audio]   client words:");
        for (unsigned i = 0; i < 4; ++i)
            printf(" %08X", wos::LoadU32(base, client + i * 4));
        printf("\n");
    }

    // A driver handle the game will hand back to us later. Any distinctive
    // non-zero value works; make it recognisable in a log rather than
    // something that could be mistaken for a pointer or a small index.
    constexpr uint32_t kDriverHandle = 0x41550001u;   // "AU" + 1
    if (handleOut != 0)
        wos::StoreU32(base, handleOut, kDriverHandle);

    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif

#ifdef WOS_IMPL_XAudioUnregisterRenderDriverClient
PPC_FUNC(__imp__XAudioUnregisterRenderDriverClient)
{
    WOS_IMPORT_STUB("XAudioUnregisterRenderDriverClient");
    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif

#ifdef WOS_IMPL_XAudioGetSpeakerConfig
// The stub left this out-parameter unwritten, so the game read whatever was in
// that memory as the speaker configuration. 1 is stereo, which is the safest
// thing to claim: it is what every console reports by default and it needs no
// surround handling anywhere downstream.
PPC_FUNC(__imp__XAudioGetSpeakerConfig)
{
    WOS_IMPORT_STUB("XAudioGetSpeakerConfig");

    if (ctx.r3.u32 != 0)
        wos::StoreU32(base, ctx.r3.u32, 1);

    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif

#ifdef WOS_IMPL_XAudioGetVoiceCategoryVolume
PPC_FUNC(__imp__XAudioGetVoiceCategoryVolume)
{
    WOS_IMPORT_STUB("XAudioGetVoiceCategoryVolume");
    // A float 1.0 (0x3F800000) — full volume. Zero here would be indis-
    // tinguishable from working audio that happens to be silent, which is a
    // considerably more annoying bug to chase.
    if (ctx.r4.u32 != 0)
        wos::StoreU32(base, ctx.r4.u32, 0x3F800000u);
    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif
