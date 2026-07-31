// Tripwires on specific *guest* functions.
//
// Everything else in kernel/ overrides an import. This file overrides ordinary
// recompiled game code, which is possible for the same reason: the recompiler
// emits every function twice — the real body as
//
//     PPC_FUNC_IMPL(__imp__sub_XXXXXXXX) { ... }
//
// and a weak alias `sub_XXXXXXXX` pointing at it. A strong definition of
// `sub_XXXXXXXX` here wins at link time, so we can observe a call and then
// forward to the untouched original. Nothing about the game's behaviour
// changes; we simply learn that control reached a particular address.
//
// This is the only way to answer "did that branch get taken", because the code
// on the far side of it calls no imports until much later. The alternative is
// to reason about a disassembly and hope, which has already cost this project
// several wrong turns.
//
// IF THIS FILE FAILS TO LINK with an unresolved `__imp__sub_XXXXXXXX`, the
// address is not a function boundary the analyser found — delete the offending
// tripwire rather than trying to force it. Nothing else depends on this file.

#include "ppc_recomp_shared.h"
#include "guest.h"

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace
{

// Report the first call, then every power-of-ten, so a function called once and
// a function called every frame both stay legible.
void Trip(const char* what, std::atomic<uint64_t>& counter, uint32_t caller)
{
    const uint64_t n = counter.fetch_add(1, std::memory_order_relaxed) + 1;

    bool interesting = (n == 1);
    for (uint64_t decade = 10; decade <= 1000000 && !interesting; decade *= 10)
        interesting = (n == decade);

    if (interesting)
        printf("[trace] %s call #%llu (from guest 0x%08X)\n",
            what, (unsigned long long)n, caller);
}

std::atomic<uint64_t> g_present{0};
std::atomic<uint64_t> g_graphicsThreadBody{0};

} // namespace

// ---------------------------------------------------------------------------
// sub_82AC4E48 — the frame-present path.
//
// --xrefs found three callers: 0x82939390, 0x82AB948C and 0x82ACEDD8. The last
// is inside sub_82ACECF0, the function both graphics threads block in, which is
// how we know presentation is gated behind that wait. VdSwap has never been
// called in any run, and the open question is whether that is because this
// function is never reached, or because it is reached and bails out early.
// Those two have completely different fixes, so guessing between them is not
// good enough.
// ---------------------------------------------------------------------------
PPC_FUNC_IMPL(__imp__sub_82AC4E48);
PPC_FUNC(sub_82AC4E48)
{
    Trip("present sub_82AC4E48", g_present, uint32_t(ctx.lr) - 4);
    __imp__sub_82AC4E48(ctx, base);
}

// ---------------------------------------------------------------------------
// sub_82ACECF0 — the graphics thread body.
//
// Two threads run this, one per graphics device context (0x4083FD5C and
// 0x4083FDAC). Its main loop waits on the KEVENT embedded at context+0x20 —
// 0x4083FD7C and 0x4083FDCC respectively, which is exactly the pair the wait
// diagnostics have been naming all along. Tripping the entry confirms how many
// threads actually get here and with which context, without having to infer it
// from a backtrace taken at an unrelated moment.
// ---------------------------------------------------------------------------
PPC_FUNC_IMPL(__imp__sub_82ACECF0);
PPC_FUNC(sub_82ACECF0)
{
    // r3 is the context pointer on entry: 82ACECFC does `mr r26,r3` and every
    // subsequent load is off r26.
    printf("[trace] graphics thread body sub_82ACECF0 entered, context 0x%08X, "
           "event 0x%08X\n", ctx.r3.u32, ctx.r3.u32 + 0x20);
    Trip("sub_82ACECF0", g_graphicsThreadBody, uint32_t(ctx.lr) - 4);
    __imp__sub_82ACECF0(ctx, base);
}

// ---------------------------------------------------------------------------
// The I/O completion chain.
//
// The deadlock is fully mapped except for one link. Two threads wait on five
// events; the only code that signals them is five instructions at 0x82965534,
// guarded by two branches, reached from the request-completion function at
// 0x82965488. The APC that should drive that path is now delivered correctly
// and still nothing signals — so the open question is whether the completion
// runs at all.
//
// Both of these are proven `bl` targets (--xrefs found four callers of
// 0x82965488, and 0x829688C0 calls 0x82968498 directly), so both resolve.
//
// sub_82968498 is what the ApcContext calls. sub_82965488 is the completion
// itself: it looks a request handle up in a table of 112-byte objects and
// signals only if that object is in state 1.
// ---------------------------------------------------------------------------

