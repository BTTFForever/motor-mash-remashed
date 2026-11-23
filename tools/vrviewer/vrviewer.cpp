// Motor Mash tabletop VR viewer.
//
// Reads the live scene that MMCDA.dll exports from the running game (shared memory
// "Local\MotorMashVR" + mesh/texture files, see ../mmcda/vrshared.h) and draws it as a
// miniature diorama on a virtual table, in an OpenXR headset (D3D11) and in a desktop
// window. Game ticks arrive at ~30 Hz; poses are interpolated per displayed frame.
//
//   MotorMashVR.exe            headset if an OpenXR runtime is available, plus window
//   MotorMashVR.exe --desktop  window only (orbit camera: drag = orbit, wheel = zoom,
//                              right-click = follow the game camera's view point)
//
// Headset controls: grip (either hand) = grab and move/turn the table,
// right stick up/down = scale, left stick left/right = rotate, A or X = recentre,
// B or Y = cycle follow: off / follow your car / follow and turn with your car.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <vector>
#include <unordered_map>
#include <string>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <share.h>

extern "C" {
#include "../mmcda/vrshared.h"
}

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "openxr_loader.lib")

using namespace DirectX;

static bool g_running, g_xr_ok;   // OpenXR session running / available
static MMVR_SHARED *g_sh;          // shared memory from MMCDA.dll (read/write)
static bool g_follow_vr;           // map slides so the player stays at the table centre
static int g_follow_mode;          // 0 off, 1 follow, 2 follow + turn with the car
static bool g_follow;              // desktop: orbit around the game camera's view point
static DirectX::XMFLOAT3 g_follow_pt; // that point (player car), absolute game coords

template <class T> static void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }
static FILE *g_log;
static void logf(const char *fmt, ...)
{
    va_list a; va_start(a, fmt);
    if (g_log) { vfprintf(g_log, fmt, a); fputc('\n', g_log); fflush(g_log); }
    va_end(a);
}

// ---------------------------------------------------------------------------------------
// D3D11 core
static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static ID3D11VertexShader *g_vs;
static ID3D11PixelShader *g_ps;
static ID3D11InputLayout *g_layout;
static ID3D11Buffer *g_cb, *g_instbuf;
static UINT g_instcap;
static ID3D11SamplerState *g_samp;
static ID3D11RasterizerState *g_rs;
static ID3D11DepthStencilState *g_dss, *g_dss_nowrite;
static ID3D11BlendState *g_blend;
static ID3D11ShaderResourceView *g_white;

struct CB { XMFLOAT4X4 world_to_clip; XMFLOAT4X4 model; XMFLOAT4 color; XMFLOAT4 light; XMFLOAT4 origin;
            XMFLOAT4 bb; XMFLOAT4 cam_right; XMFLOAT4 cam_up; XMFLOAT4 uvx; };

static const char *k_shader = R"(
cbuffer C : register(b0) {
    row_major float4x4 world_to_clip;   // table space -> clip
    row_major float4x4 model;           // absolute game space -> table space
    float4 color;                       // group colour (linear)
    float4 light;                       // xyz direction (table space), w ambient
    float4 origin;                      // xyz added to instance translation
    float4 bb;                          // x: 1 = billboard (decal), y: game units -> metres
    float4 cam_right, cam_up;           // viewer camera axes (table/world space)
    float4 uvx;                         // uv = uv * zw + xy
};
struct VI { float3 p : POSITION; float3 n : NORMAL; float2 uv : TEXCOORD0;
            float4 r0 : INST0; float4 r1 : INST1; float4 r2 : INST2; float4 r3 : INST3; };
struct VO { float4 pos : SV_Position; float3 n : NORMAL; float2 uv : TEXCOORD0; };
Texture2D tex : register(t0);
SamplerState smp : register(s0);
VO vs(VI i) {
    VO o;
    float3 wp = i.p.x * i.r0.xyz + i.p.y * i.r1.xyz + i.p.z * i.r2.xyz + i.r3.xyz + origin.xyz;
    float3 wn = i.n.x * i.r0.xyz + i.n.y * i.r1.xyz + i.n.z * i.r2.xyz;
    float4 tp = mul(float4(wp, 1), model);
    if (bb.x > 0.5) {   // decal: centred on the frame, spanned by the viewer right/up axes
        float k = bb.y;   // decal size is in world units; frame scale does not apply
        tp = mul(float4(i.r3.xyz + origin.xyz, 1), model);
        tp.xyz += (i.p.x * cam_right.xyz + i.p.y * cam_up.xyz) * k;
        wn = float3(0, 1, 0);
    }
    o.pos = mul(tp, world_to_clip);
    o.n = normalize(mul(float4(wn, 0), model).xyz + 1e-6);
    o.uv = i.uv * uvx.zw + uvx.xy;
    return o;
}
float4 ps(VO i) : SV_Target {
    float4 t = tex.Sample(smp, i.uv);
    if (t.a < 0.5) discard;
    t *= color;
    float d = (bb.x > 0.5 || bb.z > 0.5) ? 1 : abs(dot(normalize(i.n), -light.xyz));
    if (bb.z > 0.5) return float4(t.rgb, 1);   // unlit (HUD panels)
    return float4(t.rgb * (light.w + (1 - light.w) * d), color.a);
}
)";

static bool CreatePipeline()
{
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(k_shader, strlen(k_shader), "mm", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err))) {
        logf("vs: %s", err ? (char *)err->GetBufferPointer() : "?"); return false;
    }
    if (FAILED(D3DCompile(k_shader, strlen(k_shader), "mm", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err))) {
        logf("ps: %s", err ? (char *)err->GetBufferPointer() : "?"); return false;
    }
    g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
    g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "INST", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INST", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INST", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INST", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
    };
    g_dev->CreateInputLayout(il, 7, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_layout);
    vsb->Release(); psb->Release();

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(CB); bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&bd, nullptr, &g_cb);

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&sd, &g_samp);

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    g_dev->CreateRasterizerState(&rd, &g_rs);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_LESS;
    g_dev->CreateDepthStencilState(&dd, &g_dss);
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    g_dev->CreateDepthStencilState(&dd, &g_dss_nowrite);
    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_dev->CreateBlendState(&bl, &g_blend);

    UINT white = 0xFFFFFFFF;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 1; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA srd = { &white, 4, 0 };
    ID3D11Texture2D *t = nullptr;
    g_dev->CreateTexture2D(&td, &srd, &t);
    g_dev->CreateShaderResourceView(t, nullptr, &g_white);
    t->Release();
    return g_vs && g_ps && g_layout && g_cb;
}

// ---------------------------------------------------------------------------------------
// Scene assets (per generation)
struct Group { UINT ibase, icount, vbase; XMFLOAT4 color; int tex; bool translucent; };
struct Mesh {
    ID3D11Buffer *vb = nullptr, *ib = nullptr; std::vector<Group> groups; bool tried = false, ok = false;
    bool billboard = false, shadow = false;   // decal sprite / a car's flat translucent shadow quad
    float miny = 0;                           // lowest vertex (model units)
};
static std::vector<Mesh> g_meshes;
static std::unordered_map<int, ID3D11ShaderResourceView *> g_textures;
static std::string g_cache_dir;
static uint32_t g_generation = 0;

static float srgb_to_lin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }

static ID3D11ShaderResourceView *LoadTexture(int id)
{
    auto it = g_textures.find(id);
    if (it != g_textures.end()) return it->second;
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\t%d.bin", g_cache_dir.c_str(), id);
    FILE *f = fopen(path, "rb");
    ID3D11ShaderResourceView *srv = nullptr;
    if (f) {
        MMVR_TEXHDR h;
        if (fread(&h, sizeof h, 1, f) == 1 && h.magic == MMVR_TEX_MAGIC && h.width && h.height && h.width <= 4096 && h.height <= 4096) {
            std::vector<uint8_t> px((size_t)h.width * h.height * 4);
            if (fread(px.data(), 1, px.size(), f) == px.size()) {
                D3D11_TEXTURE2D_DESC td = {};
                td.Width = h.width; td.Height = h.height; td.MipLevels = 0; td.ArraySize = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; td.SampleDesc.Count = 1;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
                ID3D11Texture2D *t = nullptr;
                if (SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &t))) {
                    g_ctx->UpdateSubresource(t, 0, nullptr, px.data(), h.width * 4, 0);
                    g_dev->CreateShaderResourceView(t, nullptr, &srv);
                    if (srv) g_ctx->GenerateMips(srv);
                    t->Release();
                }
            }
        }
        fclose(f);
    }
    g_textures[id] = srv;   // null = missing, don't retry every frame
    return srv;
}

