// Win32 window + D3D11 swapchain. See present.h for why this comes first.
//
// D3D11 rather than D3D12 or Vulkan, deliberately: the job here is to get a
// picture on screen with the least machinery between the guest's bytes and the
// display. D3D11 needs no descriptor heaps, no command allocators, no manual
// synchronisation, and its dynamic-texture upload path is three calls. When
// PM4 translation arrives it can target whatever it likes; this layer only has
// to hand over a finished image, and that interface does not change.
//
// Everything here fails soft. A missing display, a driver that will not create
// a device, a window that cannot open — all of them log and leave the runtime
// headless, because this project's whole method depends on being able to take a
// diagnostic run, and a renderer that can abort one is worse than no renderer.

#include "present.h"

#include <cstdio>
#include <mutex>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

namespace wos::gpu
{
namespace
{

// The 360's standard front buffer. The game will tell us the real size through
// SetFrontBuffer; these are only what the window opens at.
constexpr uint32_t kDefaultWidth  = 1280;
constexpr uint32_t kDefaultHeight = 720;

std::mutex g_mutex;

uint8_t* g_guestBase = nullptr;
HWND g_window = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11Texture2D* g_staging = nullptr;      // CPU-writable, guest pixels land here
uint32_t g_stagingWidth = 0;
uint32_t g_stagingHeight = 0;

bool g_running = false;
uint64_t g_frames = 0;

// Where the guest's front buffer lives. Zero until VdSwap tells us.
uint32_t g_frontBuffer = 0;
uint32_t g_frontWidth = 0;
uint32_t g_frontHeight = 0;
uint32_t g_frontPitch = 0;

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // Closing the window must not kill the process: a run in progress is
    // usually collecting something, and losing it to a stray click would be a
    // needless way to waste a build.
    if (msg == WM_CLOSE)
    {
        printf("[present] window close ignored — the run continues headless. "
               "Stop the process to end it.\n");
        return 0;
    }
    if (msg == WM_DESTROY)
    {
        g_window = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool CreateStaging(uint32_t width, uint32_t height)
{
    if (g_staging != nullptr && g_stagingWidth == width && g_stagingHeight == height)
        return true;

    if (g_staging != nullptr)
    {
        g_staging->Release();
        g_staging = nullptr;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &g_staging)))
    {
        printf("[present] could not create a %ux%u staging texture\n", width, height);
        return false;
    }

    g_stagingWidth = width;
    g_stagingHeight = height;
    return true;
}

// Fill one row of the fallback pattern.
//
// The pattern exists so the window is never blank. "Nothing is drawn yet" and
// "the presenter is broken" look identical on a black window, and this project
// has already lost several rounds to exactly that kind of ambiguity. A moving
// gradient says the loop is alive even when the game has handed over nothing.
void WriteTestPatternRow(uint8_t* dst, uint32_t width, uint32_t y, uint64_t frame)
{
    const uint32_t phase = uint32_t(frame) & 0xFF;
    for (uint32_t x = 0; x < width; ++x)
    {
        dst[x * 4 + 0] = uint8_t((x * 255) / (width ? width : 1));        // B
        dst[x * 4 + 1] = uint8_t((y * 255) / (g_stagingHeight ? g_stagingHeight : 1)); // G
        dst[x * 4 + 2] = uint8_t(phase);                                   // R
        dst[x * 4 + 3] = 0xFF;
    }
}

// Copy the guest front buffer into the staging texture.
//
// The 360 stores its front buffer big-endian and usually tiled. Neither is
// handled here yet, and that is deliberate: the untiling and format work
// belongs with the rest of the texture path, and doing it wrong now would
// produce a plausible-looking wrong image, which is harder to debug than an
// obviously wrong one. What this does is a straight byte-swapped copy, so a
// linear untiled buffer comes out correct and a tiled one comes out visibly
// scrambled rather than subtly off.
void CopyGuestFrontBuffer(uint8_t* dst, uint32_t dstPitch)
{
    const uint32_t width = g_frontWidth;
    const uint32_t height = g_frontHeight;
    const uint32_t srcPitch = g_frontPitch ? g_frontPitch : width * 4;

    for (uint32_t y = 0; y < height && y < g_stagingHeight; ++y)
    {
        const uint8_t* src = g_guestBase + g_frontBuffer + size_t(y) * srcPitch;
        uint8_t* row = dst + size_t(y) * dstPitch;

        for (uint32_t x = 0; x < width && x < g_stagingWidth; ++x)
        {
            // Guest is big-endian ARGB; D3D wants little-endian BGRA.
            const uint8_t a = src[x * 4 + 0];
            const uint8_t r = src[x * 4 + 1];
            const uint8_t g = src[x * 4 + 2];
            const uint8_t b = src[x * 4 + 3];
            row[x * 4 + 0] = b;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = r;
            row[x * 4 + 3] = a;
        }
    }
}

} // namespace

