// dear imgui: Renderer Backend for Direct3D7
// ===========================================
//
// This backend renders Dear ImGui via legacy Direct3D7 (IDirect3DDevice7 +
// IDirectDraw7). D3D7 has no native scissor test, so we clip triangles
// in software using Sutherland–Hodgman polygon clipping against per-cmd
// clip rectangles. This is mainly for legacy / demo / retro purposes.
//
// Implemented features
// --------------------
//  [X] User texture binding (ImTextureID = LPDIRECTDRAWSURFACE7).
//  [X] Large meshes (ImDrawCmd::VtxOffset) via ImGuiBackendFlags_RendererHasVtxOffset.
//  [X] IMGUI_USE_BGRA_PACKED_COLOR support.
//  [X] Per-command clipping in software (emulates scissor).
//
// Limitations / Notes
// -------------------
//  - D3D7 has no scissor. We do software clipping instead of trying to
//    juggle viewports (which caused “ghost UIs” and artifacts).
//  - Textures must be created with TEXTURE caps. We prefer A8B8G8R8;
//    fall back to A8R8G8B8 (with CPU R/B swap on upload).
//  - Indices must be 16-bit (D3D7 limitation).
//  - This backend is community-level; not officially maintained.
//    Expect fewer tests than modern backends (DX9+, GL, Vulkan).
//
// Basic usage
// -----------
//   ImGui_ImplDX7_Init(d3dDevice, ddraw);
//   ImGui_ImplDX7_CreateDeviceObjects();  // creates font texture
//   ...
//   ImGui_ImplDX7_NewFrame();
//   // build your UI with ImGui
//   ImGui::Render();
//   ImGui_ImplDX7_RenderDrawData(ImGui::GetDrawData());
//   ...
//   ImGui_ImplDX7_InvalidateDeviceObjects(); // before device loss/shutdown
//   ImGui_ImplDX7_Shutdown();
//
// Implementation outline
// ----------------------
//   - Build CPU-side vertex/index arrays (XYZRHW + color + uv).
//   - Transform ImGui positions into framebuffer space
//     using (pos - DisplayPos) * FramebufferScale.
//   - For each ImDrawCmd, clip its triangles to the cmd's ClipRect
//     using Sutherland–Hodgman, then draw the clipped mesh.
//   - Backup/restore a minimal set of D3D7 render states.
//
// ---------------------------------------------------------------------------

#include "imgui.h"
#ifndef IMGUI_DISABLE
#include "imgui_impl_dx7.h"

#include <ddraw.h>
#include <d3d.h>
#include "imgui_internal.h"
#include <vector>
#include <cmath> // fabsf in intersection helper

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wold-style-cast"
#pragma clang diagnostic ignored "-Wsign-conversion"
#endif

//------------------------------------------------------------------------------
// Backend-owned data
//------------------------------------------------------------------------------
struct ImGui_ImplDX7_Data
{
    IDirect3DDevice7* d3d = nullptr; // main D3D7 device
    IDirectDraw7* ddraw = nullptr; // for creating textures

    ImGui_ImplDX7_Data() = default;
};

static ImGui_ImplDX7_Data* ImGui_ImplDX7_GetBackendData()
{
    // Only valid if a Dear ImGui context exists and we attached our pointer.
    return ImGui::GetCurrentContext()
        ? (ImGui_ImplDX7_Data*)ImGui::GetIO().BackendRendererUserData
        : nullptr;
}

//------------------------------------------------------------------------------
// Vertex type we send to D3D7
// - XYZRHW: pre-transformed vertices (no matrices needed)
// - Diffuse color: packed ARGB
// - 1 Texcoord: uv
//------------------------------------------------------------------------------
struct IMGUI_DX7_CUSTOMVERTEX
{
    float    x, y, z, rhw;
    D3DCOLOR col;
    float    u, v;
};
#define IMGUI_DX7_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

// ImGui packs color as ABGR by default unless IMGUI_USE_BGRA_PACKED_COLOR.
// Convert to D3D ARGB if needed.
#ifdef IMGUI_USE_BGRA_PACKED_COLOR
#define IMGUI_COL_TO_DX_ARGB(_COL) (_COL)
#else
#define IMGUI_COL_TO_DX_ARGB(_COL) (((_COL) & 0xFF00FF00) | (((_COL) & 0x00FF0000) >> 16) | (((_COL) & 0x000000FF) << 16))
#endif

