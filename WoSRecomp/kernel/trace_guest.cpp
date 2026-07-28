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
