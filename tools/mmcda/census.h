/* census.h - diagnostic: which DirectX COM methods does the game really call?
 *
 * Enabled with MMCDA_CENSUS=1. Nothing in the DirectX DLLs is modified (d3drm.dll aborts if
 * its vtables are patched). Instead every COM call site in the game's own code
 * (`call dword ptr [reg+disp]`, listed in census_sites.h) gets a one-shot int3. When one is
 * hit, a vectored exception handler records the vtable the register points to and the slot
 * (disp/4), puts the original byte back and lets the call run. So each site is recorded the
 * first time it executes, with no further cost.
 *
 * Vtables are named where possible: DirectDraw / Direct3D ones from probe objects created
 * when the game first calls DirectDrawCreate (one per interface version, read but never
 * patched); d3drm.dll ones are reported as module offsets and named offline.
 * Results go to rendercensus.txt next to mmcda.ini every few seconds:
 *   site  module+vtable-offset  slot  [interface]
 * MMCDA.dll's own DirectX calls are not covered (they are known from its source).
 */
#include <ddraw.h>
#include <d3d.h>
#include "census_sites.h"

typedef struct { void *vt; char name[24]; } census_name;
static census_name g_cnames[32];
static int g_ncnames;
static void *g_csite_vt[CENSUS_NSITES];        /* vtable seen at each site (NULL: not hit) */
static BYTE g_csite_byte[CENSUS_NSITES];       /* original first byte */
static volatile LONG g_chits;

static int census_find(DWORD a)
{
    int lo = 0, hi = CENSUS_NSITES - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (k_census_sites[m].addr == a) return m;
        if (k_census_sites[m].addr < a) lo = m + 1; else hi = m - 1;
    }
    return -1;
}

static void census_poke(DWORD a, BYTE v)
{
    DWORD old;
    VirtualProtect((void *)a, 1, PAGE_EXECUTE_READWRITE, &old);
    *(BYTE *)a = v;
    VirtualProtect((void *)a, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)a, 1);
}

