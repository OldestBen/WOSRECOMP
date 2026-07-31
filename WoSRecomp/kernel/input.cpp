// Controller input, via XInput.
//
// This is the one part of the runtime where host and guest hardware are the
// same thing. The Xbox 360 pad IS an XInput device, the button bit assignments
// are identical, the thumbstick ranges are identical, and the trigger ranges
// are identical. So the state copy is genuinely a straight copy with a
// byte-swap — not an approximation, not a mapping table.
//
// WHY THIS MATTERS BEFORE THERE IS ANYTHING TO PLAY: an unimplemented import
// is rewritten to nop/nop/nop/blr, so XamInputGetState was returning with r3
// still holding its first argument — the user index, usually 0. Zero is
// ERROR_SUCCESS. The game has therefore been told, every run, that a pad is
// connected and that every button on it is released and every stick centred,
// because the state structure was never written and it read whatever was on
// the stack.
//
// That is worse than reporting no pad. A title sitting at "press START" will
// sit there forever, and a title that polls for a disconnect may take a
// disconnect path on garbage. Reporting the truth — connected with real state,
// or genuinely not connected — is strictly better than either.
//
// XInput is loaded dynamically rather than linked. The DLL name has changed
// three times across Windows versions and SDKs, and a link-time dependency on
// the wrong one is a process that will not start at all; a runtime lookup that
// fails just means no pad.

#include "ppc_recomp_shared.h"
#include "import_log.h"
#include "kernel_overrides.h"
#include "guest.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace
{

// Win32 error codes the XInput API uses, which are also what the guest expects.
constexpr uint32_t kErrorSuccess            = 0u;
constexpr uint32_t kErrorDeviceNotConnected = 1167u;

// The guest's XINPUT_GAMEPAD, 12 bytes, big-endian:
//   +0x00 u16 buttons
//   +0x02 u8  left trigger
//   +0x03 u8  right trigger
//   +0x04 s16 thumb LX     +0x06 s16 thumb LY
//   +0x08 s16 thumb RX     +0x0A s16 thumb RY
//
// XINPUT_STATE is a u32 packet number followed by that, so 16 bytes total.
constexpr uint32_t kGamepadSize = 12;

#ifdef _WIN32

// Mirrors the host XINPUT_STATE. Declared rather than included so this file
// needs no xinput.h, whose location has moved between SDKs.
struct HostGamepad
{
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};

struct HostState
{
    DWORD dwPacketNumber;
    HostGamepad Gamepad;
};

using XInputGetStateFn = DWORD (WINAPI*)(DWORD, HostState*);

XInputGetStateFn LoadXInput()
{
    // Newest first. 1_4 ships with Windows 8 and later; 9_1_0 is the
    // redistributable-free subset present on essentially everything.
    static const wchar_t* kNames[] = {
        L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"
    };

    for (const wchar_t* name : kNames)
    {
        if (HMODULE dll = LoadLibraryW(name))
        {
            auto fn = reinterpret_cast<XInputGetStateFn>(
                reinterpret_cast<void*>(GetProcAddress(dll, "XInputGetState")));
            if (fn != nullptr)
            {
                char narrow[64] = {};
                WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow) - 1,
                    nullptr, nullptr);
                printf("[input] XInput loaded from %s\n", narrow);
                return fn;
            }
        }
    }

    printf("[input] no XInput DLL found — controllers will report as absent\n");
    return nullptr;
}

XInputGetStateFn XInput()
{
    static XInputGetStateFn s_fn = LoadXInput();
    return s_fn;
}

#endif // _WIN32

// Write a gamepad structure into guest memory, big-endian.
void StoreGamepad(uint8_t* base, uint32_t addr,
                  uint16_t buttons, uint8_t lt, uint8_t rt,
                  int16_t lx, int16_t ly, int16_t rx, int16_t ry)
{
    wos::StoreU16(base, addr + 0x00, buttons);
    base[addr + 0x02] = lt;
    base[addr + 0x03] = rt;
    wos::StoreU16(base, addr + 0x04, uint16_t(lx));
    wos::StoreU16(base, addr + 0x06, uint16_t(ly));
    wos::StoreU16(base, addr + 0x08, uint16_t(rx));
    wos::StoreU16(base, addr + 0x0A, uint16_t(ry));
}

void StoreEmptyGamepad(uint8_t* base, uint32_t addr)
{
    for (uint32_t i = 0; i < kGamepadSize; ++i)
        base[addr + i] = 0;
}

// XamInput* take (userIndex, flags, out) in the builds this game was made
// against, but earlier SDKs used (userIndex, out). Rather than commit to one,
// take whichever of r4/r5 looks like a guest pointer. Getting this wrong writes
// the state to address zero or to a flags word, and both are silent.
uint32_t OutParam(uint32_t r4, uint32_t r5)
{
    auto plausible = [](uint32_t a) {
        return (a >= 0x1000 && a < 0xC0000000u);
    };
    if (plausible(r5))
        return r5;
    if (plausible(r4))
        return r4;
    return 0;
}

} // namespace

