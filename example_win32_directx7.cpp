// Dear ImGui: standalone example application for DirectX 7 (windowed)

#include "ImGui/imgui.h"
#include "ImGui/imgui_impl_win32.h"
#include "ImGui/imgui_impl_dx7.h"

#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <tchar.h>

#pragma comment(lib, "ddraw.lib")
#pragma comment(lib, "dxguid.lib")

// Data
static IDirectDraw7* g_pDD = nullptr;
static IDirect3D7* g_pD3D = nullptr;
static IDirect3DDevice7* g_pD3DDevice = nullptr;
static IDirectDrawSurface7* g_pPrimary = nullptr;   // Primary surface (front buffer)
static IDirectDrawClipper* g_pClipper = nullptr;
static IDirectDrawSurface7* g_pRenderTarget = nullptr;   // Offscreen render target with DDSCAPS_3DDEVICE
static UINT                 g_ResizeWidth = 0, g_ResizeHeight = 0; // queued resize

// Forward declarations
static bool CreateDeviceD3D7(HWND hWnd, UINT w, UINT h);
static void CleanupDeviceD3D7();
static bool CreateRenderTarget(UINT w, UINT h);
static void DestroyRenderTarget();
static bool ResetDevice(UINT w, UINT h);
static void PresentToPrimary(HWND hWnd);
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Main code
int main(int, char**)
{
    // Create application window
    //ImGui_ImplWin32_EnableDpiAwareness();
WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L,
                  GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
                  _T("ImGui Example DX7"), nullptr };

    RegisterClassEx(&wc);
    HWND hwnd = CreateWindow(wc.lpszClassName, _T("Dear ImGui DirectX7 Example"),
        WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800,
        nullptr, nullptr, wc.hInstance, nullptr);

    // Initialize Direct3D7 (windowed)
    RECT rc; GetClientRect(hwnd, &rc);
    UINT init_w = (UINT)(rc.right - rc.left);
    UINT init_h = (UINT)(rc.bottom - rc.top);
    if (!CreateDeviceD3D7(hwnd, init_w, init_h))
    {
        CleanupDeviceD3D7();
        UnregisterClass(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    ImGui::StyleColorsDark();

    // Backend init
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX7_Init(g_pD3DDevice, g_pDD);

    ImGui_ImplDX7_CreateDeviceObjects(); // creates font texture

    // Our state
    bool  show_demo_window = true;
    bool  show_another_window = false;
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

    // Main loop
    bool done = false;
    while (!done)
    {
        // Pump messages
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done) break;

        // Handle queued window resize
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            ResetDevice(g_ResizeWidth, g_ResizeHeight);
            g_ResizeWidth = g_ResizeHeight = 0;
        }

        // Start the Dear ImGui frame
        ImGui_ImplWin32_NewFrame();
        ImGui_ImplDX7_NewFrame();
        ImGui::NewFrame();

        // Demo UI
        if (show_demo_window)
            ImGui::ShowDemoWindow(&show_demo_window);

        {
            static float f = 0.0f;
            static int counter = 0;

            ImGui::Begin("Hello, DX7!");
            ImGui::Text("This is some useful text.");
            ImGui::Checkbox("Demo Window", &show_demo_window);
            ImGui::Checkbox("Another Window", &show_another_window);
            ImGui::SliderFloat("float", &f, 0.0f, 1.0f);
            ImGui::ColorEdit3("clear color", (float*)&clear_color);
            if (ImGui::Button("Button")) counter++;
            ImGui::SameLine();
            ImGui::Text("counter = %d", counter);
            ImGui::Text("Application average %.3f ms/frame (%.1f FPS)",
                1000.0f / io.Framerate, io.Framerate);
            ImGui::End();
        }

        if (show_another_window)
        {
            ImGui::Begin("Another Window", &show_another_window);
            ImGui::Text("Hello from another window!");
            if (ImGui::Button("Close Me"))
                show_another_window = false;
            ImGui::End();
        }

        // Render
        ImGui::EndFrame();

        // Clear render target (Direct3D7 style)
        // We just draw a big colored quad by clearing via color fills using Blt with COLORFILL on the render target.
        // Simpler: just draw nothing and let ImGui overwrite; but to mimic DX9 sample clear, do a color fill:
        if (g_pRenderTarget)
        {
            DDBLTFX fx = {}; fx.dwSize = sizeof(fx);
            fx.dwFillColor = RGB((int)(clear_color.x * clear_color.w * 255.0f),
                (int)(clear_color.y * clear_color.w * 255.0f),
                (int)(clear_color.z * clear_color.w * 255.0f));
            g_pRenderTarget->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        }

        if (g_pD3DDevice)
        {
            g_pD3DDevice->BeginScene();

            ImGui::Render();
            ImGui_ImplDX7_RenderDrawData(ImGui::GetDrawData());

            g_pD3DDevice->EndScene();
        }

        // Present
        PresentToPrimary(hwnd);
        // Small nap helps old blitters behave nicely
        Sleep(1);
    }

    // Cleanup
    ImGui_ImplDX7_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D7();
    DestroyWindow(hwnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);
    return 0;
}

