/* render.h - renderer selection: [render] backend=original|vulkan in mmcda.ini
 * (environment MMCDA_RENDER overrides, for testing).
 *
 * original (default): nothing here runs. The game uses D3DRM + DirectDraw (dgVoodoo2) exactly
 *                     as shipped, like choosing the DX exe over the DOS one in 1998.
 * vulkan:             the game's imports of DirectDrawCreate and Direct3DRMCreate are pointed
 *                     at our implementations (vdd.h, vrm.h), which draw with Vulkan.
 * See RENDERER.md.
 */
#include <math.h>
#include <stddef.h>
#include <malloc.h>
#include <ddraw.h>
#include <d3d.h>

static void stub_note(const char *what) { log_msg("render: %s not implemented", what); }

#include "com_stubs.h"
#include "vkpresent.h"
#include "vdd.h"
#include "vrm.h"
#include "xload.h"
#include "vkrender.h"

/* g_render_vulkan is declared in mmcda.c */

static int render_patch_iat(DWORD *slot, FARPROC expect, void *fn, const char *what)
{
    DWORD old;
    if (!expect || *slot != (DWORD)expect) { log_msg("render: %s import not found, staying on the original renderer", what); return 0; }
    VirtualProtect(slot, 4, PAGE_READWRITE, &old);
    *slot = (DWORD)fn;
    VirtualProtect(slot, 4, old, &old);
    return 1;
}

/* ---- Display menu: "Hardware Acceleration" becomes "Renderer" -----------------------
 * The Display submenu (0x40E940) has two rows: Hardware Acceleration (bit 0x8000 of the
 * settings word 0x43F20C) and Mode. The first row is pointed at our own variable and
 * strings instead: Renderer = Original / Vulkan. The game's hardware setting is left as it is
 * (the original renderer depends on it being off), and so are both of its resolution lists.
 * The choice is saved to mmcda.ini [render] backend and applies at the next start, because the
 * renderer is chosen when DirectDraw / D3DRM are first created. */
static volatile DWORD g_render_choice;        /* 0x8000 = Vulkan, as the menu code expects */
static DWORD g_render_saved;
static const char *g_rm_label[6] = { "Renderer", "Renderer", "Rendu", "Renderizador", "Rendering", NULL };
static const char *g_rm_off[6], *g_rm_on[6];

static void render_menu_strings(void)
{
    int i, vk_now = g_render_vulkan;
    for (i = 0; i < 5; i++) {
        g_rm_off[i] = vk_now ? "Original (restart)" : "Original";
        g_rm_on[i] = vk_now ? "Vulkan" : "Vulkan (restart)";
    }
    if (!(g_render_choice & 0x8000) == !vk_now) {   /* unchanged: plain names */
        for (i = 0; i < 5; i++) { g_rm_off[i] = "Original"; g_rm_on[i] = "Vulkan"; }
    }
}

/* called every present: save a changed choice */
static void render_menu_poll(void)
{
    char path[MAX_PATH];
    if (g_render_choice == g_render_saved) return;
    g_render_saved = g_render_choice;
    snprintf(path, sizeof path, "%smmcda.ini", g_home);
    WritePrivateProfileStringA("render", "backend", (g_render_choice & 0x8000) ? "vulkan" : "original", path);
    render_menu_strings();
    log_msg("render: menu choice %s (applies at the next start)", (g_render_choice & 0x8000) ? "vulkan" : "original");
}

static int render_patch_operand(DWORD site, int off, const BYTE *expect, int n, DWORD value)
{
    DWORD old;
    BYTE *p = (BYTE *)site;
    if (memcmp(p, expect, n) != 0) return 0;
    VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p + off, &value, 4);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return 1;
}