static Mesh *GetMesh(uint32_t id)
{
    if (id >= 100000) return nullptr;
    if (g_meshes.size() <= id) g_meshes.resize(id + 1);
    Mesh &m = g_meshes[id];
    if (m.tried) return m.ok ? &m : nullptr;
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\m%u.bin", g_cache_dir.c_str(), id);
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;              // not written yet: try again next frame
    m.tried = true;
    MMVR_MESHHDR mh;
    std::vector<MMVR_VERTEX> verts;
    std::vector<uint32_t> idx;
    unsigned alpha_min = 255;
    if (fread(&mh, sizeof mh, 1, f) == 1 && mh.magic == MMVR_MESH_MAGIC) {
        m.billboard = (mh.flags & MMVR_MESH_BILLBOARD) != 0;
        for (uint32_t g = 0; g < mh.groups; g++) {
            MMVR_GROUPHDR gh;
            if (fread(&gh, sizeof gh, 1, f) != 1 || gh.vcount > 1000000 || gh.icount > 3000000) break;
            Group gr;
            gr.vbase = (UINT)verts.size(); gr.ibase = (UINT)idx.size(); gr.icount = gh.icount;
            gr.tex = gh.tex;
            gr.translucent = ((gh.color >> 24) & 255) < 0xF0;
            alpha_min = std::min(alpha_min, (gh.color >> 24) & 255);
            float a = ((gh.color >> 24) & 255) / 255.0f;
            gr.color = XMFLOAT4(srgb_to_lin(((gh.color >> 16) & 255) / 255.0f), srgb_to_lin(((gh.color >> 8) & 255) / 255.0f),
                                srgb_to_lin((gh.color & 255) / 255.0f), a);
            size_t v0 = verts.size(), i0 = idx.size();
            verts.resize(v0 + gh.vcount); idx.resize(i0 + gh.icount);
            if (fread(verts.data() + v0, sizeof(MMVR_VERTEX), gh.vcount, f) != gh.vcount) break;
            if (fread(idx.data() + i0, 4, gh.icount, f) != gh.icount) break;
            m.groups.push_back(gr);
        }
    }
    fclose(f);
    if (!verts.empty()) {
        float mn = 1e30f, mx = -1e30f;
        for (auto &v : verts) { mn = std::min(mn, v.p[1]); mx = std::max(mx, v.p[1]); }
        m.miny = mn;
        m.shadow = !m.billboard && m.groups.size() == 1 && verts.size() == 4 && mx - mn < 0.01f && alpha_min < 255;
    }
    if (!verts.empty() && !idx.empty()) {
        D3D11_BUFFER_DESC bd = {};
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.ByteWidth = (UINT)(verts.size() * sizeof(MMVR_VERTEX)); bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd = { verts.data() };
        g_dev->CreateBuffer(&bd, &sd, &m.vb);
        bd.ByteWidth = (UINT)(idx.size() * 4); bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        sd.pSysMem = idx.data();
        g_dev->CreateBuffer(&bd, &sd, &m.ib);
        m.ok = m.vb && m.ib;
    }
    return m.ok ? &m : nullptr;
}

static uint32_t g_texdirty_seen;
static void ReloadDirtyTextures()
{
    if (!g_sh) return;
    uint32_t q = g_sh->texdirty_seq;
    if (q - g_texdirty_seen > 256) g_texdirty_seen = q - 256;
    for (; g_texdirty_seen != q; g_texdirty_seen++) {
        auto it = g_textures.find((int)g_sh->texdirty[g_texdirty_seen % 256]);
        if (it != g_textures.end()) { if (it->second) it->second->Release(); g_textures.erase(it); }
    }
}

static void ResetAssets()
{
    for (auto &m : g_meshes) { SafeRelease(m.vb); SafeRelease(m.ib); }
    g_meshes.clear();
    for (auto &t : g_textures) if (t.second) t.second->Release();
    g_textures.clear();
}

// ---------------------------------------------------------------------------------------
// Live poses from the game
static HANDLE g_map;
struct Snapshot { uint32_t tick = 0; double t = 0; std::vector<MMVR_INST> inst; float ox = 0, oz = 0; float cam[4][3] = {}; };
static Snapshot g_snap[2];          // [0] older, [1] newer
static bool g_have_snap;
static XMFLOAT3 g_center(0, 0, 0);
static float g_extent = 4000;
static XMFLOAT3 g_bmin(0, 0, 0), g_bmax(0, 0, 0);   // tile bounds (absolute game coords)
static float g_floor_y = 0;                          // lowest geometry seen (water beds)

static double NowMs()
{
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / f.QuadPart;
}

static void ComputeBounds(const Snapshot &s)
{
    if (s.inst.empty()) return;
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    for (auto &i : s.inst) {
        float p[3] = { i.m[3][0] + s.ox, i.m[3][1], i.m[3][2] + s.oz };
        for (int k = 0; k < 3; k++) { mn[k] = std::min(mn[k], p[k]); mx[k] = std::max(mx[k], p[k]); }
    }
    g_center = XMFLOAT3((mn[0] + mx[0]) / 2, mn[1], (mn[2] + mx[2]) / 2);
    g_bmin = XMFLOAT3(mn[0] - 64, mn[1], mn[2] - 64); g_bmax = XMFLOAT3(mx[0] + 64, mx[1], mx[2] + 64);
    g_floor_y = 0;
    g_extent = std::max(std::max(mx[0] - mn[0], mx[2] - mn[2]), 100.0f) + 127;
    logf("bounds: centre %.0f %.0f %.0f, extent %.0f (%zu instances)", g_center.x, g_center.y, g_center.z, g_extent, s.inst.size());
}

static void PollGame()
{
    if (!g_sh) {
        g_map = OpenFileMappingA(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, MMVR_MAP_NAME);
        if (!g_map) return;
        g_sh = (MMVR_SHARED *)MapViewOfFile(g_map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(MMVR_SHARED));
        if (!g_sh || g_sh->magic != MMVR_MAGIC || g_sh->version != MMVR_VERSION) {
            if (g_sh) UnmapViewOfFile(g_sh);
            CloseHandle(g_map); g_map = nullptr; g_sh = nullptr; return;
        }
        logf("connected to game");
    }
    uint32_t latest = g_sh->latest % MMVR_SLOTS;
    const MMVR_FRAME &fr = g_sh->frames[latest];
    uint32_t tick = fr.tick;
    if (!tick || (g_have_snap && tick == g_snap[1].tick)) return;
    if (fr.generation != g_generation) {
        ResetAssets();
        g_generation = fr.generation;
        g_cache_dir = g_sh->cache_dir;
        g_have_snap = false;
        g_texdirty_seen = g_sh->texdirty_seq;
        logf("generation %u: %s", g_generation, g_cache_dir.c_str());
    }
    Snapshot s;
    s.tick = tick; s.t = fr.t_ms; s.ox = fr.origin[0]; s.oz = fr.origin[1];
    memcpy(s.cam, fr.cam, sizeof s.cam);
    uint32_t n = std::min<uint32_t>(fr.count, MMVR_MAX_INST);
    s.inst.assign(fr.inst, fr.inst + n);
    if (g_sh->frames[latest].tick != tick) return;   // overwritten while copying
    bool first = !g_have_snap;
    g_snap[0] = first ? s : std::move(g_snap[1]);
    g_snap[1] = std::move(s);
    if (first) { g_snap[0] = g_snap[1]; ComputeBounds(g_snap[1]); }
    g_have_snap = true;
}

// Where the game camera is looking: its forward ray hits the ground (y = 0), absolute coords
static XMFLOAT2 g_heading(0, 1);     // smoothed camera heading on the ground (game x/z)
static void UpdateFollow(float alpha)
{
    if (!g_have_snap) return;
    const Snapshot &a = g_snap[0], &b = g_snap[1];
    float p[3], f[3];
    for (int k = 0; k < 3; k++) {
        p[k] = a.cam[3][k] + (b.cam[3][k] - a.cam[3][k]) * alpha;
        f[k] = a.cam[2][k] + (b.cam[2][k] - a.cam[2][k]) * alpha;   // row 2 = forward (z)
    }
    p[0] += a.ox + (b.ox - a.ox) * alpha; p[2] += a.oz + (b.oz - a.oz) * alpha;
    float hl = sqrtf(f[0] * f[0] + f[2] * f[2]);
    if (hl > 0.15f) {   // not looking straight down: smooth the heading (~0.25 s)
        static double last; double now = NowMs();
        float k = last ? std::min(1.0f, (float)(now - last) / 250.0f) : 1.0f; last = now;
        float hx = g_heading.x + (f[0] / hl - g_heading.x) * k, hz = g_heading.y + (f[2] / hl - g_heading.y) * k;
        float n = sqrtf(hx * hx + hz * hz);
        if (n > 1e-3f) g_heading = XMFLOAT2(hx / n, hz / n);
    }
    if (f[1] > -0.05f) return;
    float t = -p[1] / f[1];
    g_follow_pt = XMFLOAT3(p[0] + f[0] * t, 0, p[2] + f[2] * t);
}

// Interpolated instance list, grouped by mesh
struct InstOut { float r[4][4]; };
static std::vector<std::pair<uint32_t, InstOut>> g_frame_inst;
static Mesh *GetMesh(uint32_t id);