// Helpers ---------------------------------------------------------------------

static bool CreateDeviceD3D7(HWND hWnd, UINT w, UINT h)
{
    // DirectDraw7
    if (FAILED(DirectDrawCreateEx(nullptr, (void**)&g_pDD, IID_IDirectDraw7, nullptr)))
        return false;

    if (FAILED(g_pDD->SetCooperativeLevel(hWnd, DDSCL_NORMAL)))
        return false;

    // Primary surface
    {
        DDSURFACEDESC2 ddsd = {};
        ddsd.dwSize = sizeof(ddsd);
        ddsd.dwFlags = DDSD_CAPS;
        ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
        if (FAILED(g_pDD->CreateSurface(&ddsd, &g_pPrimary, nullptr)))
            return false;
    }

    // Clipper (so we can blit to window client area)
    if (FAILED(g_pDD->CreateClipper(0, &g_pClipper, nullptr)))
        return false;
    g_pClipper->SetHWnd(0, hWnd);
    g_pPrimary->SetClipper(g_pClipper);

    // IDirect3D7
    if (FAILED(g_pDD->QueryInterface(IID_IDirect3D7, (void**)&g_pD3D)))
        return false;

    // Render target offscreen surface (3D capable)
    if (!CreateRenderTarget(w, h))
        return false;

    // Create device (HAL → TnL HAL → RGB fallback)
    HRESULT hr = g_pD3D->CreateDevice(IID_IDirect3DHALDevice, g_pRenderTarget, &g_pD3DDevice);
    if (FAILED(hr))
        hr = g_pD3D->CreateDevice(IID_IDirect3DTnLHalDevice, g_pRenderTarget, &g_pD3DDevice);
    if (FAILED(hr))
        hr = g_pD3D->CreateDevice(IID_IDirect3DRGBDevice, g_pRenderTarget, &g_pD3DDevice);
    if (FAILED(hr))
        return false;

    // Viewport
    D3DVIEWPORT7 vp = {};
    vp.dwX = 0; vp.dwY = 0; vp.dwWidth = w; vp.dwHeight = h;
    vp.dvMinZ = 0.0f; vp.dvMaxZ = 1.0f;
    g_pD3DDevice->SetViewport(&vp);

    // Disable Z
    g_pD3DDevice->SetRenderState(D3DRENDERSTATE_ZENABLE, FALSE);
    g_pD3DDevice->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, FALSE);

    return true;
}