namespace
{
std::atomic<uint64_t> g_ioCompletion{0};
std::atomic<uint64_t> g_requestComplete{0};
} // namespace

PPC_FUNC_IMPL(__imp__sub_82968498);
PPC_FUNC(sub_82968498)
{
    // r3 = the value read from [0x82F71AE4], r4 = bytes, r5 = error.
    // If r3 is zero the request block was never populated, which would say the
    // ordering is still wrong rather than the completion being unreachable.
    const uint64_t n = g_ioCompletion.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 4)
        printf("[trace] io completion sub_82968498 #%llu: handle 0x%08X, "
               "bytes %u, error 0x%08X\n",
            (unsigned long long)n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
    __imp__sub_82968498(ctx, base);
}

// ---------------------------------------------------------------------------
// The four functions that call sub_82965488.
//
// sub_82968498 was never supposed to reach sub_82965488 — reading it settled
// that. It is a state setter: it moves the file request from state 1 to state
// 2 and returns. sub_82965488 is the consumer on the other side of that
// boundary, reached only from these four call sites:
//
//     82965D90  in sub_82965D58+0x38
//     829661FC  in sub_829660C0+0x13C
//     82966D60  in sub_82966C58+0x108
//     82966F0C  in sub_82966D90+0x17C
//
// The request object is sitting in state 1, which is the state that signals
// immediately. So the question is no longer "what does the completion do" but
// "who is supposed to pump it, and why isn't that running". These four
// tripwires answer whether any of them is reached at all — which decides
// whether to walk up their callers or to look at why a thread that should be
// calling them isn't alive.
// ---------------------------------------------------------------------------

namespace
{
std::atomic<uint64_t> g_pump0{0};
std::atomic<uint64_t> g_pump1{0};
std::atomic<uint64_t> g_pump2{0};
std::atomic<uint64_t> g_pump3{0};
} // namespace

PPC_FUNC_IMPL(__imp__sub_82965D58);
PPC_FUNC(sub_82965D58)
{
    Trip("pump sub_82965D58", g_pump0, uint32_t(ctx.lr) - 4);
    __imp__sub_82965D58(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_829660C0);
PPC_FUNC(sub_829660C0)
{
    Trip("pump sub_829660C0", g_pump1, uint32_t(ctx.lr) - 4);
    __imp__sub_829660C0(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_82966C58);
PPC_FUNC(sub_82966C58)
{
    Trip("pump sub_82966C58", g_pump2, uint32_t(ctx.lr) - 4);
    __imp__sub_82966C58(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_82966D90);
PPC_FUNC(sub_82966D90)
{
    Trip("pump sub_82966D90", g_pump3, uint32_t(ctx.lr) - 4);
    __imp__sub_82966D90(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_82965488);
PPC_FUNC(sub_82965488)
{
    // r3 = the request handle. The state that decides whether it signals lives
    // at [object + 0x34], and the object is found through the table at
    // 0x82F719EC — so print the handle and let the next dump resolve it if this
    // turns out to be reached with a handle that fails validation.
    const uint64_t n = g_requestComplete.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8)
        printf("[trace] request-complete sub_82965488 #%llu: handle 0x%08X "
               "(from guest 0x%08X)\n",
            (unsigned long long)n, ctx.r3.u32, uint32_t(ctx.lr) - 4);
    __imp__sub_82965488(ctx, base);
}

// ---------------------------------------------------------------------------
// sub_82AC46C8 — the vblank handler proper.
//
// The graphics interrupt callback at 0x82AB9840 has two paths:
//
//     source 0 (vblank): gated on [0x7FC86544] bit 0, then bl 0x82AC46C8
//     source 1 (swap):   calls [[userData+0x2A94]+0x10] with context
//                        [[userData+0x2A94]+0x14], then clears bit (1<<cpu)
//                        in [[userData+0x2A94]+0]
//
// We deliver source 0 every vblank and we do set that register bit, so this
// should be running sixty times a second — and it is the only place left that
// could plausibly signal the two context events the graphics threads block on.
//
// "Should be" is exactly the phrasing that has cost this project its worst
// rounds, so measure it rather than reason about it. If this never trips, the
// gate is not open after all and the register write is not doing what the
// comment in video.cpp claims.
// ---------------------------------------------------------------------------
namespace { std::atomic<uint64_t> g_vblankHandler{0}; }

PPC_FUNC_IMPL(__imp__sub_82AC46C8);
PPC_FUNC(sub_82AC46C8)
{
    Trip("vblank handler sub_82AC46C8", g_vblankHandler, uint32_t(ctx.lr) - 4);
    __imp__sub_82AC46C8(ctx, base);
}