// The game lifts each car shadow quad ~5 units and the car ~10 units above the ground,
// which is invisible from its overhead camera but obvious in VR. Put each shadow just
// above the ground and drop the car next to it so its lowest point touches the ground.
static const float kShadowLift = 5.0f;
static const float kCarLift = 10.5f;
static void GroundCars()
{
    struct Sh { float x, z, ground; };
    std::vector<Sh> sh;
    for (auto &fi : g_frame_inst) {
        Mesh *m = GetMesh(fi.first);
        if (!m || !m->shadow) continue;
        float plane = fi.second.r[3][1] + m->miny * fi.second.r[1][1];
        float ground = plane - kShadowLift;
        sh.push_back({ fi.second.r[3][0], fi.second.r[3][2], ground });
        fi.second.r[3][1] -= kShadowLift - 0.4f;
    }
    if (sh.empty()) return;
    for (auto &fi : g_frame_inst) {
        Mesh *m = GetMesh(fi.first);
        if (!m || m->shadow || m->billboard) continue;
        const InstOut &o = fi.second;
        for (auto &q : sh) {
            float dx = o.r[3][0] - q.x, dz = o.r[3][2] - q.z;
            if (dx * dx + dz * dz > 30.0f * 30.0f) continue;
            // only remove the game's fixed lift (about 10-11 units at rest), so a car
            // in the air during a jump keeps its height above the ground
            float bottom = o.r[3][1] + m->miny * o.r[1][1];
            float drop = std::min(bottom - q.ground, kCarLift);
            if (drop > 0) fi.second.r[3][1] -= drop;
            break;
        }
    }
}

static void BuildInstances(double now)
{
    g_frame_inst.clear();
    if (!g_have_snap) return;
    const Snapshot &a = g_snap[0], &b = g_snap[1];
    double span = b.t - a.t;
    double show = now - (g_sh ? g_sh->tick_ms : 33.0);          // one tick behind: always between a and b
    float alpha = span > 1 ? (float)((show - a.t) / span) : 1.0f;
    alpha = std::min(std::max(alpha, 0.0f), 1.0f);
    UpdateFollow(alpha);
    std::unordered_map<uint64_t, const MMVR_INST *> prev;
    prev.reserve(a.inst.size() * 2);
    for (auto &i : a.inst) prev[((uint64_t)i.key << 32) | i.mesh] = &i;
    for (auto &i : b.inst) {
        InstOut o;
        const MMVR_INST *p = nullptr;
        auto it = prev.find(((uint64_t)i.key << 32) | i.mesh);
        if (it != prev.end()) p = it->second;
        float bx = i.m[3][0] + b.ox, bz = i.m[3][2] + b.oz;
        if (p) {
            float ax = p->m[3][0] + a.ox, az = p->m[3][2] + a.oz;
            float dx = bx - ax, dy = i.m[3][1] - p->m[3][1], dz = bz - az;
            if (dx * dx + dy * dy + dz * dz > 80.0f * 80.0f) p = nullptr;   // teleport / reused frame
            else {
                for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) o.r[r][c] = p->m[r][c] + (i.m[r][c] - p->m[r][c]) * alpha;
                o.r[3][0] = ax + dx * alpha; o.r[3][1] = p->m[3][1] + dy * alpha; o.r[3][2] = az + dz * alpha;
            }
        }
        if (!p) {
            for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) o.r[r][c] = i.m[r][c];
            o.r[3][0] = bx; o.r[3][1] = i.m[3][1]; o.r[3][2] = bz;
        }
        for (int r = 0; r < 4; r++) o.r[r][3] = r == 3 ? 1.0f : 0.0f;
        g_frame_inst.push_back({ i.mesh, o });
    }
    GroundCars();
    for (auto &fi : g_frame_inst) {                  // track the deepest geometry for the tray floor
        Mesh *m = GetMesh(fi.first);
        if (m && !m->billboard && !m->shadow) g_floor_y = std::min(g_floor_y, fi.second.r[3][1] + m->miny * fi.second.r[1][1]);
    }
    std::sort(g_frame_inst.begin(), g_frame_inst.end(), [](auto &x, auto &y) { return x.first < y.first; });
}

// ---------------------------------------------------------------------------------------
// Table placement (table space = metres, y up, right-handed; OpenXR LOCAL space)
static XMFLOAT3 g_table_pos(0, -0.45f, -0.6f);
static float g_table_yaw = 0, g_table_size = 1.0f;     // metres across the whole map

static XMMATRIX ModelMatrix()
{
    float s = g_table_size / g_extent;
    XMFLOAT3 c = g_center;
    if (g_follow && g_follow_vr) c = XMFLOAT3(g_follow_pt.x, g_center.y, g_follow_pt.z);
    // game is left-handed (z forward); mirror z into right-handed space
    XMMATRIX head = XMMatrixIdentity();
    if (g_follow_mode == 2) {
        // turn the map so the car heading points away from you (-z on the table)
        float dx = g_heading.x, dz = -g_heading.y;        // heading after the z mirror
        head = XMMatrixSet(-dz, 0, -dx, 0,  0, 1, 0, 0,  dx, 0, -dz, 0,  0, 0, 0, 1);
    }
    return XMMatrixTranslation(-c.x, -c.y, -c.z) * XMMatrixScaling(s, s, -s) * head *
           XMMatrixRotationY(g_table_yaw) * XMMatrixTranslation(g_table_pos.x, g_table_pos.y, g_table_pos.z);
}

static XMFLOAT4 g_cam_right(1, 0, 0, 0), g_cam_up(0, 1, 0, 0);
static float g_bb_scale;
static bool g_bb_on, g_unlit;
static XMFLOAT4 g_uvx(0, 0, 1, 1);
static void UpdateCB(const XMMATRIX &w2c, const XMMATRIX &model, const XMFLOAT4 &color)
{
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
    CB *c = (CB *)ms.pData;
    XMStoreFloat4x4(&c->world_to_clip, w2c);
    XMStoreFloat4x4(&c->model, model);
    c->color = color;
    XMVECTOR l = XMVector3Normalize(XMVectorSet(0.4f, -1.0f, -0.3f, 0));
    XMStoreFloat4(&c->light, l); c->light.w = 0.45f;
    c->origin = XMFLOAT4(0, 0, 0, 0);
    c->bb = XMFLOAT4(g_bb_on ? 1.0f : 0.0f, g_bb_scale, 0, 0);
    c->cam_right = g_cam_right; c->cam_up = g_cam_up;
    c->uvx = g_uvx; c->bb.z = g_unlit ? 1.0f : 0.0f;
    g_ctx->Unmap(g_cb, 0);
}

static void EnsureInstanceBuffer(UINT n)
{
    if (n <= g_instcap) return;
    SafeRelease(g_instbuf);
    g_instcap = std::max<UINT>(n, 2048);
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = g_instcap * sizeof(InstOut); bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&bd, nullptr, &g_instbuf);
}

