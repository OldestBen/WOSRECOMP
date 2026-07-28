// The per-thread block reached through r13 — the KPCR.
//
// On Xbox 360, r13 is not a general-purpose register. It holds a pointer to
// the current hardware thread's processor control region, and the kernel sets
// it on every thread. Compilers emit direct loads through it, so it is part of
// the ABI rather than something the game manages itself.
//
// Nothing here ever set it, so r13 was zero on every guest thread, and every
// access through it read the zero page instead. Two pieces of evidence agreed:
//
//   * The disassembly of guest 0x82B13200, which is the whole function:
//         lwz r11, 256(r13)     ; r13 + 0x100 -> current thread block
//         lwz r3,  332(r11)     ; thread + 0x14C
//         blr
//
//   * A harness log line from the same run:
//         [guest page] commit 0x00000000  (first touched 0x00000100, read)
//     Guest address 0x100 is exactly r13 + 0x100 when r13 is 0.
//
// The consequence is not subtle. The GPU watchdog at guest 0x82AC0C10 does:
//
//     r10 = [r13+0x100]           ; thread block
//     r30 = [r10+0x58]            ; "now"
//     ...
//     elapsed = r30 - [waiter+0xC]
//     if (elapsed < 5000) return 1        ; caller loops -> keep waiting
//
// With r13 zero, "now" is 0 on every read, the deadline stored earlier is also
// 0, so elapsed is 0 forever. It can never reach 5000, so the watchdog never
// times out, never calls its timeout handler, and its caller spins for as long
// as the process lives. That is the idle spin we have been chasing.
//
// So: give every guest thread a real KPCR and a real thread block, and keep
// the field at +0x58 advancing.

#include "ppc_recomp_shared.h"
#include "guest.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace wos
{

namespace
{

// Sizes are generous rather than exact. We know two fields the game reads
// (+0x58 and +0x14C of the thread block, +0x100 of the KPCR); we do not have
// the full layout, and over-allocating costs nothing while a short block would
// let an unknown field corrupt whatever follows.
constexpr uint32_t kPcrSize    = 0x1000;
constexpr uint32_t kThreadSize = 0x1000;

constexpr uint32_t kPcrCurrentThread = 0x100;   // KPCR   -> current thread block
constexpr uint32_t kThreadTick       = 0x058;   // thread -> monotonic tick
constexpr uint32_t kThreadSelfId     = 0x14C;   // thread -> value 0x82B13200 returns

std::mutex g_mutex;
std::vector<uint32_t> g_threadBlocks;           // guest addresses to keep ticking
std::atomic<bool> g_tickerRunning{false};

// Keep every thread block's tick field advancing.
//
// This has to be driven from the host. The whole reason the field matters is
// that guest code polls it in a loop that calls nothing at all — no import
// fires, so there is no other moment at which we could refresh it.
//
// The unit is milliseconds. That is an assumption, and a falsifiable one: the
// game compares an elapsed value against 5000, and 5 seconds is the timeout a
// GPU watchdog would use. If the real unit were something else, the deadline
// would simply be reached at a different wall-clock time — the code path is
// the same either way.
void TickerThread(uint8_t* base)
{
    const auto start = std::chrono::steady_clock::now();

    while (g_tickerRunning.load(std::memory_order_relaxed))
    {
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const uint32_t ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (uint32_t block : g_threadBlocks)
                StoreU32(base, block + kThreadTick, ms);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace

uint32_t CreateThreadPcr(uint8_t* base)
{
    const uint32_t pcr = GuestAlloc(base, 0, kPcrSize, 0x1000);
    const uint32_t thread = GuestAlloc(base, 0, kThreadSize, 0x1000);
    if (pcr == 0 || thread == 0)
    {
        printf("[pcr] FAILED to allocate a per-thread block — r13 stays 0 for this thread\n");
        return 0;
    }

    StoreU32(base, pcr + kPcrCurrentThread, thread);
    StoreU32(base, thread + kThreadTick, 0);

    // Give each thread a distinct non-zero identity here.
    //
    // guest 0x82B13200 returns this field, and the watchdog compares it with a
    // value the graphics context recorded earlier from the same function —
    // i.e. "is this the thread that started the wait?". With r13 zero, every
    // thread read 0, so that test was true for all of them. Whatever the field
    // is called, distinct per thread is right and identical for all threads is
    // wrong, so use the block's own address.
    StoreU32(base, thread + kThreadSelfId, thread);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_threadBlocks.push_back(thread);
    }

    if (!g_tickerRunning.exchange(true))
    {
        std::thread(TickerThread, base).detach();
        printf("[pcr] tick thread started (1 ms resolution)\n");
    }

    printf("[pcr] thread block: KPCR 0x%08X, thread 0x%08X\n", pcr, thread);
    return pcr;
}

} // namespace wos