//------------------------------------------------------------------------------
// Helpers for software clipping
//------------------------------------------------------------------------------

// Simple lerp for floats.
static inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// A light vertex struct we use while clipping (matches our FVF layout).
struct ClippedVert {
    float    x, y, z, rhw;
    D3DCOLOR col;
    float    u, v;
};

// Return whether a vertex is inside the half-plane of one side of the rect.
// side: 0=left, 1=top, 2=right, 3=bottom
static inline bool InsideBySide(const ClippedVert& p, const ImVec4& R, int side)
{
    switch (side) {
    case 0: return p.x >= R.x; // left boundary: x >= minX
    case 1: return p.y >= R.y; // top boundary:  y >= minY
    case 2: return p.x <= R.z; // right boundary:x <= maxX
    default:return p.y <= R.w; // bottom boundary:y <= maxY
    }
}

// Intersect segment PQ with one side of the rect (see InsideBySide).
// Returns the point along PQ where it hits the boundary line,
// and interpolates z, rhw, uv, color accordingly.
static inline ClippedVert IntersectWithSide(const ClippedVert& P,
    const ClippedVert& Q,
    int side,                // 0=L,1=T,2=R,3=B
    const ImVec4& R)
{
    const float dx = Q.x - P.x;
    const float dy = Q.y - P.y;

    float t = 0.0f, x = 0.0f, y = 0.0f;
    const float EPS = 1e-6f;

    if (side == 0 || side == 2) {              // vertical line: x = const
        x = (side == 0) ? R.x : R.z;
        if (fabsf(dx) < EPS) return P;         // parallel, fallback (won't be used if both are outside)
        t = (x - P.x) / dx;
        y = P.y + t * dy;
    }
    else {                                   // horizontal line: y = const
        y = (side == 1) ? R.y : R.w;
        if (fabsf(dy) < EPS) return P;         // parallel
        t = (y - P.y) / dy;
        x = P.x + t * dx;
    }

    // Defensive clamp to avoid tiny FP drift.
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;

    // Interpolate remaining attributes.
    ClippedVert Rv = P;
    Rv.x = x;            Rv.y = y;
    Rv.z = P.z + t * (Q.z - P.z);
    Rv.rhw = P.rhw + t * (Q.rhw - P.rhw);
    Rv.u = P.u + t * (Q.u - P.u);
    Rv.v = P.v + t * (Q.v - P.v);

    auto lerpB = [&](int a, int b) { return (int)(a + t * (b - a) + 0.5f); };
    int a1 = (P.col >> 24) & 0xFF, r1 = (P.col >> 16) & 0xFF, g1 = (P.col >> 8) & 0xFF, b1 = (P.col) & 0xFF;
    int a2 = (Q.col >> 24) & 0xFF, r2 = (Q.col >> 16) & 0xFF, g2 = (Q.col >> 8) & 0xFF, b2 = (Q.col) & 0xFF;
    Rv.col = (D3DCOLOR)((lerpB(a1, a2) << 24) | (lerpB(r1, r2) << 16) | (lerpB(g1, g2) << 8) | lerpB(b1, b2));

    return Rv;
}

// Clip a single triangle ABC against the rect R.
// Output vertices are appended to out_v, and out_i receives fan triangulation.
// If the triangle is completely outside, nothing is appended.
static void EmitClippedTri(const ClippedVert& a, const ClippedVert& b, const ClippedVert& c,
    const ImVec4& R, std::vector<ClippedVert>& out_v, std::vector<WORD>& out_i)
{
    // Start with the original triangle as a polygon.
    ClippedVert poly[8] = { a, b, c }; int n = 3;

    // Sutherland–Hodgman: successively clip polygon against each rect side.
    for (int side = 0; side < 4 && n; ++side) {
        ClippedVert next[8]; int m = 0;
        for (int i = 0; i < n; ++i) {
            const ClippedVert& P = poly[i];
            const ClippedVert& Q = poly[(i + 1) % n];
            const bool inP = InsideBySide(P, R, side);
            const bool inQ = InsideBySide(Q, R, side);

            if (inP && inQ) {
                // both inside: keep Q
                next[m++] = Q;
            }
            else if (inP && !inQ) {
                // leaving: add intersection
                next[m++] = IntersectWithSide(P, Q, side, R);
            }
            else if (!inP && inQ) {
                // entering: add intersection + Q
                next[m++] = IntersectWithSide(P, Q, side, R);
                next[m++] = Q;
            }
            // both outside: add nothing
        }
        // Continue with the clipped polygon
        memcpy(poly, next, m * sizeof(ClippedVert));
        n = m;
    }

    if (n < 3) return; // fully clipped

    // Triangulate clipped polygon as a fan: (0, i, i+1)
    WORD base = (WORD)out_v.size();
    for (int i = 0; i < n; ++i) out_v.push_back(poly[i]);
    for (int i = 1; i < n - 1; ++i) {
        out_i.push_back(base);
        out_i.push_back(base + i);
        out_i.push_back(base + i + 1);
    }
}

