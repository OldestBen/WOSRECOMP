// A watch on the three D3D device fields that gate everything.
//
// Two dumps settled where the frame loop stops, and both answers are single
// fields in one guest structure:
//
//   82ACEDB8  lbz     r11,10942(r30)      ; [device+0x2ABE]
//   82ACEDBC  rlwinm. r11,r11,0,30,30     ; test 0x02
//   82ACEDC0  beq     0x82aceddc          ; clear -> skip the present calls
//
// The graphics thread times out of its 30 ms wait about 33 times a second and
// lands on that `beq` every time, so bit 1 of [device+0x2ABE] is the flag that
// says "a frame is ready to present" and nothing sets it. And:
//
//   82AC0C50  lbz     r11,10941(r29)      ; [device+0x2ABD]
//   82AC0C54  rlwinm. r11,r11,0,30,30
//   82AC0C58  bne     -> return 0         ; the only clean exit from the wait
//   82AC0C5C  lwz     r11,10896(r29)      ; [device+0x2A90] -> the polled counter
//   82AC0C68  lwz     r8,0(r11)           ;    ...its value
//
// is the predicate the main thread spins on, which is why the main thread has
// never appeared in the import trace: the whole loop calls nothing.
//
// The device is reachable from a fixed global — `lis r24,-32256` then
// `lwz r11,4516(r24)` is [0x820011A4], and the device is what that points to.
// So all of this is directly observable rather than inferred, which is the
// standard this project has had to learn the hard way.
//
// WHY A SEPARATE FAST THREAD: a flag that is set and cleared inside one frame
// is invisible to a five-second sample. Sampling at 200 us and OR-ing what we
// see into a sticky mask means "this bit was never set once in thirty seconds"
// becomes a claim we can actually make, rather than "we never happened to look
// while it was set".

#include "guest.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace
{

// [0x820011A4] holds a pointer to the pointer to the device.
constexpr uint32_t kDeviceSlot = 0x820011A4;

constexpr uint32_t kFlagsPresentReady = 0x2ABE;  // bit 1 gates the present call
constexpr uint32_t kFlagsWaitExit     = 0x2ABD;  // bit 1 ends the main thread's wait
constexpr uint32_t kFencePointer      = 0x2A90;  // -> the counter the wait polls
constexpr uint32_t kFenceLastSeen     = 0x2A88;
constexpr uint32_t kFenceEnable       = 0x2AFC;

std::atomic<uint32_t> g_device{0};
std::atomic<uint32_t> g_stickyPresentReady{0};   // OR of every value seen
std::atomic<uint32_t> g_stickyWaitExit{0};
std::atomic<uint32_t> g_fencePointer{0};
std::atomic<uint32_t> g_fenceValue{0};
std::atomic<uint32_t> g_fenceMin{0xFFFFFFFFu};
std::atomic<uint32_t> g_fenceMax{0};
std::atomic<uint64_t> g_samples{0};

// Resolve the device, or 0 if the game has not built it yet. Guarded because
// this runs before the guest has necessarily written either pointer.
uint32_t ResolveDevice(uint8_t* base)
{
    const uint32_t slot = wos::LoadU32(base, kDeviceSlot);
    if (slot < 0x1000 || slot >= 0xC0000000u)
        return 0;
    const uint32_t device = wos::LoadU32(base, slot);
    if (device < 0x1000 || device >= 0xC0000000u)
        return 0;
    return device;
}

} // namespace

namespace wos
{

void StartD3DProbe(uint8_t* base)
{
    std::thread([base]
    {
        for (;;)
        {
            const uint32_t device = ResolveDevice(base);
            if (device == 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            if (g_device.exchange(device) != device)
                printf("[d3d] device at guest 0x%08X\n", device);

            const uint8_t presentReady =
                *reinterpret_cast<uint8_t*>(base + device + kFlagsPresentReady);
            const uint8_t waitExit =
                *reinterpret_cast<uint8_t*>(base + device + kFlagsWaitExit);

            g_stickyPresentReady.fetch_or(presentReady, std::memory_order_relaxed);
            g_stickyWaitExit.fetch_or(waitExit, std::memory_order_relaxed);

            const uint32_t fencePtr = LoadU32(base, device + kFencePointer);
            g_fencePointer.store(fencePtr, std::memory_order_relaxed);

            if (fencePtr >= 0x1000 && fencePtr < 0xC0000000u)
            {
                const uint32_t value = LoadU32(base, fencePtr);
                g_fenceValue.store(value, std::memory_order_relaxed);

                uint32_t lo = g_fenceMin.load(std::memory_order_relaxed);
                while (value < lo &&
                       !g_fenceMin.compare_exchange_weak(lo, value)) {}
                uint32_t hi = g_fenceMax.load(std::memory_order_relaxed);
                while (value > hi &&
                       !g_fenceMax.compare_exchange_weak(hi, value)) {}
            }

            g_samples.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }).detach();
}

void ReportD3DProbe(uint8_t* base)
{
    const uint32_t device = g_device.load(std::memory_order_relaxed);
    if (device == 0)
    {
        printf("[d3d] device not constructed yet ([0x%08X] is still empty)\n",
            kDeviceSlot);
        return;
    }

    const uint32_t presentSticky = g_stickyPresentReady.load(std::memory_order_relaxed);
    const uint32_t waitSticky    = g_stickyWaitExit.load(std::memory_order_relaxed);
    const uint32_t fencePtr      = g_fencePointer.load(std::memory_order_relaxed);

    printf("[d3d] device 0x%08X  +2ABE(present-gate) ever=0x%02X bit1=%s  "
           "+2ABD(wait-exit) ever=0x%02X bit1=%s\n",
        device, presentSticky, (presentSticky & 0x02) ? "SEEN" : "never",
        waitSticky, (waitSticky & 0x02) ? "SEEN" : "never");

    printf("[d3d]   fence ptr [+2A90]=0x%08X value=0x%08X (min 0x%08X max 0x%08X)  "
           "[+2A88]=0x%08X [+2AFC]=0x%08X  %llu sample(s)\n",
        fencePtr, g_fenceValue.load(std::memory_order_relaxed),
        g_fenceMin.load(std::memory_order_relaxed),
        g_fenceMax.load(std::memory_order_relaxed),
        LoadU32(base, device + kFenceLastSeen),
        LoadU32(base, device + kFenceEnable),
        (unsigned long long)g_samples.load(std::memory_order_relaxed));
}

} // namespace wos