// a unit box used for the table top and the controller markers
static ID3D11Buffer *g_box_vb, *g_box_ib;
static void CreateBox()
{
    MMVR_VERTEX v[24]; uint32_t idx[36]; int nv = 0, ni = 0;
    const float n[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
    for (int f = 0; f < 6; f++) {
        XMVECTOR N = XMVectorSet(n[f][0], n[f][1], n[f][2], 0);
        XMVECTOR U = fabsf(n[f][1]) > 0.5f ? XMVectorSet(1, 0, 0, 0) : XMVectorSet(0, 1, 0, 0);
        XMVECTOR V = XMVector3Cross(N, U);
        for (int k = 0; k < 4; k++) {
            float su = (k & 1) ? 0.5f : -0.5f, sv = (k & 2) ? 0.5f : -0.5f;
            XMVECTOR P = N * 0.5f + U * su + V * sv;
            XMStoreFloat3((XMFLOAT3 *)v[nv + k].p, P);
            v[nv + k].n[0] = n[f][0]; v[nv + k].n[1] = n[f][1]; v[nv + k].n[2] = n[f][2];
            v[nv + k].uv[0] = su + 0.5f; v[nv + k].uv[1] = sv + 0.5f;
        }
        uint32_t q[6] = { 0, 1, 2, 2, 1, 3 };
        for (int k = 0; k < 6; k++) idx[ni++] = nv + q[k];
        nv += 4;
    }
    D3D11_BUFFER_DESC bd = {}; bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = sizeof v; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd = { v };
    g_dev->CreateBuffer(&bd, &sd, &g_box_vb);
    bd.ByteWidth = sizeof idx; bd.BindFlags = D3D11_BIND_INDEX_BUFFER; sd.pSysMem = idx;
    g_dev->CreateBuffer(&bd, &sd, &g_box_ib);
}

static void DrawBox(const XMMATRIX &w2c, const XMMATRIX &xf, XMFLOAT4 color)
{
    // instance = identity; model = box transform in table/world space
    InstOut one = {}; one.r[0][0] = one.r[1][1] = one.r[2][2] = one.r[3][3] = 1;
    D3D11_MAPPED_SUBRESOURCE ms;
    EnsureInstanceBuffer(1);
    // use a separate tiny upload so the main instance data stays intact: draw boxes first
    if (FAILED(g_ctx->Map(g_instbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
    memcpy(ms.pData, &one, sizeof one);
    g_ctx->Unmap(g_instbuf, 0);
    UpdateCB(w2c, xf, color);
    UINT strides[2] = { sizeof(MMVR_VERTEX), sizeof(InstOut) }, offs[2] = { 0, 0 };
    ID3D11Buffer *vbs[2] = { g_box_vb, g_instbuf };
    g_ctx->IASetVertexBuffers(0, 2, vbs, strides, offs);
    g_ctx->IASetIndexBuffer(g_box_ib, DXGI_FORMAT_R32_UINT, 0);
    g_ctx->PSSetShaderResources(0, 1, &g_white);
    g_ctx->DrawIndexedInstanced(36, 1, 0, 0, 0);
}

static XMMATRIX g_hand_xf[2];
static bool g_hand_valid[2];
static XMFLOAT3 g_head_pos(0, 0, 0); static float g_head_yaw = 0;
static XMVECTOR g_face_pt;          // panels turn toward this one point (both eyes alike)

// HUD overlay from the game (320 wide), shown in pieces above the controllers
static ID3D11Texture2D *g_hud_tex;
static ID3D11ShaderResourceView *g_hud_srv;
static uint32_t g_hud_seq;
static DWORD g_hud_centre_ms;
static void UpdateHud()
{
    if (!g_sh || g_sh->hud_seq == g_hud_seq || !g_sh->hud_w) return;
    g_hud_seq = g_sh->hud_seq;
    if (!g_hud_tex) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = 320; td.Height = 240; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        g_dev->CreateTexture2D(&td, nullptr, &g_hud_tex);
        if (g_hud_tex) g_dev->CreateShaderResourceView(g_hud_tex, nullptr, &g_hud_srv);
    }
    if (g_hud_tex) g_ctx->UpdateSubresource(g_hud_tex, 0, nullptr, g_sh->hud_rgba, 320 * 4, 0);
    // centre column (start lights, pause menu, messages): show its panel only when it has pixels
    int n = 0;
    for (uint32_t y = 0; y < g_sh->hud_h && y < 240; y++)
        for (int x = 88; x < 230; x++) n += (g_sh->hud_rgba[y * 320 + x] >> 24) != 0;
    if (n > 40) g_hud_centre_ms = GetTickCount();
}

// A quad (unit square in x/y, uv 0..1 top-down) facing `toward`, centred at `pos`
static ID3D11Buffer *g_quad_vb, *g_quad_ib;
static void CreateQuad()
{
    MMVR_VERTEX v[4]; uint32_t idx[6] = { 0, 1, 2, 2, 1, 3 };
    for (int k = 0; k < 4; k++) {
        v[k].p[0] = (k & 1) ? 0.5f : -0.5f; v[k].p[1] = (k & 2) ? -0.5f : 0.5f; v[k].p[2] = 0;
        v[k].n[0] = 0; v[k].n[1] = 0; v[k].n[2] = 1;
        v[k].uv[0] = (k & 1) ? 1.0f : 0.0f; v[k].uv[1] = (k & 2) ? 1.0f : 0.0f;
    }
    D3D11_BUFFER_DESC bd = {}; bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = sizeof v; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd = { v };
    g_dev->CreateBuffer(&bd, &sd, &g_quad_vb);
    bd.ByteWidth = sizeof idx; bd.BindFlags = D3D11_BIND_INDEX_BUFFER; sd.pSysMem = idx;
    g_dev->CreateBuffer(&bd, &sd, &g_quad_ib);
}

static void EnsureInstanceBuffer(UINT n);
static void UpdateCB(const XMMATRIX &w2c, const XMMATRIX &model, const XMFLOAT4 &color);
// HUD piece: pixels (x0,y0)-(x1,y1) of the 320x240 overlay, `height` metres tall
static void DrawHudPiece(const XMMATRIX &w2c, XMVECTOR pos, XMVECTOR eye, int x0, int y0, int x1, int y1, float height)
{
    if (!g_hud_srv || !g_sh) return;
    float w = height * (float)(x1 - x0) / (float)(y1 - y0);
    XMVECTOR f = XMVector3Normalize(XMVectorSetY(eye - pos, 0));     // face the viewer, stay upright
    if (XMVectorGetX(XMVector3LengthSq(f)) < 1e-6f) f = XMVectorSet(0, 0, 1, 0);
    XMVECTOR up = XMVectorSet(0, 1, 0, 0), right = XMVector3Cross(up, f);
    XMMATRIX basis(right, up, f, XMVectorSet(0, 0, 0, 1));
    XMMATRIX xf = XMMatrixScaling(w, height, 1) * basis * XMMatrixTranslationFromVector(pos);
    InstOut one = {}; one.r[0][0] = one.r[1][1] = one.r[2][2] = one.r[3][3] = 1;
    D3D11_MAPPED_SUBRESOURCE ms;
    EnsureInstanceBuffer(1);
    if (FAILED(g_ctx->Map(g_instbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
    memcpy(ms.pData, &one, sizeof one);
    g_ctx->Unmap(g_instbuf, 0);
    g_uvx = XMFLOAT4(x0 / 320.0f, y0 / 240.0f, (x1 - x0) / 320.0f, (y1 - y0) / 240.0f);
    g_unlit = true;
    UpdateCB(w2c, xf, XMFLOAT4(1, 1, 1, 1));
    g_unlit = false; g_uvx = XMFLOAT4(0, 0, 1, 1);
    UINT strides[2] = { sizeof(MMVR_VERTEX), sizeof(InstOut) }, offs[2] = { 0, 0 };
    ID3D11Buffer *vbs[2] = { g_quad_vb, g_instbuf };
    g_ctx->IASetVertexBuffers(0, 2, vbs, strides, offs);
    g_ctx->IASetIndexBuffer(g_quad_ib, DXGI_FORMAT_R32_UINT, 0);
    g_ctx->PSSetShaderResources(0, 1, &g_hud_srv);
    g_ctx->DrawIndexedInstanced(6, 1, 0, 0, 0);
}

// The game's menus (front end), shown as a big screen above the far side of the table
static ID3D11Texture2D *g_scr_tex;
static ID3D11ShaderResourceView *g_scr_srv;
static uint32_t g_scr_seq;
static bool g_scr_visible;
static void UpdateScreen()
{
    g_scr_visible = g_sh && g_sh->screen_w && (GetTickCount() - g_sh->screen_ms) < 600;
    if (!g_scr_visible || g_sh->screen_seq == g_scr_seq) return;
    g_scr_seq = g_sh->screen_seq;
    if (!g_scr_tex) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = 640; td.Height = 480; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        g_dev->CreateTexture2D(&td, nullptr, &g_scr_tex);
        if (g_scr_tex) g_dev->CreateShaderResourceView(g_scr_tex, nullptr, &g_scr_srv);
    }
    if (g_scr_tex) g_ctx->UpdateSubresource(g_scr_tex, 0, nullptr, g_sh->screen_rgba, 640 * 4, 0);
}

static void DrawFloatingPanel(const XMMATRIX &w2c, ID3D11ShaderResourceView *srv, XMFLOAT4 uv, float w, float h);
static void DrawScreenPanel(const XMMATRIX &w2c, const XMMATRIX &view)
{
    // the race HUD's centre column while it shows something (pause menu, countdown, messages)
    if (!g_scr_visible && g_hud_srv && g_sh && g_sh->hud_w && GetTickCount() - g_hud_centre_ms < 300) {
        float hh = (float)g_sh->hud_h;
        DrawFloatingPanel(w2c, g_hud_srv, XMFLOAT4(88 / 320.0f, 0, 142 / 320.0f, hh / 240.0f), 0.5f, 0.5f * hh / 142.0f);
    }
    if (!g_scr_visible || !g_scr_srv) return;
    DrawFloatingPanel(w2c, g_scr_srv, XMFLOAT4(0, 0, g_sh->screen_w / 640.0f, g_sh->screen_h / 480.0f), 1.0f, 0.75f);
}

// a panel standing above the far side of the table, turned toward the viewer
static void DrawFloatingPanel(const XMMATRIX &w2c, ID3D11ShaderResourceView *srv, XMFLOAT4 uv, float w, float h)
{
    XMVECTOR eye = g_face_pt;
    XMVECTOR pos = XMVector3TransformCoord(XMVectorSet(0, 0.05f + h / 2, -0.55f, 1),
                                           XMMatrixRotationY(g_table_yaw) *
                                           XMMatrixTranslation(g_table_pos.x, g_table_pos.y, g_table_pos.z));
    XMVECTOR f = XMVector3Normalize(XMVectorSetY(eye - pos, 0));
    XMVECTOR up = XMVectorSet(0, 1, 0, 0), right = XMVector3Cross(up, f);
    XMMATRIX xf = XMMatrixScaling(w, h, 1) * XMMATRIX(right, up, f, XMVectorSet(0, 0, 0, 1)) * XMMatrixTranslationFromVector(pos);
    InstOut one = {}; one.r[0][0] = one.r[1][1] = one.r[2][2] = one.r[3][3] = 1;
    D3D11_MAPPED_SUBRESOURCE ms;
    EnsureInstanceBuffer(1);
    if (FAILED(g_ctx->Map(g_instbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
    memcpy(ms.pData, &one, sizeof one);
    g_ctx->Unmap(g_instbuf, 0);
    g_uvx = uv;
    g_unlit = true;
    UpdateCB(w2c, xf, XMFLOAT4(1, 1, 1, 1));
    g_unlit = false; g_uvx = XMFLOAT4(0, 0, 1, 1);
    UINT strides[2] = { sizeof(MMVR_VERTEX), sizeof(InstOut) }, offs[2] = { 0, 0 };
    ID3D11Buffer *vbs[2] = { g_quad_vb, g_instbuf };
    g_ctx->IASetVertexBuffers(0, 2, vbs, strides, offs);
    g_ctx->IASetIndexBuffer(g_quad_ib, DXGI_FORMAT_R32_UINT, 0);
    g_ctx->PSSetShaderResources(0, 1, &srv);
    g_ctx->DrawIndexedInstanced(6, 1, 0, 0, 0);
}

static void DrawHudPanels(const XMMATRIX &w2c, const XMMATRIX &view)
{
    DrawScreenPanel(w2c, view);
    if (!g_hud_srv || !g_sh || !g_sh->hud_w) return;
    int hh = (int)g_sh->hud_h;
    XMVECTOR eye = g_face_pt;
    // left controller: the racers column (x < 88); right: lap counter (top) and timer (bottom)
    if (g_hand_valid[0])
        DrawHudPiece(w2c, g_hand_xf[0].r[3] + XMVectorSet(0, 0.11f, 0, 0), eye, 0, 0, 88, hh, 0.15f);
    if (g_hand_valid[1]) {
        DrawHudPiece(w2c, g_hand_xf[1].r[3] + XMVectorSet(0, 0.10f, 0, 0), eye, 230, 0, 320, hh / 4, 0.04f);
        DrawHudPiece(w2c, g_hand_xf[1].r[3] + XMVectorSet(0, 0.06f, 0, 0), eye, 230, hh - hh / 4, 320, hh, 0.04f);
    }
}

static void DrawScene(const XMMATRIX &w2c, const XMMATRIX &view)
{
    XMMATRIX iv = XMMatrixInverse(nullptr, view);           // camera -> world: rows are its axes
    XMStoreFloat4(&g_cam_right, iv.r[0]); XMStoreFloat4(&g_cam_up, iv.r[1]);
    g_bb_scale = g_table_size / g_extent;
    g_bb_on = false;
    g_ctx->IASetInputLayout(g_layout);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->PSSetShader(g_ps, nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &g_cb);
    g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
    g_ctx->PSSetSamplers(0, 1, &g_samp);
    g_ctx->RSSetState(g_rs);
    g_ctx->OMSetDepthStencilState(g_dss, 0);

    // the map sits in a tray: floor at the deepest geometry (river beds), walls up to ground level
    {
        XMMATRIX model = ModelMatrix();
        float x0 = g_bmin.x, x1 = g_bmax.x, z0 = g_bmin.z, z1 = g_bmax.z, y0 = g_floor_y - 4, y1 = g_center.y;
        float th = 24;                                    // wall thickness, game units
        auto box = [&](float ax, float ay, float az, float bx, float by, float bz, XMFLOAT4 c) {
            XMMATRIX xf = XMMatrixScaling(bx - ax, by - ay, bz - az) *
                          XMMatrixTranslation((ax + bx) / 2, (ay + by) / 2, (az + bz) / 2) * model;
            DrawBox(w2c, xf, c);
        };
        XMFLOAT4 wood(0.09f, 0.06f, 0.04f, 1), side(0.13f, 0.09f, 0.06f, 1);
        box(x0 - th, y0 - 30, z0 - th, x1 + th, y0, z1 + th, wood);           // floor
        box(x0 - th, y0, z0 - th, x0, y1, z1 + th, side);                     // walls
        box(x1, y0, z0 - th, x1 + th, y1, z1 + th, side);
        box(x0, y0, z0 - th, x1, y1, z0, side);
        box(x0, y0, z1, x1, y1, z1 + th, side);
    }
    for (int h = 0; h < 2; h++)
        if (g_hand_valid[h]) DrawBox(w2c, XMMatrixScaling(0.03f, 0.03f, 0.08f) * g_hand_xf[h], XMFLOAT4(0.6f, 0.6f, 0.7f, 1));
    DrawHudPanels(w2c, view);

    g_bb_on = false;
    if (g_frame_inst.empty()) return;
    EnsureInstanceBuffer((UINT)g_frame_inst.size());
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(g_ctx->Map(g_instbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
    InstOut *dst = (InstOut *)ms.pData;
    for (size_t i = 0; i < g_frame_inst.size(); i++) dst[i] = g_frame_inst[i].second;
    g_ctx->Unmap(g_instbuf, 0);

    XMMATRIX model = ModelMatrix();
    for (int pass = 0; pass < 2; pass++) {          // 0: opaque, 1: translucent (blended, no depth write)
        if (pass == 1) { g_ctx->OMSetBlendState(g_blend, nullptr, 0xFFFFFFFF); g_ctx->OMSetDepthStencilState(g_dss_nowrite, 0); }
        size_t i = 0;
        while (i < g_frame_inst.size()) {
            uint32_t id = g_frame_inst[i].first;
            size_t j = i;
            while (j < g_frame_inst.size() && g_frame_inst[j].first == id) j++;
            Mesh *m = GetMesh(id);
            if (m) {
                UINT strides[2] = { sizeof(MMVR_VERTEX), sizeof(InstOut) }, offs[2] = { 0, 0 };
                ID3D11Buffer *vbs[2] = { m->vb, g_instbuf };
                g_ctx->IASetVertexBuffers(0, 2, vbs, strides, offs);
                g_ctx->IASetIndexBuffer(m->ib, DXGI_FORMAT_R32_UINT, 0);
                for (auto &g : m->groups) {
                    if (g.translucent != (pass == 1)) continue;
                    ID3D11ShaderResourceView *srv = g.tex >= 0 ? LoadTexture(g.tex) : nullptr;
                    if (!srv) srv = g_white;
                    g_bb_on = m->billboard;
                    UpdateCB(w2c, model, g.color);
                    g_ctx->PSSetShaderResources(0, 1, &srv);
                    g_ctx->DrawIndexedInstanced(g.icount, (UINT)(j - i), g.ibase, g.vbase, (UINT)i);
                }
            }
            i = j;
        }
    }
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(g_dss, 0);
}

// ---------------------------------------------------------------------------------------
// Desktop window (orbit camera)
static HWND g_wnd;
static IDXGISwapChain1 *g_swap;
static ID3D11RenderTargetView *g_win_rtv;
static ID3D11DepthStencilView *g_win_dsv;
static UINT g_win_w = 1280, g_win_h = 720;
static float g_orbit_yaw = 0.6f, g_orbit_pitch = 0.75f, g_orbit_dist = 1.6f;
static bool g_follow_env = getenv("MMVR_FOLLOW") != nullptr;
static POINT g_drag; static bool g_dragging;
static bool g_quit;

static void CreateWindowTargets()
{
    SafeRelease(g_win_rtv); SafeRelease(g_win_dsv);
    ID3D11Texture2D *bb = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    D3D11_RENDER_TARGET_VIEW_DESC rd = {}; rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    g_dev->CreateRenderTargetView(bb, &rd, &g_win_rtv);
    bb->Release();
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = g_win_w; td.Height = g_win_h; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D *d = nullptr;
    g_dev->CreateTexture2D(&td, nullptr, &d);
    g_dev->CreateDepthStencilView(d, nullptr, &g_win_dsv);
    d->Release();
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CLOSE: g_quit = true; return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SIZE:
        if (g_swap && LOWORD(l) && HIWORD(l)) {
            g_win_w = LOWORD(l); g_win_h = HIWORD(l);
            SafeRelease(g_win_rtv); SafeRelease(g_win_dsv);
            g_swap->ResizeBuffers(0, g_win_w, g_win_h, DXGI_FORMAT_UNKNOWN, 0);
            CreateWindowTargets();
        }
        return 0;
    case WM_LBUTTONDOWN: g_dragging = true; g_drag = { (short)LOWORD(l), (short)HIWORD(l) }; SetCapture(h); return 0;
    case WM_LBUTTONUP: g_dragging = false; ReleaseCapture(); return 0;
    case WM_MOUSEMOVE:
        if (g_dragging) {
            int x = (short)LOWORD(l), y = (short)HIWORD(l);
            g_orbit_yaw += (x - g_drag.x) * 0.006f;
            g_orbit_pitch = std::min(1.5f, std::max(0.05f, g_orbit_pitch + (y - g_drag.y) * 0.006f));
            g_drag = { x, y };
        }
        return 0;
    case WM_RBUTTONDOWN: g_follow = !g_follow; return 0;
    case WM_MOUSEWHEEL: g_orbit_dist *= (GET_WHEEL_DELTA_WPARAM(w) > 0) ? 0.9f : 1.1f; return 0;
    case WM_KEYDOWN: if (w == VK_ESCAPE) g_quit = true; return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static void RenderWindow()
{
    float clear[4] = { 0.02f, 0.02f, 0.03f, 1 };
    g_ctx->ClearRenderTargetView(g_win_rtv, clear);
    g_ctx->ClearDepthStencilView(g_win_dsv, D3D11_CLEAR_DEPTH, 1, 0);
    g_ctx->OMSetRenderTargets(1, &g_win_rtv, g_win_dsv);
    D3D11_VIEWPORT vp = { 0, 0, (float)g_win_w, (float)g_win_h, 0, 1 };
    g_ctx->RSSetViewports(1, &vp);
    XMVECTOR target = XMVectorSet(g_table_pos.x, g_table_pos.y, g_table_pos.z, 1);
    if (getenv("MMVR_TARGET") && g_have_snap) {   // test: orbit an absolute game position "x,y,z"
        float tx = 0, ty = 0, tz = 0;
        sscanf_s(getenv("MMVR_TARGET"), "%f,%f,%f", &tx, &ty, &tz);
        target = XMVector3TransformCoord(XMVectorSet(tx, ty, tz, 1), ModelMatrix());
    } else if (g_follow && g_have_snap && !g_follow_vr)
        target = XMVector3TransformCoord(XMVectorSet(g_follow_pt.x, g_follow_pt.y, g_follow_pt.z, 1), ModelMatrix());
    XMVECTOR eye = target + XMVectorSet(sinf(g_orbit_yaw) * cosf(g_orbit_pitch), sinf(g_orbit_pitch),
                                        cosf(g_orbit_yaw) * cosf(g_orbit_pitch), 0) * g_orbit_dist;
    XMMATRIX view = XMMatrixLookAtRH(eye, target, XMVectorSet(0, 1, 0, 0));
    XMVECTOR saved_face = g_face_pt;
    g_face_pt = eye;
    XMMATRIX proj = XMMatrixPerspectiveFovRH(XMConvertToRadians(50), (float)g_win_w / g_win_h, 0.02f, 50.0f);
    DrawScene(view * proj, view);
    g_face_pt = saved_face;
    // While the headset runs, OpenXR paces the loop: never wait for the desktop monitor
    // (a 75 Hz screen would otherwise hold a 120 Hz headset back).
    g_swap->Present(g_running ? 0 : 1, g_running ? DXGI_PRESENT_DO_NOT_WAIT : 0);
}

// ---------------------------------------------------------------------------------------
// OpenXR
static XrInstance g_xr = XR_NULL_HANDLE;
static XrSystemId g_sys = XR_NULL_SYSTEM_ID;
static XrSession g_sess = XR_NULL_HANDLE;
static XrSpace g_space = XR_NULL_HANDLE;
struct EyeChain { XrSwapchain sc; int w, h; std::vector<ID3D11RenderTargetView *> rtv; ID3D11DepthStencilView *dsv; };
static EyeChain g_eye[2];
static XrActionSet g_aset;
static XrAction g_act_pose, g_act_squeeze, g_act_stick, g_act_recenter, g_act_follow, g_act_trigger,
                g_act_fire, g_act_action, g_act_esc;
static XrPath g_hand_path[2];
static XrSpace g_hand_space[2];

#define XRCHK(x) do { XrResult r_ = (x); if (XR_FAILED(r_)) { logf("%s failed: %d", #x, (int)r_); return false; } } while (0)

static XrPath Path(const char *s) { XrPath p = XR_NULL_PATH; xrStringToPath(g_xr, s, &p); return p; }

static bool InitXrInstance()
{
    const char *ext[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy_s(ci.applicationInfo.applicationName, "Motor Mash VR");
    ci.applicationInfo.applicationVersion = 1;
    strcpy_s(ci.applicationInfo.engineName, "mmvr");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = 1; ci.enabledExtensionNames = ext;
    XRCHK(xrCreateInstance(&ci, &g_xr));
    XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
    xrGetInstanceProperties(g_xr, &ip);
    logf("OpenXR runtime: %s", ip.runtimeName);
    return true;
}

static bool GetXrSystem()
{
    XrSystemGetInfo sg = { XR_TYPE_SYSTEM_GET_INFO }; sg.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult r = xrGetSystem(g_xr, &sg, &g_sys);
    if (XR_FAILED(r)) { static XrResult last; if (r != last) logf("headset not available yet (%d), retrying", (int)r); last = r; return false; }
    // the spec requires this call before xrCreateSession
    PFN_xrGetD3D11GraphicsRequirementsKHR req = nullptr;
    xrGetInstanceProcAddr(g_xr, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction *)&req);
    XrGraphicsRequirementsD3D11KHR gr = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    if (req) req(g_xr, g_sys, &gr);
    return true;
}

static bool CreateDevice()
{
    IDXGIAdapter1 *adapter = nullptr;
    if (g_xr && g_sys != XR_NULL_SYSTEM_ID) {
        PFN_xrGetD3D11GraphicsRequirementsKHR req = nullptr;
        xrGetInstanceProcAddr(g_xr, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction *)&req);
        XrGraphicsRequirementsD3D11KHR gr = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
        if (req && XR_SUCCEEDED(req(g_xr, g_sys, &gr))) {
            IDXGIFactory1 *fac = nullptr;
            CreateDXGIFactory1(IID_PPV_ARGS(&fac));
            for (UINT i = 0; fac && fac->EnumAdapters1(i, &adapter) == S_OK; i++) {
                DXGI_ADAPTER_DESC1 d; adapter->GetDesc1(&d);
                if (!memcmp(&d.AdapterLuid, &gr.adapterLuid, sizeof(LUID))) break;
                SafeRelease(adapter);
            }
            SafeRelease(fac);
        }
    }
    D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 2, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx);
    SafeRelease(adapter);
    return SUCCEEDED(hr);
}

static bool InitXrSession()
{
    XrGraphicsBindingD3D11KHR gb = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR }; gb.device = g_dev;
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO }; sci.next = &gb; sci.systemId = g_sys;
    XRCHK(xrCreateSession(g_xr, &sci, &g_sess));
    XrReferenceSpaceCreateInfo rs = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; rs.poseInReferenceSpace.orientation.w = 1;
    XRCHK(xrCreateReferenceSpace(g_sess, &rs, &g_space));

    uint32_t nv = 0;
    XrViewConfigurationView vcv[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    XRCHK(xrEnumerateViewConfigurationViews(g_xr, g_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv, vcv));
    uint32_t nf = 0;
    xrEnumerateSwapchainFormats(g_sess, 0, &nf, nullptr);
    std::vector<int64_t> fmts(nf);
    xrEnumerateSwapchainFormats(g_sess, nf, &nf, fmts.data());
    int64_t fmt = fmts.empty() ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : fmts[0];
    for (int64_t f : fmts) if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) { fmt = f; break; }
    for (int e = 0; e < 2; e++) {
        XrSwapchainCreateInfo sc = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        sc.format = fmt; sc.sampleCount = 1;
        sc.width = vcv[e].recommendedImageRectWidth; sc.height = vcv[e].recommendedImageRectHeight;
        sc.faceCount = sc.arraySize = sc.mipCount = 1;
        XRCHK(xrCreateSwapchain(g_sess, &sc, &g_eye[e].sc));
        g_eye[e].w = sc.width; g_eye[e].h = sc.height;
        uint32_t ni = 0;
        xrEnumerateSwapchainImages(g_eye[e].sc, 0, &ni, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> imgs(ni, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
        xrEnumerateSwapchainImages(g_eye[e].sc, ni, &ni, (XrSwapchainImageBaseHeader *)imgs.data());
        for (auto &im : imgs) {
            D3D11_RENDER_TARGET_VIEW_DESC rd = {}; rd.Format = (DXGI_FORMAT)fmt; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            ID3D11RenderTargetView *rtv = nullptr;
            g_dev->CreateRenderTargetView(im.texture, &rd, &rtv);
            g_eye[e].rtv.push_back(rtv);
        }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = sc.width; td.Height = sc.height; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_D32_FLOAT;
        td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ID3D11Texture2D *d = nullptr;
        g_dev->CreateTexture2D(&td, nullptr, &d);
        g_dev->CreateDepthStencilView(d, nullptr, &g_eye[e].dsv);
        d->Release();
        logf("eye %d: %dx%d format %lld, %u images", e, sc.width, sc.height, (long long)fmt, ni);
    }

    // actions
    XrActionSetCreateInfo as = { XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy_s(as.actionSetName, "table"); strcpy_s(as.localizedActionSetName, "Table");
    XRCHK(xrCreateActionSet(g_xr, &as, &g_aset));
    g_hand_path[0] = Path("/user/hand/left"); g_hand_path[1] = Path("/user/hand/right");
    auto mk = [&](XrAction *a, const char *name, XrActionType t) {
        XrActionCreateInfo ac = { XR_TYPE_ACTION_CREATE_INFO };
        ac.actionType = t; strcpy_s(ac.actionName, name); strcpy_s(ac.localizedActionName, name);
        bool per_hand = t == XR_ACTION_TYPE_POSE_INPUT || t == XR_ACTION_TYPE_FLOAT_INPUT || t == XR_ACTION_TYPE_VECTOR2F_INPUT ||
                        !strcmp(name, "recenter") || !strcmp(name, "follow");
        ac.countSubactionPaths = per_hand ? 2 : 0; ac.subactionPaths = per_hand ? g_hand_path : nullptr;
        return xrCreateAction(g_aset, &ac, a);
    };
    XRCHK(mk(&g_act_pose, "grip_pose", XR_ACTION_TYPE_POSE_INPUT));
    XRCHK(mk(&g_act_squeeze, "squeeze", XR_ACTION_TYPE_FLOAT_INPUT));
    XRCHK(mk(&g_act_stick, "thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT));
    XRCHK(mk(&g_act_recenter, "recenter", XR_ACTION_TYPE_BOOLEAN_INPUT));
    XRCHK(mk(&g_act_follow, "follow", XR_ACTION_TYPE_BOOLEAN_INPUT));
    XRCHK(mk(&g_act_trigger, "trigger", XR_ACTION_TYPE_FLOAT_INPUT));
    XRCHK(mk(&g_act_fire, "fire", XR_ACTION_TYPE_BOOLEAN_INPUT));
    XRCHK(mk(&g_act_action, "action", XR_ACTION_TYPE_BOOLEAN_INPUT));
    XRCHK(mk(&g_act_esc, "escape", XR_ACTION_TYPE_BOOLEAN_INPUT));
    XrActionSuggestedBinding b[] = {
        { g_act_pose, Path("/user/hand/left/input/grip/pose") }, { g_act_pose, Path("/user/hand/right/input/grip/pose") },
        { g_act_squeeze, Path("/user/hand/left/input/squeeze/value") }, { g_act_squeeze, Path("/user/hand/right/input/squeeze/value") },
        { g_act_stick, Path("/user/hand/left/input/thumbstick") }, { g_act_stick, Path("/user/hand/right/input/thumbstick") },
        { g_act_recenter, Path("/user/hand/left/input/x/click") }, { g_act_recenter, Path("/user/hand/left/input/thumbstick/click") },
        { g_act_follow, Path("/user/hand/right/input/thumbstick/click") },
        { g_act_trigger, Path("/user/hand/left/input/trigger/value") }, { g_act_trigger, Path("/user/hand/right/input/trigger/value") },
        { g_act_fire, Path("/user/hand/right/input/a/click") }, { g_act_action, Path("/user/hand/right/input/b/click") },
        { g_act_esc, Path("/user/hand/left/input/y/click") }, { g_act_esc, Path("/user/hand/left/input/menu/click") },
    };
    XrInteractionProfileSuggestedBinding sb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    sb.interactionProfile = Path("/interaction_profiles/oculus/touch_controller");
    sb.suggestedBindings = b; sb.countSuggestedBindings = sizeof b / sizeof b[0];
    XRCHK(xrSuggestInteractionProfileBindings(g_xr, &sb));
    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo sc = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        sc.action = g_act_pose; sc.subactionPath = g_hand_path[h]; sc.poseInActionSpace.orientation.w = 1;
        XRCHK(xrCreateActionSpace(g_sess, &sc, &g_hand_space[h]));
    }
    XrSessionActionSetsAttachInfo at = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    at.countActionSets = 1; at.actionSets = &g_aset;
    XRCHK(xrAttachSessionActionSets(g_sess, &at));
    return true;
}

static XMMATRIX PoseMatrix(const XrPosef &p)
{
    XMVECTOR q = XMVectorSet(p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w);
    return XMMatrixRotationQuaternion(q) * XMMatrixTranslation(p.position.x, p.position.y, p.position.z);
}

static XMMATRIX ProjFov(const XrFovf &f, float n, float fa)
{
    float l = tanf(f.angleLeft) * n, r = tanf(f.angleRight) * n, d = tanf(f.angleDown) * n, u = tanf(f.angleUp) * n;
    return XMMatrixPerspectiveOffCenterRH(l, r, d, u, n, fa);
}

// grab state
struct Grab { bool active; XMFLOAT3 hand0; float hand_yaw0; XMFLOAT3 table0; float table_yaw0; };
static Grab g_grab[2];
static float YawOf(const XrQuaternionf &q)
{
    // forward (-z) projected onto xz
    XMVECTOR f = XMVector3Rotate(XMVectorSet(0, 0, -1, 0), XMVectorSet(q.x, q.y, q.z, q.w));
    return atan2f(-XMVectorGetX(f), -XMVectorGetZ(f));
}

static float g_in_steer, g_in_stick_y, g_in_accel, g_in_brake;
static bool BoolAction(XrAction a)
{
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO }; gi.action = a;
    XrActionStateBoolean b = { XR_TYPE_ACTION_STATE_BOOLEAN };
    return XR_SUCCEEDED(xrGetActionStateBoolean(g_sess, &gi, &b)) && b.isActive && b.currentState;
}

// controller -> game keys (merged into the game's keyboard by MMCDA.dll)
static void SendGameInput()
{
    if (!g_sh) return;
    uint32_t b = 0;
    if (BoolAction(g_act_fire)) b |= MMVR_BTN_FIRE;
    if (BoolAction(g_act_action)) b |= MMVR_BTN_ACTION;
    if (BoolAction(g_act_esc)) b |= MMVR_BTN_ESC;
    if (g_in_stick_y > 0.6f) b |= MMVR_BTN_UP;          // menus
    if (g_in_stick_y < -0.6f) b |= MMVR_BTN_DOWN;
    g_sh->in_steer = g_in_steer; g_sh->in_accel = g_in_accel; g_sh->in_brake = g_in_brake;
    g_sh->in_buttons = b;
    InterlockedIncrement((volatile LONG *)&g_sh->in_seq);
}

static void HandleInput(XrTime t, float dt)
{
    XrActiveActionSet aas = { g_aset, XR_NULL_PATH };
    XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO }; si.countActiveActionSets = 1; si.activeActionSets = &aas;
    if (XR_FAILED(xrSyncActions(g_sess, &si))) return;
    for (int h = 0; h < 2; h++) {
        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
        g_hand_valid[h] = XR_SUCCEEDED(xrLocateSpace(g_hand_space[h], g_space, t, &loc)) &&
                          (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        if (!g_hand_valid[h]) { g_grab[h].active = false; continue; }
        g_hand_xf[h] = PoseMatrix(loc.pose);
        XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO }; gi.subactionPath = g_hand_path[h];
        gi.action = g_act_squeeze;
        XrActionStateFloat sq = { XR_TYPE_ACTION_STATE_FLOAT };
        xrGetActionStateFloat(g_sess, &gi, &sq);
        bool down = sq.isActive && sq.currentState > 0.6f;
        float hy = YawOf(loc.pose.orientation);
        XMFLOAT3 hp(loc.pose.position.x, loc.pose.position.y, loc.pose.position.z);
        if (down && !g_grab[h].active && !g_grab[1 - h].active) {
            g_grab[h] = { true, hp, hy, g_table_pos, g_table_yaw };
        } else if (!down) g_grab[h].active = false;
        if (g_grab[h].active) {
            // rotate the table about the hand by the hand's yaw change, and follow the hand
            float dy = hy - g_grab[h].hand_yaw0;
            XMVECTOR off = XMLoadFloat3(&g_grab[h].table0) - XMLoadFloat3(&g_grab[h].hand0);
            off = XMVector3Transform(off, XMMatrixRotationY(dy));
            XMStoreFloat3(&g_table_pos, XMLoadFloat3(&hp) + off);
            g_table_yaw = g_grab[h].table_yaw0 + dy;
        }
        gi.action = g_act_stick;
        XrActionStateVector2f st = { XR_TYPE_ACTION_STATE_VECTOR2F };
        xrGetActionStateVector2f(g_sess, &gi, &st);
        if (st.isActive) {
            if (h == 1 && fabsf(st.currentState.y) > 0.3f) g_table_size = std::min(20.0f, std::max(0.2f, g_table_size * expf(st.currentState.y * dt * 1.2f)));
            if (h == 1 && fabsf(st.currentState.x) > 0.3f) g_table_yaw -= st.currentState.x * dt * 1.5f;
            if (h == 0) { g_in_steer = st.currentState.x; g_in_stick_y = st.currentState.y; }
        } else if (h == 0) g_in_steer = g_in_stick_y = 0;
        gi.action = g_act_trigger;
        XrActionStateFloat tr = { XR_TYPE_ACTION_STATE_FLOAT };
        xrGetActionStateFloat(g_sess, &gi, &tr);
        (h == 1 ? g_in_accel : g_in_brake) = tr.isActive ? tr.currentState : 0.0f;
        gi.action = g_act_recenter;
        XrActionStateBoolean rc = { XR_TYPE_ACTION_STATE_BOOLEAN };
        xrGetActionStateBoolean(g_sess, &gi, &rc);
        gi.action = g_act_follow;
        XrActionStateBoolean fb = { XR_TYPE_ACTION_STATE_BOOLEAN };
        xrGetActionStateBoolean(g_sess, &gi, &fb);
        if (fb.isActive && fb.changedSinceLastSync && fb.currentState) {
            g_follow_mode = (g_follow_mode + 1) % 3;
            g_follow_vr = g_follow_mode != 0; g_follow = g_follow_vr || g_follow;
            if (g_follow_vr && g_table_size < 3.0f) g_table_size = 3.0f;   // closer look when following
            logf("follow mode %d", g_follow_mode);
        }
        gi.action = g_act_recenter;
        xrGetActionStateBoolean(g_sess, &gi, &rc);
        if (rc.isActive && rc.changedSinceLastSync && rc.currentState) {
            g_table_pos = XMFLOAT3(g_head_pos.x - sinf(g_head_yaw) * 0.6f, g_head_pos.y - 0.45f, g_head_pos.z - cosf(g_head_yaw) * 0.6f);
            g_table_yaw = g_head_yaw;
        }
    }
    SendGameInput();
}

static void PollXrEvents()
{
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(g_xr, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto *s = (XrEventDataSessionStateChanged *)&ev;
            logf("session state %d", (int)s->state);
            if (s->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                g_running = XR_SUCCEEDED(xrBeginSession(g_sess, &bi));
            } else if (s->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g_sess); g_running = false;
            } else if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING) {
                g_running = false; g_quit = true;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) g_quit = true;
        ev = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

static void XrFrame()
{
    XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs = { XR_TYPE_FRAME_STATE };
    if (XR_FAILED(xrWaitFrame(g_sess, &wi, &fs))) return;
    XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO };
    xrBeginFrame(g_sess, &bi);
    {   // log the achieved rate every 5 s
        static double t0; static int frames; double now = NowMs();
        if (!t0) t0 = now;
        frames++;
        if (now - t0 >= 5000) {
            logf("headset: %.1f fps (runtime period %.2f ms)", frames * 1000.0 / (now - t0), fs.predictedDisplayPeriod * 1e-6);
            t0 = now; frames = 0;
        }
    }
    static XrTime last_t; float dt = last_t ? (float)((fs.predictedDisplayTime - last_t) * 1e-9) : 0.011f;
    last_t = fs.predictedDisplayTime;
    HandleInput(fs.predictedDisplayTime, std::min(dt, 0.1f));

    XrCompositionLayerProjectionView pv[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
    XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    const XrCompositionLayerBaseHeader *layers[1] = { (XrCompositionLayerBaseHeader *)&layer };
    uint32_t nlayers = 0;
    if (fs.shouldRender) {
        XrViewLocateInfo li = { XR_TYPE_VIEW_LOCATE_INFO };
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = fs.predictedDisplayTime; li.space = g_space;
        XrViewState vs = { XR_TYPE_VIEW_STATE };
        XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
        uint32_t nv = 0;
        if (XR_SUCCEEDED(xrLocateViews(g_sess, &li, &vs, 2, &nv, views)) && nv == 2) {
            g_head_pos = XMFLOAT3((views[0].pose.position.x + views[1].pose.position.x) / 2,
                                  (views[0].pose.position.y + views[1].pose.position.y) / 2,
                                  (views[0].pose.position.z + views[1].pose.position.z) / 2);
            g_head_yaw = YawOf(views[0].pose.orientation);
            g_face_pt = XMVectorSet(g_head_pos.x, g_head_pos.y, g_head_pos.z, 1);
            for (int e = 0; e < 2; e++) {
                uint32_t img = 0;
                XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
                xrAcquireSwapchainImage(g_eye[e].sc, &ai, &img);
                XrSwapchainImageWaitInfo wi2 = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi2.timeout = XR_INFINITE_DURATION;
                xrWaitSwapchainImage(g_eye[e].sc, &wi2);
                float clear[4] = { 0.02f, 0.02f, 0.03f, 1 };
                g_ctx->ClearRenderTargetView(g_eye[e].rtv[img], clear);
                g_ctx->ClearDepthStencilView(g_eye[e].dsv, D3D11_CLEAR_DEPTH, 1, 0);
                g_ctx->OMSetRenderTargets(1, &g_eye[e].rtv[img], g_eye[e].dsv);
                D3D11_VIEWPORT vp = { 0, 0, (float)g_eye[e].w, (float)g_eye[e].h, 0, 1 };
                g_ctx->RSSetViewports(1, &vp);
                XMMATRIX view = XMMatrixInverse(nullptr, PoseMatrix(views[e].pose));
                DrawScene(view * ProjFov(views[e].fov, 0.02f, 50.0f), view);
                XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                xrReleaseSwapchainImage(g_eye[e].sc, &ri);
                pv[e].pose = views[e].pose; pv[e].fov = views[e].fov;
                pv[e].subImage.swapchain = g_eye[e].sc;
                pv[e].subImage.imageRect = { { 0, 0 }, { g_eye[e].w, g_eye[e].h } };
            }
            layer.space = g_space; layer.viewCount = 2; layer.views = pv;
            nlayers = 1;
        }
    }
    XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = nlayers; ei.layers = nlayers ? layers : nullptr;
    xrEndFrame(g_sess, &ei);
}

// ---------------------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmd, int)
{
    char logpath[MAX_PATH];
    GetModuleFileNameA(nullptr, logpath, MAX_PATH);
    strcpy_s(strrchr(logpath, '\\') + 1, 32, "MotorMashVR.log");
    g_log = _fsopen(logpath, "w", _SH_DENYNO);
    bool desktop = strstr(cmd, "--desktop") != nullptr;
    g_follow = g_follow_env;
    if (getenv("MMVR_DIST")) g_orbit_dist = (float)atof(getenv("MMVR_DIST"));
    if (getenv("MMVR_YAW")) g_orbit_yaw = (float)atof(getenv("MMVR_YAW"));
    if (getenv("MMVR_FOLLOWMODE")) {   // test the headset follow modes from the desktop
        g_follow_mode = atoi(getenv("MMVR_FOLLOWMODE")) % 3;
        g_follow_vr = g_follow = g_follow_mode != 0;
        if (g_follow_vr) g_table_size = 3.0f;
    }

    bool xr_inst = !desktop && InitXrInstance();
    bool xr_sys = xr_inst && GetXrSystem();
    if (!CreateDevice()) { MessageBoxA(nullptr, "Could not create a Direct3D 11 device.", "Motor Mash VR", MB_ICONERROR); return 1; }
    if (!CreatePipeline()) { MessageBoxA(nullptr, "Shader setup failed (see MotorMashVR.log).", "Motor Mash VR", MB_ICONERROR); return 1; }
    CreateBox();
    CreateQuad();
    if (xr_sys) g_xr_ok = InitXrSession();

    WNDCLASSA wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = "MotorMashVR";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    // Open on a monitor the game isn't on, filling its work area, and never take focus:
    // the game pauses (it gets minimised) when it loses focus, and it needs the keyboard.
    RECT r = { 100, 100, 100 + (LONG)g_win_w, 100 + (LONG)g_win_h };
    {
        HWND game = FindWindowA(nullptr, "Motor Mash");
        HMONITOR gm = game ? MonitorFromWindow(game, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        struct Pick { HMONITOR avoid; RECT work; bool found; } pick = { gm, {}, false };
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
            Pick *pk = (Pick *)p;
            if (m == pk->avoid || pk->found) return TRUE;
            MONITORINFO mi = { sizeof mi };
            GetMonitorInfoA(m, &mi);
            pk->work = mi.rcWork; pk->found = true;
            return TRUE;
        }, (LPARAM)&pick);
        if (pick.found) r = pick.work;
    }
    g_win_w = r.right - r.left; g_win_h = r.bottom - r.top;
    g_wnd = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_APPWINDOW, "MotorMashVR",
                            g_xr_ok ? "Motor Mash VR (headset active)" : "Motor Mash VR (desktop)",
                            WS_POPUP, r.left, r.top, g_win_w, g_win_h, nullptr, nullptr, inst, nullptr);
    ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
    IDXGIDevice *dd = nullptr; IDXGIAdapter *ad = nullptr; IDXGIFactory2 *f2 = nullptr;
    g_dev->QueryInterface(IID_PPV_ARGS(&dd)); dd->GetAdapter(&ad); ad->GetParent(IID_PPV_ARGS(&f2));
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = g_win_w; sd.Height = g_win_h; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    f2->CreateSwapChainForHwnd(g_dev, g_wnd, &sd, nullptr, nullptr, &g_swap);
    SafeRelease(f2); SafeRelease(ad); SafeRelease(dd);
    CreateWindowTargets();

    while (!g_quit) {
        MSG m;
        while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
        PollGame();
        UpdateHud();
        UpdateScreen();
        ReloadDirtyTextures();
        BuildInstances(NowMs());
        if (xr_inst && !g_xr_ok) {          // headset not streaming yet: retry every 2 s
            static DWORD next;
            if (GetTickCount() >= next) {
                next = GetTickCount() + 2000;
                if (GetXrSystem()) {
                    g_xr_ok = InitXrSession();
                    if (g_xr_ok) SetWindowTextA(g_wnd, "Motor Mash VR (headset active)");
                }
            }
        }
        if (g_xr_ok) {
            PollXrEvents();
            if (g_running) XrFrame();
            else Sleep(10);
        }
        {   // desktop mirror: every frame on its own, at most ~30 Hz while the headset runs
            static double last;
            double now = NowMs();
            if (!g_running || now - last >= 33.0) { last = now; RenderWindow(); }
        }
    }
    if (g_sess) xrDestroySession(g_sess);
    if (g_xr) xrDestroyInstance(g_xr);
    return 0;
}