//------------------------------------------------------------------------------
// Minimal D3D7 state backup struct
// - We only back up what we touch and restore it after rendering.
//------------------------------------------------------------------------------
struct ImGui_ImplDX7_StateBackup
{
    D3DMATRIX     world{}, view{}, proj{};
    DWORD         rs_alpha_blend{}, rs_src_blend{}, rs_dst_blend{}, rs_zenable{}, rs_zwrite{}, rs_cullmode{}, rs_lighting{}, rs_shade{};
    DWORD         rs_fog{}, rs_clipping{};
    IDirectDrawSurface7* tex0{};
    DWORD         tss0_colorop{}, tss0_colorarg1{}, tss0_colorarg2{}, tss0_alphaop{}, tss0_alphaarg1{}, tss0_alphaarg2{};
    DWORD         tss1_colorop{}, tss1_alphaop{};
    D3DVIEWPORT7  viewport{}; // included even though we don't change it

    void Capture(IDirect3DDevice7* d3d)
    {
        d3d->GetTransform(D3DTRANSFORMSTATE_WORLD, &world);
        d3d->GetTransform(D3DTRANSFORMSTATE_VIEW, &view);
        d3d->GetTransform(D3DTRANSFORMSTATE_PROJECTION, &proj);

        d3d->GetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, &rs_alpha_blend);
        d3d->GetRenderState(D3DRENDERSTATE_SRCBLEND, &rs_src_blend);
        d3d->GetRenderState(D3DRENDERSTATE_DESTBLEND, &rs_dst_blend);
        d3d->GetRenderState(D3DRENDERSTATE_ZENABLE, &rs_zenable);
        d3d->GetRenderState(D3DRENDERSTATE_ZWRITEENABLE, &rs_zwrite);
        d3d->GetRenderState(D3DRENDERSTATE_CULLMODE, &rs_cullmode);
        d3d->GetRenderState(D3DRENDERSTATE_LIGHTING, &rs_lighting);
        d3d->GetRenderState(D3DRENDERSTATE_SHADEMODE, &rs_shade);
        d3d->GetRenderState(D3DRENDERSTATE_FOGENABLE, &rs_fog);
        d3d->GetRenderState(D3DRENDERSTATE_CLIPPING, &rs_clipping);

        d3d->GetTexture(0, &tex0); // AddRef()'d; must Release() later
        d3d->GetTextureStageState(0, D3DTSS_COLOROP, &tss0_colorop);
        d3d->GetTextureStageState(0, D3DTSS_COLORARG1, &tss0_colorarg1);
        d3d->GetTextureStageState(0, D3DTSS_COLORARG2, &tss0_colorarg2);
        d3d->GetTextureStageState(0, D3DTSS_ALPHAOP, &tss0_alphaop);
        d3d->GetTextureStageState(0, D3DTSS_ALPHAARG1, &tss0_alphaarg1);
        d3d->GetTextureStageState(0, D3DTSS_ALPHAARG2, &tss0_alphaarg2);
        d3d->GetTextureStageState(1, D3DTSS_COLOROP, &tss1_colorop);
        d3d->GetTextureStageState(1, D3DTSS_ALPHAOP, &tss1_alphaop);

