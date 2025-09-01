# D3D7 - Imgui

> Dear ImGui renderer backend for **Direct3D 7** with a minimal Win32 example app.

---

## Table of Contents
- [Overview](#overview)
- [Features](#features)
- [Screenshots](#screenshots)
- [Requirements](#requirements)
- [Getting Started](#getting-started)
  - [Folder Layout](#folder-layout)
  - [Build (Visual Studio)](#build-visual-studio)
  - [Build (CMake, optional)](#build-cmake-optional)
- [How It Works](#how-it-works)
- [Usage in Your App](#usage-in-your-app)
- [Textures](#textures)
- [Resizing & Device Reset](#resizing--device-reset)
- [Limitations & Notes](#limitations--notes)
- [Troubleshooting](#troubleshooting)
- [FAQ](#faq)
- [Roadmap / TODO](#roadmap--todo)
- [License](#license)
- [Acknowledgments](#acknowledgments)

---

## Overview
This repository provides a **legacy** renderer backend for [Dear ImGui](https://github.com/ocornut/imgui) targeting **Direct3D 7** (`IDirect3DDevice7` + `IDirectDraw7`). It also includes a small **Win32 example** that demonstrates creating a D3D7 device, uploading the ImGui font atlas, submitting draw calls, and presenting to the primary surface.

This is primarily for **retro / demo / archival** purposes. Modern projects should prefer DX9+, OpenGL, Vulkan, Metal, or DX12 backends.

## Features
- ✅ User texture binding (`ImTextureID = LPDIRECTDRAWSURFACE7`).
- ✅ Large meshes via `ImGuiBackendFlags_RendererHasVtxOffset`.
- ✅ `IMGUI_USE_BGRA_PACKED_COLOR` supported.
- ✅ **Per-command clipping in software** (emulates scissor using Sutherland–Hodgman polygon clipping against `ImDrawCmd::ClipRect`).

## Screenshots
*(Optional – drop images here of the demo window, FPS overlay, etc.)*

## Requirements
- **OS:** Windows 98/2000/XP and later (tested primarily on modern Windows via legacy SDK headers).
- **Compiler:** MSVC (Visual Studio 2019+ recommended). MinGW may work with compatible headers/libs.
- **SDK/Libraries:**
  - `ddraw.h`, `d3d.h` (DirectX 7 era headers)
  - Link with **`ddraw.lib`** and **`dxguid.lib`**
  - Dear ImGui source (e.g., `ImGui/` folder)

> **Note:** You do **not** need the full “DirectX 7 SDK” if your compiler already ships legacy DirectDraw/Direct3D 7 headers/libs. On modern setups these typically come with the Windows SDK / VS toolchain for compatibility.

## Getting Started

### Folder Layout
```
<root>/
  ImGui/
    imgui.h
    imgui.cpp
    imgui_demo.cpp        (optional)
    imgui_draw.cpp
    imgui_tables.cpp
    imgui_widgets.cpp
    imgui_internal.h
    imgui_impl_win32.h
    imgui_impl_win32.cpp
    imconfig.h (optional)
    # stb_* headers, etc.
  imgui_impl_dx7.h
  imgui_impl_dx7.cpp      # The D3D7 renderer backend
  example_win32_directx7.cpp  # Win32 + D3D7 sample entry point
  imgui.ini               # Runtime settings (generated)
```

### Build (Visual Studio)
1. Create a **Win32** or **Console** C/C++ project.
2. Add **source files** to the project:
   - `imgui_impl_dx7.cpp`
   - `ImGui/imgui_impl_win32.cpp`
   - `example_win32_directx7.cpp`
   - **ImGui core:** `ImGui/imgui.cpp`, `ImGui/imgui_draw.cpp`, `ImGui/imgui_tables.cpp`, `ImGui/imgui_widgets.cpp` *(optional: `ImGui/imgui_demo.cpp`)*
3. **Include directories**:
   - `.` (project root)
   - `ImGui`
4. **Linker → Input → Additional Dependencies**:
   - `ddraw.lib; dxguid.lib;` (plus defaults)
5. **Subsystem**:
   - *Windows* (GUI app) or *Console* — both work with this sample.
6. Build **Win32 (x86)** for maximum compatibility, or **x64** if desired.

### Build (CMake, optional)
```cmake
cmake_minimum_required(VERSION 3.16)
project(D3D7imgui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# WIN32 builds a GUI app without a console. Remove WIN32 to keep a console window.
add_executable(D3D7imgui WIN32
  imgui_impl_dx7.cpp
  example_win32_directx7.cpp
  ImGui/imgui.cpp
  ImGui/imgui_draw.cpp
  ImGui/imgui_tables.cpp
  ImGui/imgui_widgets.cpp
  ImGui/imgui_demo.cpp       # optional: remove if you don't want the demo window
  ImGui/imgui_impl_win32.cpp
)

target_include_directories(D3D7imgui PRIVATE . ImGui)
target_link_libraries(D3D7imgui PRIVATE ddraw dxguid)
```

> **Tip:** If the linker can’t find `ddraw.lib`/`dxguid.lib`, ensure your Windows SDK is installed and the correct VC toolset is active.

## How It Works
- **Vertex format:** Pre-transformed **XYZRHW** with packed **ARGB** color and one set of UVs (`FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1`).
- **Color packing:** ImGui packs ABGR by default; converted to D3D’s ARGB when `IMGUI_USE_BGRA_PACKED_COLOR` is not defined.
- **Software clipping:** D3D7 lacks native scissor testing. Each `ImDrawCmd`’s triangles are clipped against its `ClipRect` via **Sutherland–Hodgman** polygon clipping. The result is submitted with `DrawIndexedPrimitive`.
- **State backup:** Only a minimal set of transforms, render states, texture stage states, and texture bindings are backed up and restored around the ImGui pass.
- **Font texture:** The ImGui font atlas is uploaded into a `IDirectDrawSurface7` texture (prefer **A8B8G8R8**, fallback to **A8R8G8B8** with channel swap on upload).

## Usage in Your App
Minimal integration points (pseudocode):
```cpp
// Init (once)
ImGui::CreateContext();
ImGui_ImplWin32_Init(hwnd);
ImGui_ImplDX7_Init(d3dDevice, ddraw);
ImGui_ImplDX7_CreateDeviceObjects(); // uploads font texture

// Per-frame
ImGui_ImplWin32_NewFrame();
ImGui_ImplDX7_NewFrame();
ImGui::NewFrame();
// ... build your UI ...
ImGui::Render();
ImGui_ImplDX7_RenderDrawData(ImGui::GetDrawData());

// Shutdown
ImGui_ImplDX7_InvalidateDeviceObjects();
ImGui_ImplDX7_Shutdown();
ImGui_ImplWin32_Shutdown();
ImGui::DestroyContext();
```

## Textures
- **ImTextureID** is a raw `LPDIRECTDRAWSURFACE7`. Create textures with **`DDSCAPS_TEXTURE`** (and preferably video memory). The backend assumes 32-bit ARGB if possible.
- For user textures, set `ImGui::Image((ImTextureID)mySurface, ImVec2(w,h));`.

## Resizing & Device Reset
- The example queues resize events (`WM_SIZE`) and recreates the offscreen render target accordingly.
- Before destroying/recreating surfaces or losing the device, call `ImGui_ImplDX7_InvalidateDeviceObjects()`; after re-creation, call `ImGui_ImplDX7_CreateDeviceObjects()`.
- Viewport and basic render states are re-applied after reset.

## Limitations & Notes
- **No hardware scissor:** all clipping is done on CPU; very large UI meshes may cost extra CPU.
- **16-bit indices only:** D3D7 requires `ImDrawIdx` to be 16-bit (asserts if not).
- **Texture formats:** Prefer **A8B8G8R8**, fallback to **A8R8G8B8** with R/B swap during upload.
- **Community-level backend:** Not officially maintained by the ImGui project; fewer tests than DX9+/GL/Vulkan backends.
- **Legacy API:** Expect quirks on modern drivers; vsync and presentation behavior vary.

## Troubleshooting
**The window shows but nothing is rendered**
- Ensure `DrawIndexedPrimitive` is called between `BeginScene`/`EndScene`.
- Verify your `ImGui::Render()` is called and `GetDrawData()` returns non-empty.

**Fonts are garbled or invisible**
- Confirm the font atlas surface is 32-bit ARGB and the R/B channel swap matches your `IMGUI_USE_BGRA_PACKED_COLOR` define.
- Make sure `io.Fonts->SetTexID(g_FontTexture);` is called after uploading.

**Clipping looks wrong / triangles disappear**
- Check the transform to framebuffer space: `(pos - DisplayPos) * FramebufferScale` must be applied.
- Verify the clip rect is clamped to framebuffer bounds before clipping.

**Linker can’t find ddraw/dxguid**
- Install the Windows SDK and ensure your VS toolset uses it. Add `ddraw.lib; dxguid.lib;` to Additional Dependencies.

**Resize flicker or artifacts**
- Ensure you fully release and recreate the offscreen render target and reset viewport states.

## FAQ
**Why D3D7?**  
For fun, education, retro-compatibility, and demos targeting very old hardware/VMs.

**Will this work on Windows 11?**  
Yes, as long as the legacy headers/libs are available and the driver stack cooperates. This is unsupported territory—test on your target machines.

**Can I enable 24/32-bit depth?**  
This sample disables depth entirely; ImGui doesn’t need it. You can enable Z if your app requires it.

## Roadmap / TODO
- Optional: batch software clipping to reduce allocations.
- Optional: add a simple texture helper (create/destroy/update) for user images.
- Optional: support paletted textures for true retro flavor.
- Optional: add vsync / timing controls and FPS cap.

## License
Choose a license that fits your needs (e.g., MIT for your code). Dear ImGui itself is MIT-licensed—respect its license when distributing.

## Acknowledgments
- Omar Cornut and all Dear ImGui contributors.
- Microsoft DirectX team (for keeping legacy headers around).
- Everyone still shipping retro D3D games and tools.
