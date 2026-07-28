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