        d3d->GetViewport(&viewport);
    }

    void Restore(IDirect3DDevice7* d3d)
    {
        d3d->SetTransform(D3DTRANSFORMSTATE_WORLD, &world);
        d3d->SetTransform(D3DTRANSFORMSTATE_VIEW, &view);
        d3d->SetTransform(D3DTRANSFORMSTATE_PROJECTION, &proj);

        d3d->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, rs_alpha_blend);
        d3d->SetRenderState(D3DRENDERSTATE_SRCBLEND, rs_src_blend);
        d3d->SetRenderState(D3DRENDERSTATE_DESTBLEND, rs_dst_blend);
        d3d->SetRenderState(D3DRENDERSTATE_ZENABLE, rs_zenable);
        d3d->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, rs_zwrite);
        d3d->SetRenderState(D3DRENDERSTATE_CULLMODE, rs_cullmode);
        d3d->SetRenderState(D3DRENDERSTATE_LIGHTING, rs_lighting);
        d3d->SetRenderState(D3DRENDERSTATE_SHADEMODE, rs_shade);
        d3d->SetRenderState(D3DRENDERSTATE_FOGENABLE, rs_fog);
        d3d->SetRenderState(D3DRENDERSTATE_CLIPPING, rs_clipping);

        d3d->SetTexture(0, tex0);
        if (tex0) tex0->Release();

        d3d->SetTextureStageState(0, D3DTSS_COLOROP, tss0_colorop);
        d3d->SetTextureStageState(0, D3DTSS_COLORARG1, tss0_colorarg1);
        d3d->SetTextureStageState(0, D3DTSS_COLORARG2, tss0_colorarg2);
        d3d->SetTextureStageState(0, D3DTSS_ALPHAOP, tss0_alphaop);
        d3d->SetTextureStageState(0, D3DTSS_ALPHAARG1, tss0_alphaarg1);
        d3d->SetTextureStageState(0, D3DTSS_ALPHAARG2, tss0_alphaarg2);
        d3d->SetTextureStageState(1, D3DTSS_COLOROP, tss1_colorop);
        d3d->SetTextureStageState(1, D3DTSS_ALPHAOP, tss1_alphaop);

        d3d->SetViewport(&viewport);
    }
};

//------------------------------------------------------------------------------
// Common render state setup for ImGui rendering
//------------------------------------------------------------------------------
static void ImGui_ImplDX7_SetupRenderState(ImDrawData* /*draw_data*/)
{
    ImGui_ImplDX7_Data* bd = ImGui_ImplDX7_GetBackendData();
    IDirect3DDevice7* d3d = bd->d3d;

    // Disable depth and lighting; enable alpha blending.
    d3d->SetRenderState(D3DRENDERSTATE_ZENABLE, FALSE);
    d3d->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, FALSE);
    d3d->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    d3d->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
    d3d->SetRenderState(D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
    d3d->SetRenderState(D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d3d->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
    d3d->SetRenderState(D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD);
    d3d->SetRenderState(D3DRENDERSTATE_FOGENABLE, FALSE);
    d3d->SetRenderState(D3DRENDERSTATE_CLIPPING, TRUE);

    // Texture pipeline config: modulate texture * vertex color, clamp addressing.
    d3d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    d3d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d3d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d3d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    d3d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d3d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d3d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d3d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    d3d->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTFN_LINEAR);
    d3d->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTFG_LINEAR);
    d3d->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTFP_POINT);
    d3d->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    d3d->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);

    // Identity transforms (we submit XYZRHW so matrices are not used).
    D3DMATRIX I;
    I._11 = 1.0f;  I._12 = 0.0f;  I._13 = 0.0f;  I._14 = 0.0f;
    I._21 = 0.0f;  I._22 = 1.0f;  I._23 = 0.0f;  I._24 = 0.0f;
    I._31 = 0.0f;  I._32 = 0.0f;  I._33 = 1.0f;  I._34 = 0.0f;
    I._41 = 0.0f;  I._42 = 0.0f;  I._43 = 0.0f;  I._44 = 1.0f;
    d3d->SetTransform(D3DTRANSFORMSTATE_WORLD, &I);
    d3d->SetTransform(D3DTRANSFORMSTATE_VIEW, &I);
    d3d->SetTransform(D3DTRANSFORMSTATE_PROJECTION, &I);
}