#ifdef WOS_IMPL_XamInputGetState
// DWORD XamInputGetState(DWORD userIndex, DWORD flags, PXINPUT_STATE state)
PPC_FUNC(__imp__XamInputGetState)
{
    WOS_IMPORT_STUB("XamInputGetState");

    const uint32_t userIndex = ctx.r3.u32;
    const uint32_t out = OutParam(ctx.r4.u32, ctx.r5.u32);

    if (out == 0)
    {
        ctx.r3.u64 = kErrorDeviceNotConnected;
        return;
    }

#ifdef _WIN32
    if (auto getState = XInput(); getState != nullptr && userIndex < 4)
    {
        HostState hs = {};
        if (getState(userIndex, &hs) == ERROR_SUCCESS)
        {
            // The 360 pad and an XInput pad are the same device, so the button
            // bits, stick ranges and trigger ranges all carry across unchanged.
            wos::StoreU32(base, out + 0x00, hs.dwPacketNumber);
            StoreGamepad(base, out + 0x04,
                hs.Gamepad.wButtons, hs.Gamepad.bLeftTrigger, hs.Gamepad.bRightTrigger,
                hs.Gamepad.sThumbLX, hs.Gamepad.sThumbLY,
                hs.Gamepad.sThumbRX, hs.Gamepad.sThumbRY);

            // Announce the first connection, and the first button press. The
            // second one is what tells you input is reaching the game rather
            // than merely being read by us.
            static bool s_announced = false;
            if (!s_announced)
            {
                s_announced = true;
                printf("[input] controller %u connected\n", userIndex);
            }
            static bool s_pressed = false;
            if (!s_pressed && hs.Gamepad.wButtons != 0)
            {
                s_pressed = true;
                printf("[input] first button press seen: buttons 0x%04X\n",
                    hs.Gamepad.wButtons);
            }

            ctx.r3.u64 = kErrorSuccess;
            return;
        }
    }
#endif

    // Not connected. Zero the structure anyway: a caller that ignores the
    // return value — and plenty do — must not read uninitialised guest memory
    // as a stick position.
    wos::StoreU32(base, out + 0x00, 0);
    StoreEmptyGamepad(base, out + 0x04);
    ctx.r3.u64 = kErrorDeviceNotConnected;
}
#endif

#ifdef WOS_IMPL_XamInputGetCapabilities
// DWORD XamInputGetCapabilities(DWORD userIndex, DWORD flags,
//                               PXINPUT_CAPABILITIES caps)
//
// X_INPUT_CAPABILITIES is Type, SubType, Flags, then a gamepad describing which
// controls exist, then a vibration structure describing which motors exist.
PPC_FUNC(__imp__XamInputGetCapabilities)
{
    WOS_IMPORT_STUB("XamInputGetCapabilities");

    const uint32_t userIndex = ctx.r3.u32;
    const uint32_t out = OutParam(ctx.r4.u32, ctx.r5.u32);

    if (out == 0)
    {
        ctx.r3.u64 = kErrorDeviceNotConnected;
        return;
    }

    bool connected = false;
#ifdef _WIN32
    if (auto getState = XInput(); getState != nullptr && userIndex < 4)
    {
        HostState hs = {};
        connected = (getState(userIndex, &hs) == ERROR_SUCCESS);
    }
#endif

    if (!connected)
    {
        for (uint32_t i = 0; i < 20; ++i)
            base[out + i] = 0;
        ctx.r3.u64 = kErrorDeviceNotConnected;
        return;
    }

    base[out + 0x00] = 1;      // Type    = XINPUT_DEVTYPE_GAMEPAD
    base[out + 0x01] = 1;      // SubType = XINPUT_DEVSUBTYPE_GAMEPAD
    wos::StoreU16(base, out + 0x02, 0);   // Flags

    // The capability gamepad is a mask of which controls exist, not a reading.
    // All buttons, both triggers at full range, both sticks at full range.
    StoreGamepad(base, out + 0x04,
        0xFFFF, 0xFF, 0xFF,
        int16_t(0xFFC0), int16_t(0xFFC0), int16_t(0xFFC0), int16_t(0xFFC0));

    wos::StoreU16(base, out + 0x10, 0xFFFF);   // left motor
    wos::StoreU16(base, out + 0x12, 0xFFFF);   // right motor

    ctx.r3.u64 = kErrorSuccess;
}
#endif

#ifdef WOS_IMPL_XamInputSetState
// DWORD XamInputSetState(DWORD userIndex, DWORD flags, PXINPUT_VIBRATION v)
//
// Rumble. Accepted and discarded: driving the motors needs XInputSetState and a
// decision about whether a diagnostic run should be shaking a pad on someone's
// desk. Returning success is right either way — the game only needs to know the
// call was accepted.
PPC_FUNC(__imp__XamInputSetState)
{
    WOS_IMPORT_STUB("XamInputSetState");
    ctx.r3.u64 = kErrorSuccess;
}
#endif

#ifdef WOS_IMPL_XamInputGetKeystroke
// Chatpad / keyboard keystrokes. There are none, and saying so is important:
// the alternative is a caller looping until the queue drains, which never
// happens if we claim a keystroke is always available.
PPC_FUNC(__imp__XamInputGetKeystroke)
{
    WOS_IMPORT_STUB("XamInputGetKeystroke");
    ctx.r3.u64 = kErrorDeviceNotConnected;
}
#endif

#ifdef WOS_IMPL_XamInputGetKeystrokeEx
PPC_FUNC(__imp__XamInputGetKeystrokeEx)
{
    WOS_IMPORT_STUB("XamInputGetKeystrokeEx");
    ctx.r3.u64 = kErrorDeviceNotConnected;
}
#endif