static LONG CALLBACK census_veh(EXCEPTION_POINTERS *ep)
{
    CONTEXT *c = ep->ContextRecord;
    DWORD a = (DWORD)ep->ExceptionRecord->ExceptionAddress;
    DWORD regs[8];
    int i;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    if ((i = census_find(a)) < 0 || g_csite_vt[i]) return EXCEPTION_CONTINUE_SEARCH;
    regs[0] = c->Eax; regs[1] = c->Ecx; regs[2] = c->Edx; regs[3] = c->Ebx;
    regs[4] = c->Esp; regs[5] = c->Ebp; regs[6] = c->Esi; regs[7] = c->Edi;
    g_csite_vt[i] = (void *)regs[k_census_sites[i].reg];
    if (!g_csite_vt[i]) g_csite_vt[i] = (void *)1;
    census_poke(a, g_csite_byte[i]);
    c->Eip = a;                                 /* re-run the real call */
    InterlockedIncrement(&g_chits);
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* ---- naming DirectDraw / Direct3D vtables from probe objects ---- */
static void census_name_vt(IUnknown *o, const char *name)
{
    if (o && g_ncnames < 32) {
        g_cnames[g_ncnames].vt = *(void **)o;
        lstrcpynA(g_cnames[g_ncnames].name, name, sizeof g_cnames[0].name);
        g_ncnames++;
    }
}

static void census_name_qi(IUnknown *o, REFIID iid, const char *name)
{
    IUnknown *q = NULL;
    if (o && SUCCEEDED(o->lpVtbl->QueryInterface(o, iid, (void **)&q)) && q) {
        census_name_vt(q, name);
        q->lpVtbl->Release(q);
    }
}

typedef HRESULT (WINAPI *PFN_DDCREATE)(GUID *, LPDIRECTDRAW *, IUnknown *);
static PFN_DDCREATE g_real_ddcreate;

static void census_probe_ddraw(void)
{
    LPDIRECTDRAW dd = NULL;
    LPDIRECTDRAWSURFACE surf = NULL;
    LPDIRECTDRAWPALETTE pal = NULL;
    LPDIRECTDRAWCLIPPER clip = NULL;
    DDSURFACEDESC sd;
    PALETTEENTRY pe[256];
    if (FAILED(g_real_ddcreate(NULL, &dd, NULL)) || !dd) return;
    IDirectDraw_SetCooperativeLevel(dd, NULL, DDSCL_NORMAL);
    census_name_vt((IUnknown *)dd, "IDirectDraw");
    census_name_qi((IUnknown *)dd, &IID_IDirectDraw2, "IDirectDraw2");
    census_name_qi((IUnknown *)dd, &IID_IDirectDraw4, "IDirectDraw4");
    census_name_qi((IUnknown *)dd, &IID_IDirect3D, "IDirect3D");
    census_name_qi((IUnknown *)dd, &IID_IDirect3D2, "IDirect3D2");
    census_name_qi((IUnknown *)dd, &IID_IDirect3D3, "IDirect3D3");
    memset(&sd, 0, sizeof sd);
    sd.dwSize = sizeof sd;
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    sd.dwWidth = sd.dwHeight = 8;
    if (SUCCEEDED(IDirectDraw_CreateSurface(dd, &sd, &surf, NULL)) && surf) {
        census_name_vt((IUnknown *)surf, "IDirectDrawSurface");
        census_name_qi((IUnknown *)surf, &IID_IDirectDrawSurface2, "IDirectDrawSurface2");
        census_name_qi((IUnknown *)surf, &IID_IDirectDrawSurface3, "IDirectDrawSurface3");
        census_name_qi((IUnknown *)surf, &IID_IDirectDrawSurface4, "IDirectDrawSurface4");
        census_name_qi((IUnknown *)surf, &IID_IDirect3DTexture, "IDirect3DTexture");
        census_name_qi((IUnknown *)surf, &IID_IDirect3DTexture2, "IDirect3DTexture2");
        IDirectDrawSurface_Release(surf);
    }
    memset(pe, 0, sizeof pe);
    if (SUCCEEDED(IDirectDraw_CreatePalette(dd, DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, &pal, NULL)) && pal) {
        census_name_vt((IUnknown *)pal, "IDirectDrawPalette");
        IDirectDrawPalette_Release(pal);
    }
    if (SUCCEEDED(IDirectDraw_CreateClipper(dd, 0, &clip, NULL)) && clip) {
        census_name_vt((IUnknown *)clip, "IDirectDrawClipper");
        IDirectDrawClipper_Release(clip);
    }
    IDirectDraw_Release(dd);
}

static HRESULT WINAPI census_ddcreate(GUID *g, LPDIRECTDRAW *out, IUnknown *outer)
{
    static int probed;
    HRESULT hr = g_real_ddcreate(g, out, outer);
    if (!probed) { probed = 1; census_probe_ddraw(); }
    return hr;
}

static void census_dump(void)
{
    char path[MAX_PATH];
    FILE *f;
    int i, j;
    snprintf(path, sizeof path, "%srendercensus.txt", g_home);
    if (!(f = fopen(path, "w"))) return;
    fprintf(f, "# %ld of %d game COM call sites hit\n# site     module+vtable  slot  interface\n",
            g_chits, CENSUS_NSITES);
    for (i = 0; i < CENSUS_NSITES; i++) {
        void *vt = g_csite_vt[i];
        HMODULE m = NULL;
        char mod[MAX_PATH] = "?", *b;
        const char *nm = "";
        if (!vt || vt == (void *)2) continue;   /* not hit, or site already hooked */
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)vt, &m) && m) {
            GetModuleFileNameA(m, mod, sizeof mod);
            if ((b = strrchr(mod, 0x5C))) memmove(mod, b + 1, strlen(b));
        }
        for (j = 0; j < g_ncnames; j++) if (g_cnames[j].vt == vt) nm = g_cnames[j].name;
        fprintf(f, "%06lX  %s+%06lX  %3d  %s\n", k_census_sites[i].addr, mod,
                m ? (DWORD)vt - (DWORD)m : (DWORD)vt, k_census_sites[i].disp / 4, nm);
    }
    fclose(f);
}

static DWORD WINAPI census_thread(LPVOID p)
{
    (void)p;
    for (;;) { Sleep(3000); census_dump(); }
}

static void install_census(void)
{
    char buf[4];
    DWORD old, *iat = (DWORD *)0x0043D010;
    FARPROC real;
    int i, armed = 0;
    if (!GetEnvironmentVariableA("MMCDA_CENSUS", buf, sizeof buf)) return;
    AddVectoredExceptionHandler(1, census_veh);
    for (i = 0; i < CENSUS_NSITES; i++) {
        BYTE *s = (BYTE *)k_census_sites[i].addr;
        /* still the original call [reg+disp]? (other hooks may have rewritten some) */
        if (s[0] != 0xFF || ((s[1] >> 3) & 7) != 2) { g_csite_vt[i] = (void *)2; continue; }
        g_csite_byte[i] = s[0];
        census_poke((DWORD)s, 0xCC);
        armed++;
    }
    real = GetProcAddress(GetModuleHandleA("DDraw.dll"), "DirectDrawCreate");
    for (i = 0; i < 2; i++)
        if (real && iat[i] == (DWORD)real) {
            g_real_ddcreate = (PFN_DDCREATE)real;
            VirtualProtect(&iat[i], 4, PAGE_READWRITE, &old);
            iat[i] = (DWORD)census_ddcreate;
            VirtualProtect(&iat[i], 4, old, &old);
        }
    log_msg("census: %d of %d call sites armed", armed, CENSUS_NSITES);
    CreateThread(NULL, 0, census_thread, NULL, 0, NULL);
}