// Convert RGBA32 -> BGRA32 if needed when uploading the font atlas.
static inline ImU32 ImGui_ImplDX7_RgbaToBgra(ImU32 rgba)
{
#ifndef IMGUI_USE_BGRA_PACKED_COLOR
    return ((rgba & 0xFF00FF00) | ((rgba & 0x00FF0000) >> 16) | ((rgba & 0x000000FF) << 16));
#else
    return rgba;
#endif
}

//------------------------------------------------------------------------------
// Font texture (ImGui atlas) upload to DirectDraw7 texture surface
//------------------------------------------------------------------------------
static IDirectDrawSurface7* g_FontTexture = nullptr;

static bool ImGui_ImplDX7_CreateFontsTexture()
{
    ImGui_ImplDX7_Data* bd = ImGui_ImplDX7_GetBackendData();
    if (!bd || !bd->ddraw) return false;

    ImGuiIO& io = ImGui::GetIO();

    // Ask ImGui for RGBA32 pixels.
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

    // Describe a 32-bit ARGB texture.
    DDSURFACEDESC2 desc{};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.dwWidth = (DWORD)w;
    desc.dwHeight = (DWORD)h;

    // Prefer VRAM, fall back to system memory.
    desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY;

    DDPIXELFORMAT pf{};
    pf.dwSize = sizeof(pf);
    pf.dwFlags = DDPF_ALPHAPIXELS | DDPF_RGB;
    pf.dwRGBBitCount = 32;
    pf.dwRGBAlphaBitMask = 0xFF000000; // A
    pf.dwRBitMask = 0x00FF0000; // R
    pf.dwGBitMask = 0x0000FF00; // G
    pf.dwBBitMask = 0x000000FF; // B
    desc.ddpfPixelFormat = pf;

    if (FAILED(bd->ddraw->CreateSurface(&desc, &g_FontTexture, nullptr)))
    {
        // System memory fallback if VRAM creation failed.
        desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_SYSTEMMEMORY;
        if (FAILED(bd->ddraw->CreateSurface(&desc, &g_FontTexture, nullptr)))
            return false;
    }

    // Lock and copy pixels (RGBA -> ARGB with optional R/B swap).
    RECT r{ 0,0,w,h };
    DDSURFACEDESC2 lockd{}; lockd.dwSize = sizeof(lockd);
    if (FAILED(g_FontTexture->Lock(&r, &lockd, 0, 0)))
    {
        g_FontTexture->Release(); g_FontTexture = nullptr;
        return false;
    }

    const ImU32* src = (const ImU32*)pixels;
    for (int y = 0; y < h; y++)
    {
        ImU32* dst = (ImU32*)((unsigned char*)lockd.lpSurface + y * lockd.lPitch);
        const ImU32* s = src + y * w;
        for (int x = 0; x < w; x++)
            dst[x] = ImGui_ImplDX7_RgbaToBgra(s[x]);
    }
    g_FontTexture->Unlock(nullptr);

    io.Fonts->SetTexID((ImTextureID)(intptr_t)g_FontTexture);
    return true;
}

static void ImGui_ImplDX7_DestroyFontsTexture()
{
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->SetTexID(0);
    if (g_FontTexture) { g_FontTexture->Release(); g_FontTexture = nullptr; }
}

//------------------------------------------------------------------------------
// Public API
//------------------------------------------------------------------------------
bool ImGui_ImplDX7_Init(IDirect3DDevice7* device, IDirectDraw7* ddraw)
{
    ImGuiIO& io = ImGui::GetIO();
    IMGUI_CHECKVERSION();
    IM_ASSERT(io.BackendRendererUserData == nullptr && "Renderer backend already initialized.");

    ImGui_ImplDX7_Data* bd = IM_NEW(ImGui_ImplDX7_Data)();
    io.BackendRendererUserData = (void*)bd;
    io.BackendRendererName = "imgui_impl_dx7";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

    bd->d3d = device; if (bd->d3d)   bd->d3d->AddRef();
    bd->ddraw = ddraw;  if (bd->ddraw) bd->ddraw->AddRef();

    return true;
}

void ImGui_ImplDX7_Shutdown()
{
    ImGui_ImplDX7_Data* bd = ImGui_ImplDX7_GetBackendData();
    IM_ASSERT(bd != nullptr && "No renderer backend to shutdown, or already shutdown?");
    ImGuiIO& io = ImGui::GetIO();

    ImGui_ImplDX7_InvalidateDeviceObjects();

    if (bd->ddraw) { bd->ddraw->Release(); bd->ddraw = nullptr; }
    if (bd->d3d) { bd->d3d->Release();   bd->d3d = nullptr; }

    io.BackendRendererName = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &= ~ImGuiBackendFlags_RendererHasVtxOffset;

    IM_DELETE(bd);
}