bool StartPresenter(uint8_t* guestBase)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_running)
        return true;

    g_guestBase = guestBase;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    // MAKEINTRESOURCEW rather than IDC_ARROW: this project does not define
    // UNICODE, so IDC_ARROW expands to MAKEINTRESOURCEA and will not convert to
    // the LPCWSTR that LoadCursorW wants. Everything else here is explicitly
    // the W variant with L"" literals, so spell this one out too rather than
    // mixing in an A call.
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW
    wc.lpszClassName = L"WoSRecompWindow";
    RegisterClassExW(&wc);

    RECT r = { 0, 0, LONG(kDefaultWidth), LONG(kDefaultHeight) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

    g_window = CreateWindowExW(0, L"WoSRecompWindow",
        L"Spider-Man: Web of Shadows — WoSRecomp",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (g_window == nullptr)
    {
        printf("[present] CreateWindow failed (%lu) — continuing headless\n", GetLastError());
        return false;
    }

    ShowWindow(g_window, SW_SHOW);

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = kDefaultWidth;
    scd.BufferDesc.Height = kDefaultHeight;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = g_window;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL wanted[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got = {};

    const HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        wanted, ARRAYSIZE(wanted), D3D11_SDK_VERSION,
        &scd, &g_swapChain, &g_device, &got, &g_context);

    if (FAILED(hr))
    {
        printf("[present] D3D11CreateDeviceAndSwapChain failed (0x%08lX) — "
               "continuing headless\n", (unsigned long)hr);
        DestroyWindow(g_window);
        g_window = nullptr;
        return false;
    }

    if (!CreateStaging(kDefaultWidth, kDefaultHeight))
        return false;

    g_running = true;
    printf("[present] window open, %ux%u, D3D11 feature level 0x%04X\n",
        kDefaultWidth, kDefaultHeight, unsigned(got));
    printf("[present] showing a test pattern until VdSwap hands over a front buffer\n");
    return true;
}

void SetFrontBuffer(uint32_t guestAddr, uint32_t width, uint32_t height, uint32_t pitch)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    const bool first = (g_frontBuffer == 0);
    g_frontBuffer = guestAddr;
    g_frontWidth = width;
    g_frontHeight = height;
    g_frontPitch = pitch;

    if (first)
        printf("[present] FRONT BUFFER: guest 0x%08X, %ux%u, pitch %u — "
               "presenting real frames now\n", guestAddr, width, height, pitch);
}

bool HasFrontBuffer()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_frontBuffer != 0;
}

void PresentFrame()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_running)
        return;

    // Pump messages here rather than on a dedicated thread: a window whose
    // messages are pumped from the thread that created it needs no cross-thread
    // marshalling, and the vblank thread is already the frame clock.
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(g_context->Map(g_staging, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return;

    uint8_t* dst = static_cast<uint8_t*>(mapped.pData);
    if (g_frontBuffer != 0 && g_guestBase != nullptr)
        CopyGuestFrontBuffer(dst, mapped.RowPitch);
    else
        for (uint32_t y = 0; y < g_stagingHeight; ++y)
            WriteTestPatternRow(dst + size_t(y) * mapped.RowPitch, g_stagingWidth, y, g_frames);

    g_context->Unmap(g_staging, 0);

    // Straight copy into the back buffer. No shaders yet — the staging texture
    // is already in the swapchain's format and size, so a resource copy is both
    // correct and the least code that can be wrong.
    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&backBuffer))))
    {
        g_context->CopyResource(backBuffer, g_staging);
        backBuffer->Release();
    }

    g_swapChain->Present(0, 0);
    ++g_frames;
}

void ShutdownPresenter()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_running = false;
    if (g_staging)   { g_staging->Release();   g_staging = nullptr; }
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context)   { g_context->Release();   g_context = nullptr; }
    if (g_device)    { g_device->Release();    g_device = nullptr; }
    if (g_window)    { DestroyWindow(g_window); g_window = nullptr; }
}

} // namespace wos::gpu

#else   // not _WIN32

// The harness builds and runs on Linux for syntax checking and for the
// analysis tools; there is simply no window there yet.
namespace wos::gpu
{
bool StartPresenter(uint8_t*) { printf("[present] no presenter on this platform\n"); return false; }
void SetFrontBuffer(uint32_t, uint32_t, uint32_t, uint32_t) {}
bool HasFrontBuffer() { return false; }
void PresentFrame() {}
void ShutdownPresenter() {}
} // namespace wos::gpu

#endif