static bool CreateRenderTarget(UINT w, UINT h)
{
    DestroyRenderTarget();
    if (w == 0) w = 1;
    if (h == 0) h = 1;

    // 1) Get current display format and use it for the RT
    DDSURFACEDESC2 mode = {};
    mode.dwSize = sizeof(mode);
    if (FAILED(g_pDD->GetDisplayMode(&mode)))
        return false;

    // 2) Prepare a descriptor that matches the display PF
    DDSURFACEDESC2 ddsd = {};
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    ddsd.dwWidth = w;
    ddsd.dwHeight = h;
    ddsd.ddpfPixelFormat = mode.ddpfPixelFormat;                   // match desktop
    ddsd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;

    HRESULT hr = g_pDD->CreateSurface(&ddsd, &g_pRenderTarget, nullptr);

    // 3) Fallbacks: drop VIDMEM, then try common 16/32-bit formats explicitly
    if (FAILED(hr)) {
        ddsd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE;
        hr = g_pDD->CreateSurface(&ddsd, &g_pRenderTarget, nullptr);
    }
    if (FAILED(hr)) {
        // Try 16-bit 565
        ddsd.ddpfPixelFormat = {};
        ddsd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
        ddsd.ddpfPixelFormat.dwFlags = DDPF_RGB;
        ddsd.ddpfPixelFormat.dwRGBBitCount = 16;
        ddsd.ddpfPixelFormat.dwRBitMask = 0xF800;
        ddsd.ddpfPixelFormat.dwGBitMask = 0x07E0;
        ddsd.ddpfPixelFormat.dwBBitMask = 0x001F;
        ddsd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
        hr = g_pDD->CreateSurface(&ddsd, &g_pRenderTarget, nullptr);
        if (FAILED(hr)) {
            ddsd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE;
            hr = g_pDD->CreateSurface(&ddsd, &g_pRenderTarget, nullptr);
        }
    }
    if (FAILED(hr)) {
        // Try 32-bit X8R8G8B8 explicitly
        ddsd.ddpfPixelFormat = {};
        ddsd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
        ddsd.ddpfPixelFormat.dwFlags = DDPF_RGB;
        ddsd.ddpfPixelFormat.dwRGBBitCount = 32;
        ddsd.ddpfPixelFormat.dwRBitMask = 0x00FF0000;
        ddsd.ddpfPixelFormat.dwGBitMask = 0x0000FF00;
        ddsd.ddpfPixelFormat.dwBBitMask = 0x000000FF;
        ddsd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
        hr = g_pDD->CreateSurface(&ddsd, &g_pRenderTarget, nullptr);
        if (FAILED(hr)) {
            ddsd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE;
            hr = g_pDD->CreateSurface(&ddsd, &g_pRenderTarget, nullptr);
        }
    }

    return SUCCEEDED(hr);
}

static void DestroyRenderTarget()
{
    if (g_pRenderTarget) { g_pRenderTarget->Release(); g_pRenderTarget = nullptr; }
}

static bool ResetDevice(UINT w, UINT h)
{
    ImGui_ImplDX7_InvalidateDeviceObjects();

    if (!CreateRenderTarget(w, h))
        return false;


    if (g_pD3DDevice) {
        g_pD3DDevice->SetRenderTarget(g_pRenderTarget, 0);

        D3DVIEWPORT7 vp = {};
        vp.dwX = 0; vp.dwY = 0; vp.dwWidth = w; vp.dwHeight = h;
        vp.dvMinZ = 0.0f; vp.dvMaxZ = 1.0f;
        g_pD3DDevice->SetViewport(&vp);
        g_pD3DDevice->SetRenderState(D3DRENDERSTATE_ZENABLE, FALSE);
        g_pD3DDevice->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, FALSE);
    }

    ImGui_ImplDX7_CreateDeviceObjects();
    return true;
}


static void CleanupDeviceD3D7()
{
    DestroyRenderTarget();

    if (g_pD3DDevice) { g_pD3DDevice->Release(); g_pD3DDevice = nullptr; }
    if (g_pD3D) { g_pD3D->Release();       g_pD3D = nullptr; }
    if (g_pPrimary) { g_pPrimary->Release();   g_pPrimary = nullptr; }
    if (g_pClipper) { g_pClipper->Release();   g_pClipper = nullptr; }
    if (g_pDD) { g_pDD->Release();        g_pDD = nullptr; }
}

static void PresentToPrimary(HWND hWnd)
{
    if (!g_pPrimary || !g_pRenderTarget)
        return;

    RECT rc_client; GetClientRect(hWnd, &rc_client);
    POINT pt = { rc_client.left, rc_client.top };
    ClientToScreen(hWnd, &pt);

    RECT dst = { pt.x, pt.y, pt.x + (rc_client.right - rc_client.left), pt.y + (rc_client.bottom - rc_client.top) };
    RECT src = { 0, 0, rc_client.right - rc_client.left, rc_client.bottom - rc_client.top };

    // Blit from offscreen 3D RT to primary surface
    g_pPrimary->Blt(&dst, g_pRenderTarget, &src, DDBLT_WAIT, nullptr);
}

// Win32 message handler -------------------------------------------------------

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
        {
            g_ResizeWidth = (UINT)LOWORD(lParam);
            g_ResizeHeight = (UINT)HIWORD(lParam);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}
