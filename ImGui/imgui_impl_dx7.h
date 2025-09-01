#pragma once
#include "imgui.h"
#ifndef IMGUI_DISABLE

struct IDirect3DDevice7;
struct IDirectDraw7;          // add this forward-declare
struct IDirectDrawSurface7;

IMGUI_IMPL_API bool ImGui_ImplDX7_Init(IDirect3DDevice7* device, IDirectDraw7* ddraw);
IMGUI_IMPL_API void ImGui_ImplDX7_Shutdown();
IMGUI_IMPL_API void ImGui_ImplDX7_NewFrame();
IMGUI_IMPL_API void ImGui_ImplDX7_RenderDrawData(ImDrawData* draw_data);

IMGUI_IMPL_API bool ImGui_ImplDX7_CreateDeviceObjects();
IMGUI_IMPL_API void ImGui_ImplDX7_InvalidateDeviceObjects();

#endif