bool ImGui_ImplDX7_CreateDeviceObjects()
{
    // Currently we only need to upload the font texture.
    return ImGui_ImplDX7_CreateFontsTexture();
}

void ImGui_ImplDX7_InvalidateDeviceObjects()
{
    ImGui_ImplDX7_DestroyFontsTexture();
}

void ImGui_ImplDX7_NewFrame()
{
    ImGui_ImplDX7_Data* bd = ImGui_ImplDX7_GetBackendData();
    IM_ASSERT(bd != nullptr && "Context or backend not initialized! Did you call ImGui_ImplDX7_Init()?");
    IM_UNUSED(bd);
}

//------------------------------------------------------------------------------
// Main render entry point: converts ImGui draw data to D3D7 calls.
//------------------------------------------------------------------------------
void ImGui_ImplDX7_RenderDrawData(ImDrawData* draw_data)
{
    if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f)
        return;

    // D3D7 DrawIndexedPrimitive expects 16-bit indices (WORD).
    IM_ASSERT(sizeof(ImDrawIdx) == 2 && "D3D7 backend requires 16-bit ImDrawIdx!");

    ImGui_ImplDX7_Data* bd = ImGui_ImplDX7_GetBackendData();
    IDirect3DDevice7* d3d = bd->d3d;

    // Backup application state (we touch a subset).
    ImGui_ImplDX7_StateBackup backup{};
    backup.Capture(d3d);

    // Set render state appropriate for UI.
    ImGui_ImplDX7_SetupRenderState(draw_data);

    // Build CPU-side contiguous vertex & index buffers for the whole frame.
    const int total_vtx = draw_data->TotalVtxCount;
    const int total_idx = draw_data->TotalIdxCount;

    ImVector<IMGUI_DX7_CUSTOMVERTEX> vbuf;
    ImVector<ImDrawIdx>              ibuf;
    vbuf.resize(total_vtx);
    ibuf.resize(total_idx);

    // Transform from ImGui-space to framebuffer-space.
    const ImVec2 clip_off = draw_data->DisplayPos;
    const ImVec2 clip_scale = draw_data->FramebufferScale; // often (1,1)

    IMGUI_DX7_CUSTOMVERTEX* vtx_dst = vbuf.Data;
    ImDrawIdx* idx_dst = ibuf.Data;

    for (int n = 0; n < draw_data->CmdListsCount; n++)
    {
        const ImDrawList* dl = draw_data->CmdLists[n];
        const ImDrawVert* vtx_src = dl->VtxBuffer.Data;

        // Convert vertices: XYZRHW + ARGB + UV
        for (int i = 0; i < dl->VtxBuffer.Size; i++)
        {
            vtx_dst->x = (vtx_src->pos.x - clip_off.x) * clip_scale.x;
            vtx_dst->y = (vtx_src->pos.y - clip_off.y) * clip_scale.y;
            vtx_dst->z = 0.0f;
            vtx_dst->rhw = 1.0f;
            vtx_dst->col = IMGUI_COL_TO_DX_ARGB(vtx_src->col);
            vtx_dst->u = vtx_src->uv.x;
            vtx_dst->v = vtx_src->uv.y;
            ++vtx_dst; ++vtx_src;
        }

        // Copy indices as-is (16-bit).
        memcpy(idx_dst, dl->IdxBuffer.Data, (size_t)dl->IdxBuffer.Size * sizeof(ImDrawIdx));
        idx_dst += dl->IdxBuffer.Size;
    }

    // Running offsets into our contiguous buffers for each draw list.
    int global_vtx_offset = 0;
    int global_idx_offset = 0;

    // Framebuffer size used to clamp clip rects (defensive).
    const int fb_width = (int)(draw_data->DisplaySize.x * clip_scale.x);
    const int fb_height = (int)(draw_data->DisplaySize.y * clip_scale.y);

    // Iterate draw commands and render them.
    for (int n = 0; n < draw_data->CmdListsCount; n++)
    {
        const ImDrawList* dl = draw_data->CmdLists[n];

        for (int cmd_i = 0; cmd_i < dl->CmdBuffer.Size; cmd_i++)
        {
            const ImDrawCmd* pcmd = &dl->CmdBuffer[cmd_i];

            // Handle user callbacks (rare).
            if (pcmd->UserCallback)
            {
                if (pcmd->UserCallback == ImDrawCallback_ResetRenderState)
                {
                    ImGui_ImplDX7_SetupRenderState(draw_data);
                }
                else
                {
                    pcmd->UserCallback(dl, pcmd);
                    // Reset common state after callback so the next draw is stable.
                    ImGui_ImplDX7_SetupRenderState(draw_data);
                    d3d->SetViewport(&backup.viewport);
                }
                continue;
            }

            // Convert the per-cmd clip rect to framebuffer space.
            ImVec2 cr_min = ImVec2((pcmd->ClipRect.x - clip_off.x) * clip_scale.x,
                (pcmd->ClipRect.y - clip_off.y) * clip_scale.y);
            ImVec2 cr_max = ImVec2((pcmd->ClipRect.z - clip_off.x) * clip_scale.x,
                (pcmd->ClipRect.w - clip_off.y) * clip_scale.y);

            // Skip if empty or fully out of bounds (coarse reject).
            if (cr_max.x <= cr_min.x || cr_max.y <= cr_min.y)
                continue;
            if (cr_max.x < 0.0f || cr_max.y < 0.0f || cr_min.x > fb_width || cr_min.y > fb_height)
                continue;

            // Clamp to framebuffer bounds (defensive).
            if (cr_min.x < 0.0f) cr_min.x = 0.0f;
            if (cr_min.y < 0.0f) cr_min.y = 0.0f;
            if (cr_max.x > (float)fb_width)  cr_max.x = (float)fb_width;
            if (cr_max.y > (float)fb_height) cr_max.y = (float)fb_height;

            // Bind the texture for this draw.
            d3d->SetTexture(0, (IDirectDrawSurface7*)pcmd->GetTexID());

            // Compute start pointers into the big buffers for this cmd.
            const IMGUI_DX7_CUSTOMVERTEX* vstart = vbuf.Data + (pcmd->VtxOffset + global_vtx_offset);
            const ImDrawIdx* istart = ibuf.Data + (pcmd->IdxOffset + global_idx_offset);

            // Rect as {minX, minY, maxX, maxY}.
            ImVec4 R = ImVec4(cr_min.x, cr_min.y, cr_max.x, cr_max.y);

            // Temporary clipped buffers (per-draw-call).
            std::vector<ClippedVert> cv;
            std::vector<WORD>        ci;
            cv.reserve(pcmd->ElemCount);
            ci.reserve(pcmd->ElemCount);

            // Convert our FVF vertex to ClippedVert.
            auto toCV = [](const IMGUI_DX7_CUSTOMVERTEX& s) {
                ClippedVert d; d.x = s.x; d.y = s.y; d.z = s.z; d.rhw = s.rhw; d.col = s.col; d.u = s.u; d.v = s.v; return d;
                };

            // Process triangles in this command, clip each, and push to cv/ci.
            for (unsigned t = 0; t < pcmd->ElemCount; t += 3)
            {
                const IMGUI_DX7_CUSTOMVERTEX& A = vstart[istart[t + 0]];
                const IMGUI_DX7_CUSTOMVERTEX& B = vstart[istart[t + 1]];
                const IMGUI_DX7_CUSTOMVERTEX& C = vstart[istart[t + 2]];
                EmitClippedTri(toCV(A), toCV(B), toCV(C), R, cv, ci);
            }

            // Submit clipped triangles (if any).
            if (!ci.empty())
            {
                d3d->DrawIndexedPrimitive(
                    D3DPT_TRIANGLELIST,
                    IMGUI_DX7_FVF,
                    cv.data(), (DWORD)cv.size(),
                    ci.data(), (DWORD)ci.size(),
                    0);
            }
        }

        // Advance the global offsets to next draw list.
        global_idx_offset += dl->IdxBuffer.Size;
        global_vtx_offset += dl->VtxBuffer.Size;
    }

    // Restore application state.
    backup.Restore(d3d);
}

#endif // IMGUI_DISABLE