static void install_render_menu(void)
{
    static const struct { DWORD site; int off; BYTE b[7]; int n; int what; } k[] = {
        { 0x40EC40, 1, { 0x68, 0x00, 0x13, 0x45, 0x00 }, 5, 0 },              /* push label table */
        { 0x40EC50, 1, { 0xA1, 0x0C, 0xF2, 0x43, 0x00 }, 5, 3 },              /* mov eax,[setting] */
        { 0x40EC6D, 1, { 0x68, 0x20, 0x15, 0x45, 0x00 }, 5, 1 },              /* push "No" table */
        { 0x40EC7B, 1, { 0x68, 0xC8, 0x17, 0x45, 0x00 }, 5, 2 },              /* push "Yes" table */
        { 0x40ED79, 2, { 0x85, 0x3D, 0x0C, 0xF2, 0x43, 0x00 }, 6, 3 },        /* test [setting],edi */
        { 0x40ED87, 3, { 0x8B, 0x34, 0x95, 0x20, 0x15, 0x45, 0x00 }, 7, 1 },  /* mov esi,[No+edx*4] */
        { 0x40ED95, 3, { 0x8B, 0x34, 0x85, 0xC8, 0x17, 0x45, 0x00 }, 7, 2 },  /* mov esi,[Yes+eax*4] */
        { 0x40EB63, 2, { 0x31, 0x3D, 0x0C, 0xF2, 0x43, 0x00 }, 6, 3 },        /* xor [setting],edi */
    };
    int i, ok = 0;
    for (i = 0; i < (int)(sizeof k / sizeof k[0]); i++)
        if (memcmp((BYTE *)k[i].site, k[i].b, k[i].n) != 0) { log_msg("render: Display menu site %06lX mismatch, menu left as it is", k[i].site); return; }
    g_render_choice = g_render_vulkan ? 0x8000 : 0;
    g_render_saved = g_render_choice;
    render_menu_strings();
    for (i = 0; i < (int)(sizeof k / sizeof k[0]); i++) {
        DWORD v = k[i].what == 0 ? (DWORD)g_rm_label : k[i].what == 1 ? (DWORD)g_rm_off :
                  k[i].what == 2 ? (DWORD)g_rm_on : (DWORD)&g_render_choice;
        ok += render_patch_operand(k[i].site, k[i].off, k[i].b, k[i].n, v);
    }
    log_msg("render: Display menu shows Renderer (%d/8 sites)", ok);
}

static void install_render_backend(void)
{
    char v[32] = "original", path[MAX_PATH];
    DWORD *dd_iat = (DWORD *)0x0043D010, *rm_iat = (DWORD *)0x0043D264;
    FARPROC ddc = GetProcAddress(GetModuleHandleA("DDraw.dll"), "DirectDrawCreate");
    FARPROC rmc = GetProcAddress(GetModuleHandleA("d3drm.dll"), "Direct3DRMCreate");
    int i, ok = 0;
    snprintf(path, sizeof path, "%smmcda.ini", g_home);
    GetPrivateProfileStringA("render", "backend", "original", v, sizeof v, path);
    GetEnvironmentVariableA("MMCDA_RENDER", v, sizeof v);
    if (_stricmp(v, "vulkan") != 0) { log_msg("render: original (D3DRM + DirectDraw)"); return; }
    /* DDRAW IAT holds DirectDrawEnumerateA and DirectDrawCreate (order checked by value) */
    for (i = 0; i < 2; i++)
        if (dd_iat[i] == (DWORD)ddc) ok = render_patch_iat(&dd_iat[i], ddc, vdd_DirectDrawCreate, "DirectDrawCreate");
    if (!ok) { log_msg("render: DirectDrawCreate import not found, staying on the original renderer"); return; }
    if (!render_patch_iat(rm_iat, rmc, vrm_Direct3DRMCreate, "Direct3DRMCreate")) {
        for (i = 0; i < 2; i++) if (dd_iat[i] == (DWORD)vdd_DirectDrawCreate) render_patch_iat(&dd_iat[i], (FARPROC)vdd_DirectDrawCreate, ddc, "undo");
        return;
    }
    g_vrm_xload = x_meshbuilder_load;
    g_vrm_on_render = vr_on_render;
    g_vrm_on_clear = vr_on_clear;
    g_vrm_tex_free = vr_tex_free;
    g_render_vulkan = 1;
    log_msg("render: vulkan (stage 2: D3DRM scene drawn with Vulkan)");
}

static void install_render(void)
{
    install_render_backend();
    install_render_menu();
}
