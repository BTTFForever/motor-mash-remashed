/*
 * MMCDA.dll - CD-audio emulator for Motor Mash (MotorMsh.exe, 1998).
 *
 * The game's import of WINMM.dll is renamed to MMCDA.dll (patch_motormsh.py).
 * We export the 7 functions the game imports. MCI commands sent to the
 * "cdaudio" device are emulated by playing Music\TrackNN.wav through waveOut;
 * everything else (the intro AVI device, timeGetTime, ...) goes to the real winmm.
 *
 * Behaviour the game relies on (from REA decomp of 0x42bc90 / 0x42c5e0):
 *   MCI_OPEN  type "cdaudio"            -> returns a device id
 *   MCI_SET   MCI_SET_TIME_FORMAT TMSF  -> accepted
 *   MCI_PLAY  FROM|TO|NOTIFY, TMSF      -> plays track FROM; posts MM_MCINOTIFY
 *                                          (MCI_NOTIFY_SUCCESSFUL) when it ends,
 *                                          the game then restarts it (loop)
 *   MCI_STOP / MCI_CLOSE                -> stop
 *   aux*: a fake AUXCAPS_CDAUDIO device controls the music volume.
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
/* public-domain Ogg Vorbis decoder (github.com/nothings/stb, v1.25) */
#define STB_VORBIS_NO_PUSHDATA_API
#pragma warning(push, 0)
#include "stb_vorbis.c"
#pragma warning(pop)

#define FAKE_CD_ID 0xCDA0

static CRITICAL_SECTION g_lock;
static HWAVEOUT g_wo;
static WAVEHDR g_hdr;
static BYTE *g_pcm;
static HANDLE g_thread, g_done;
static volatile LONG g_generation;
static DWORD g_volume = 0xFFFFFFFF;
static char g_dir[MAX_PATH];    /* folder of the running exe (music lives here) */
static char g_home[MAX_PATH];   /* settings + log: MMCDA_HOME, else g_dir */
static int g_open;
/* size of the current game mode, and of its image on the monitor (for HUD scaling) */
static volatile int g_mode_w = 640, g_mode_h = 480, g_disp_w = 640, g_disp_h = 480;
/* UI-thread windows and transition state (defined with the UI code further down) */
static HWND g_back, g_cover;
static volatile LONG g_transition, g_trans_mode_set, g_trans_presents;
static volatile DWORD g_trans_start;
static void ui_hide_cover(void);
static void ui_restack_backdrop(void);
/* present timing (ms, QPC) for display-time prediction in interp.h */
static volatile double g_last_present_ms, g_present_dt = 1000.0 / 60.0;
/* F8 overview camera (tiles.h); camera mode 0 chase, 1 overview, 2 free fly (debugmenu.h) */
static volatile int g_overview, g_cam_mode;
static void debugmenu_poll(void);
/* frame interpolation (interp.h) */
static void interp_note_race(void);
static void interp_on_viewport(void *vp);
static void interp_on_teardown(void);

static void log_msg(const char *fmt, ...)
{
    char path[MAX_PATH], buf[512];
    va_list ap; FILE *f;
    if (!GetEnvironmentVariableA("MMCDA_LOG", buf, 2)) return;
    snprintf(path, sizeof path, "%smmcda.log", g_home);
    if (!(f = fopen(path, "a"))) return;
    va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

static void stop_playback(void)
{
    InterlockedIncrement(&g_generation);
    EnterCriticalSection(&g_lock);
    if (g_wo) {
        waveOutReset(g_wo);
        waveOutUnprepareHeader(g_wo, &g_hdr, sizeof g_hdr);
        waveOutClose(g_wo);
        g_wo = NULL;
    }
    free(g_pcm); g_pcm = NULL;
    LeaveCriticalSection(&g_lock);
}

struct play_args { int track; HWND notify; LONG gen; };

static DWORD WINAPI play_thread(LPVOID p)
{
    struct play_args a = *(struct play_args *)p;
    char path[MAX_PATH];
    HANDLE f; DWORD size, got;
    BYTE *file; WAVEFORMATEX wf = {0};
    BYTE *data = NULL; DWORD datalen = 0, off = 12;
    int ok = 0;
    free(p);

    /* Music\TrackNN.ogg (decoded fully to 16-bit PCM), else Music\TrackNN.wav */
    snprintf(path, sizeof path, "%sMusic\\Track%02d.ogg", g_dir, a.track);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        int ch = 0, rate = 0;
        short *pcm = NULL;
        int frames = stb_vorbis_decode_filename(path, &ch, &rate, &pcm);
        if (frames <= 0 || !pcm) { log_msg("bad ogg %s", path); return 0; }
        file = (BYTE *)pcm;
        data = file;
        datalen = (DWORD)frames * ch * 2;
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = (WORD)ch;
        wf.nSamplesPerSec = rate;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = (WORD)(ch * 2);
        wf.nAvgBytesPerSec = rate * ch * 2;
    } else {
        snprintf(path, sizeof path, "%sMusic\\Track%02d.wav", g_dir, a.track);
        f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) { log_msg("missing %s", path); return 0; }
        size = GetFileSize(f, NULL);
        file = (BYTE *)malloc(size);
        ReadFile(f, file, size, &got, NULL);
        CloseHandle(f);

        /* minimal RIFF walk */
        while (off + 8 <= got) {
            DWORD id = *(DWORD *)(file + off), len = *(DWORD *)(file + off + 4);
            if (id == mmioFOURCC('f','m','t',' ')) memcpy(&wf, file + off + 8, 16);
            if (id == mmioFOURCC('d','a','t','a')) { data = file + off + 8; datalen = min(len, got - off - 8); }
            off += 8 + len + (len & 1);
        }
        if (!data || wf.wFormatTag != WAVE_FORMAT_PCM) { free(file); log_msg("bad wav %s", path); return 0; }
    }

    EnterCriticalSection(&g_lock);
    if (a.gen == g_generation &&
        waveOutOpen(&g_wo, WAVE_MAPPER, &wf, (DWORD_PTR)g_done, 0, CALLBACK_EVENT) == MMSYSERR_NOERROR) {
        g_pcm = file;
        ZeroMemory(&g_hdr, sizeof g_hdr);
        g_hdr.lpData = (LPSTR)data;
        g_hdr.dwBufferLength = datalen;
        waveOutSetVolume(g_wo, g_volume);
        ResetEvent(g_done);
        waveOutPrepareHeader(g_wo, &g_hdr, sizeof g_hdr);
        waveOutWrite(g_wo, &g_hdr, sizeof g_hdr);
        ok = 1;
    } else free(file);
    LeaveCriticalSection(&g_lock);
    if (!ok) return 0;
    log_msg("playing track %d", a.track);

    for (;;) {
        WaitForSingleObject(g_done, 250);
        if (a.gen != g_generation) return 0;            /* stopped / replaced */
        if (g_hdr.dwFlags & WHDR_DONE) break;
    }
    if (a.gen == g_generation && a.notify) {
        log_msg("track %d finished, notify", a.track);
        PostMessageA(a.notify, MM_MCINOTIFY, MCI_NOTIFY_SUCCESSFUL, FAKE_CD_ID);
    }
    return 0;
}

/* ---- exports ---------------------------------------------------------- */

MCIERROR WINAPI mmcda_mciSendCommandA(MCIDEVICEID id, UINT msg, DWORD_PTR flags, DWORD_PTR param)
{
    if (msg == MCI_OPEN && (flags & MCI_OPEN_TYPE) && !(flags & MCI_OPEN_TYPE_ID) && param) {
        MCI_OPEN_PARMSA *op = (MCI_OPEN_PARMSA *)param;
        if (op->lpstrDeviceType && lstrcmpiA(op->lpstrDeviceType, "cdaudio") == 0) {
            op->wDeviceID = FAKE_CD_ID;
            g_open = 1;
            log_msg("open cdaudio");
            return 0;
        }
    }
    if (id != FAKE_CD_ID)
        return mciSendCommandA(id, msg, flags, param);

    switch (msg) {
    case MCI_PLAY: {
        MCI_PLAY_PARMS *pp = (MCI_PLAY_PARMS *)param;
        struct play_args *a;
        int track = (flags & MCI_FROM) && pp ? (int)MCI_TMSF_TRACK(pp->dwFrom) : 2;
        stop_playback();
        a = (struct play_args *)malloc(sizeof *a);
        a->track = track;
        a->notify = (flags & MCI_NOTIFY) && pp ? (HWND)pp->dwCallback : NULL;
        a->gen = g_generation;
        if (g_thread) CloseHandle(g_thread);
        g_thread = CreateThread(NULL, 0, play_thread, a, 0, NULL);
        return 0;
    }
    case MCI_STOP: case MCI_PAUSE:
        stop_playback();
        return 0;
    case MCI_CLOSE:
        stop_playback();
        g_open = 0;
        return 0;
    case MCI_STATUS:
        if (param) {
            MCI_STATUS_PARMS *sp = (MCI_STATUS_PARMS *)param;
            switch (sp->dwItem) {
            case MCI_STATUS_NUMBER_OF_TRACKS: sp->dwReturn = 14; break;
            case MCI_STATUS_MEDIA_PRESENT: case MCI_STATUS_READY: sp->dwReturn = TRUE; break;
            case MCI_STATUS_MODE: sp->dwReturn = g_wo ? MCI_MODE_PLAY : MCI_MODE_STOP; break;
            default: sp->dwReturn = 0;
            }
        }
        return 0;
    default:
        return 0;   /* MCI_SET (time format) and anything else: accept */
    }
}

BOOL WINAPI mmcda_mciGetErrorStringA(MCIERROR err, LPSTR text, UINT len)
{
    return mciGetErrorStringA(err, text, len);
}

DWORD WINAPI mmcda_timeGetTime(void) { return timeGetTime(); }

/* The fake CD aux device lives at index == real device count. */
UINT WINAPI mmcda_auxGetNumDevs(void) { return auxGetNumDevs() + 1; }

MMRESULT WINAPI mmcda_auxGetDevCapsA(UINT_PTR dev, LPAUXCAPSA caps, UINT size)
{
    if (dev == auxGetNumDevs() && caps && size >= sizeof(AUXCAPSA)) {
        ZeroMemory(caps, sizeof *caps);
        lstrcpyA(caps->szPname, "MMCDA CD Audio");
        caps->wTechnology = AUXCAPS_CDAUDIO;
        caps->dwSupport = AUXCAPS_VOLUME | AUXCAPS_LRVOLUME;
        return MMSYSERR_NOERROR;
    }
    return auxGetDevCapsA(dev, caps, size);
}

MMRESULT WINAPI mmcda_auxGetVolume(UINT dev, LPDWORD vol)
{
    if (dev == auxGetNumDevs()) { if (vol) *vol = g_volume; return MMSYSERR_NOERROR; }
    return auxGetVolume(dev, vol);
}

MMRESULT WINAPI mmcda_auxSetVolume(UINT dev, DWORD vol)
{
    if (dev == auxGetNumDevs()) {
        g_volume = vol;
        EnterCriticalSection(&g_lock);
        if (g_wo) waveOutSetVolume(g_wo, vol);
        LeaveCriticalSection(&g_lock);
        return MMSYSERR_NOERROR;
    }
    return auxSetVolume(dev, vol);
}

/* ---- widescreen (Hor+) field of view ---------------------------------------
 * 0x42a5a0 creates the D3DRM viewport at the device size and calls
 * viewport->SetBack(5000.0) via `mov edx,[eax]; call [edx+0x3c]` at 0x42a632.
 * The field of view is never set (D3DRM default 0.5), and D3DRM's uniform scaling
 * applies it to the larger window dimension, so wider modes lose vertical view.
 * We redirect that call here, then widen the field so the vertical view stays
 * what it is at 4:3. Disable with MMCDA_NOWIDE=1.
 */
typedef HRESULT (WINAPI *PFN_SETVAL)(void *self, float v);
typedef DWORD (WINAPI *PFN_GETDIM)(void *self);
#define VP_SETBACK   15
#define VP_SETFIELD  16
#define DEV_GETHEIGHT 26
#define DEV_GETWIDTH  27
#define G_DEVICE (*(void **volatile *)0x0054AF18)

/* The race loop (0x403230) calls viewport->SetPlane(+-0.52, +-0.684|0.57) every
 * frame, which overrides any field of view. We hook SetPlane (vtable slot 24) on
 * the viewport class and widen left/right by aspect/(4/3) for 16:9 modes (Hor+). */
#define VP_SETPLANE 24
typedef HRESULT (WINAPI *PFN_SETPLANE)(void *vp, float l, float r, float b, float t);
static PFN_SETPLANE g_setplane_orig;
static volatile float g_hscale = 1.0f;

static HRESULT WINAPI viewport_setplane_hook(void *vp, float l, float r, float b, float t)
{
    float s = g_hscale;
    interp_note_race();
    /* first race camera frame after a mode switch: drop the frozen picture now, so
       the loading screen (2D laid out for 4:3) is never seen in widescreen */
    if (g_transition && g_trans_mode_set) {
        ui_hide_cover();
        log_msg("transition: released at first race frame after %lu ms", GetTickCount() - g_trans_start);
    }
    return g_setplane_orig(vp, l * s, r * s, b, t);
}

static void hook_viewport_vtable(void *vp)
{
    void **vt = *(void ***)vp;
    DWORD old;
    if (g_setplane_orig || vt[VP_SETPLANE] == (void *)viewport_setplane_hook) return;
    if (!VirtualProtect(&vt[VP_SETPLANE], sizeof(void *), PAGE_READWRITE, &old)) return;
    g_setplane_orig = (PFN_SETPLANE)vt[VP_SETPLANE];
    vt[VP_SETPLANE] = (void *)viewport_setplane_hook;
    VirtualProtect(&vt[VP_SETPLANE], sizeof(void *), old, &old);
    log_msg("viewport SetPlane hook installed");
}

static HRESULT WINAPI viewport_setback_hook(void *vp, float back)
{
    void **vt = *(void ***)vp;
    HRESULT hr = ((PFN_SETVAL)vt[VP_SETBACK])(vp, back);
    void *dev = G_DEVICE;
    if (hr == 0 && dev) {
        void **dvt = *(void ***)dev;
        DWORD w = ((PFN_GETDIM)dvt[DEV_GETWIDTH])(dev);
        DWORD h = ((PFN_GETDIM)dvt[DEV_GETHEIGHT])(dev);
        hook_viewport_vtable(vp);
        interp_on_viewport(vp);
        /* only true widescreen modes (16:9 = 1.78); 320x200/640x400 (1.6) were
           shown stretched to 4:3 on CRTs, so they keep the original planes */
        g_hscale = (w && h && w * 10 > h * 17) ? ((float)w / (float)h) / (4.0f / 3.0f) : 1.0f;
        log_msg("viewport %lux%lu -> horizontal plane scale %.4f", w, h, g_hscale);
    }
    return hr;
}

/* ---- race HUD overlay ----------------------------------------------------------
 * 0x4075b0 draws the HUD into a 320-wide overlay surface and Blt()s it over the
 * whole back buffer (0x407f25: push edx(dst rect); push eax(dst surf); call
 * [ecx+0x14]). On 16:9 that stretches the HUD wide; we constrain it to a centred
 * 4:3 rectangle. Thunk replaces those 5 bytes with a call. */
typedef HRESULT (WINAPI *PFN_BLT)(void *dst, RECT *dr, void *src, RECT *sr, DWORD fl, void *fx);

/* HUD scale: 0 = fit (full height, 4:3), N = each HUD pixel is N x N screen pixels */
static volatile int g_hud = 0;
/* HUD anchoring: 1 = pieces pinned to their screen sides, 0 = one centred block */
static volatile int g_hud_edges = 1;

/* Edge-anchored layout. Column splits sit in the gaps between HUD elements seen in
 * the 320-wide overlay: portraits x<88, start lights 88..230, lap counter and timer
 * x>=230 (lap top half, timer bottom half). */
#define HUD_SPLIT_L 88
#define HUD_SPLIT_R 230

static HRESULT hud_piece(PFN_BLT blt, void *dst, const RECT *dr, void *src, const RECT *sr,
                         DWORD fl, void *fx, double sx, double sy,
                         int x0, int y0, int x1, int y1, int ax, int ay)
{
    /* ax/ay: 0 = start edge, 1 = centre, 2 = end edge */
    RECT s, d;
    LONG w = dr->right - dr->left, h = dr->bottom - dr->top;
    LONG pw = (LONG)((x1 - x0) * sx + 0.5), ph = (LONG)((y1 - y0) * sy + 0.5);
    LONG dx, dy;
    if (x1 <= x0 || y1 <= y0) return 0;
    SetRect(&s, sr->left + x0, sr->top + y0, sr->left + x1, sr->top + y1);
    if (ax == 0) dx = 0;
    else if (ax == 2) dx = w - pw;
    else dx = (LONG)(w / 2.0 + (x0 - (sr->right - sr->left) / 2.0) * sx + 0.5);  /* keep x=160 at centre */
    if (ay == 0) dy = 0;
    else if (ay == 2) dy = h - ph;
    else dy = (h - ph) / 2;
    SetRect(&d, dr->left + dx, dr->top + dy, dr->left + dx + pw, dr->top + dy + ph);
    return blt(dst, &d, src, &s, fl, fx);
}

static void debugmenu_overlay(void *backbuf);   /* debugmenu.h */
static HRESULT WINAPI hud_blt_inner(void *dst, RECT *dr, void *src, RECT *sr, DWORD fl, void *fx);

/* HUD overlay -> back buffer, then the debug menu on top (races only) */
static void vr_hud_capture(void *src, RECT *sr);   /* vrexport.h */
static HRESULT WINAPI hud_blt_hook(void *dst, RECT *dr, void *src, RECT *sr, DWORD fl, void *fx)
{
    HRESULT hr;
    vr_hud_capture(src, sr);
    hr = hud_blt_inner(dst, dr, src, sr, fl, fx);
    debugmenu_overlay(dst);
    return hr;
}

static HRESULT WINAPI hud_blt_inner(void *dst, RECT *dr, void *src, RECT *sr, DWORD fl, void *fx)
{
    PFN_BLT blt = (PFN_BLT)(*(void ***)dst)[5];
    RECT r = *dr;

    if (g_hud_edges && sr && g_disp_w > 0 && g_disp_h > 0) {
        LONG w = r.right - r.left, h = r.bottom - r.top;
        int sw = sr->right - sr->left, sh = sr->bottom - sr->top;
        double bx = (double)g_mode_w / g_disp_w, by = (double)g_mode_h / g_disp_h;
        double k = g_hud > 0 ? g_hud : (double)g_disp_h / sh;   /* screen px per HUD px */
        double sx = k * bx, sy = k * by;
        if (sw == 320 && sh > 0 && sh * sy <= h + 0.5 && sw * sx <= w + 0.5) {
            HRESULT hr;
            /* the game lays the HUD out for 200 rows unless its hi-res flag (0x6302BC) is set;
               the overlay can still be 240 (six cars draw portraits below 200), so the
               timer column ends at row 200 there */
            int rb = (*(volatile short *)0x006302BC == 0 && sh > 200) ? 200 : sh;
            hr = hud_piece(blt, dst, &r, src, sr, fl, fx, sx, sy, 0, 0, HUD_SPLIT_L, sh, 0, 1);
            if (hr == 0) hr = hud_piece(blt, dst, &r, src, sr, fl, fx, sx, sy, HUD_SPLIT_L, 0, HUD_SPLIT_R, rb, 1, 0);
            if (hr == 0) hr = hud_piece(blt, dst, &r, src, sr, fl, fx, sx, sy, HUD_SPLIT_R, 0, sw, rb / 2, 2, 0);
            if (hr == 0) hr = hud_piece(blt, dst, &r, src, sr, fl, fx, sx, sy, HUD_SPLIT_R, rb / 2, sw, rb, 2, 2);
            return hr;
        }
    }
    LONG w = r.right - r.left, h = r.bottom - r.top;
    int n = g_hud;
    if (n > 0 && sr && g_disp_w > 0 && g_disp_h > 0) {
        /* back-buffer pixels per screen pixel, so the HUD lands on exact screen pixels */
        double bx = (double)g_mode_w / g_disp_w, by = (double)g_mode_h / g_disp_h;
        LONG hw = (LONG)((sr->right - sr->left) * n * bx + 0.5);
        LONG hh = (LONG)((sr->bottom - sr->top) * n * by + 0.5);
        if (hw > w || hh > h) n = 0;              /* too big for this mode: fall back to fit */
        else {
            r.left += (w - hw) / 2; r.right = r.left + hw;
            r.top += (h - hh) / 2;  r.bottom = r.top + hh;
        }
    }
    if (n == 0 && h > 0 && w * 10 > h * 17) {    /* fit, widescreen: centred 4:3 */
        LONG hw = h * 4 / 3;
        r.left += (w - hw) / 2;
        r.right = r.left + hw;
    }
    return blt(dst, &r, src, sr, fl, fx);
}

static __declspec(naked) void hud_blt_thunk(void)
{
    __asm {
        pop ecx                 ; return address (0x407f2a)
        push edx                ; dest rect
        push eax                ; dest surface (this)
        push ecx                ; return address back on top
        jmp hud_blt_hook        ; stdcall, pops 6 args, returns to 0x407f2a
    }
}

/* ---- camera zoom ------------------------------------------------------------------
 * The race camera height is the float at 0x44d3bc (56.0); its pull-back distance
 * is height * 0x4889b4 and its look vector scales with it, so raising the height
 * dollies the camera out along its view line. PageDown = zoom out, PageUp = zoom in,
 * Home = original. Saved in mmcda.ini. The per-frame far clip (0x4032f5, 1024) is
 * raised so a higher camera does not clip the track. */
#define CAM_HEIGHT (*(volatile float *)0x0044D3BC)
static float g_zoom = 1.3f;
static void apply_far_clip(void);

static void save_zoom(void)
{
    char path[MAX_PATH], v[32];
    snprintf(path, sizeof path, "%smmcda.ini", g_home);
    snprintf(v, sizeof v, "%.3f", g_zoom);
    WritePrivateProfileStringA("camera", "zoom", v, path);
    if (g_hud) snprintf(v, sizeof v, "%d", g_hud); else strcpy(v, "fit");
    WritePrivateProfileStringA("hud", "scale", v, path);
    WritePrivateProfileStringA("hud", "anchor", g_hud_edges ? "edges" : "center", path);
}

static void vr_subclass(HWND wnd);   /* vrexport.h */
static DWORD WINAPI zoom_thread(LPVOID p)
{
    int dirty = 0, end_was_down = 0, ins_was_down = 0;
    HWND last_fg = NULL;
    int was_iconic = 0;
    for (;;) {
        HWND wnd = *(HWND *)0x006172A8, fg;
        int changed = 0, end_down, ins_down, iconic;
        Sleep(30);
        /* seamless-transition safety net */
        if (g_transition && GetTickCount() - g_trans_start > 10000) {
            ui_hide_cover();
            log_msg("transition: timeout");
        }
        if (!wnd) continue;
        vr_subclass(wnd);
        /* keep the black side panels directly behind the game */
        fg = GetForegroundWindow();
        iconic = IsIconic(wnd);
        if (fg == g_back && g_back) { SetForegroundWindow(wnd); fg = wnd; }
        if ((fg == wnd) != (last_fg == wnd) || iconic != was_iconic) ui_restack_backdrop();
        last_fg = fg; was_iconic = iconic;
        if (fg != wnd) continue;
        /* Insert: toggle HUD pieces pinned to screen sides vs one centred block */
        ins_down = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        if (ins_down && !ins_was_down) {
            g_hud_edges = !g_hud_edges;
            log_msg("hud anchor %s", g_hud_edges ? "edges" : "center");
            save_zoom();
        }
        ins_was_down = ins_down;
        {   /* F8: toggle the overview camera (whole level from above) */
            static int f8_was_down;
            int f8_down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
            debugmenu_poll();
            if (f8_down && !f8_was_down) {
                g_overview = !g_overview;
                g_cam_mode = g_overview ? 1 : 0;
                log_msg("overview camera %s", g_overview ? "on" : "off");
            }
            f8_was_down = f8_down;
        }
        /* End: cycle HUD scale fit -> 1 -> 2 -> ... -> largest that fits -> fit */
        end_down = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
        if (end_down && !end_was_down) {
            int maxn = 0;
            if (g_disp_h > 0) maxn = min(g_disp_w / 320, g_disp_h / 240);
            if (maxn < 1) maxn = 1;
            g_hud = (g_hud >= maxn) ? 0 : g_hud + 1;
            log_msg("hud scale %s%d", g_hud ? "x" : "fit ", g_hud);
            save_zoom();
        }
        end_was_down = end_down;
        if (GetAsyncKeyState(VK_NEXT) & 0x8000) { g_zoom *= 1.015f; changed = 1; }
        if (GetAsyncKeyState(VK_PRIOR) & 0x8000) { g_zoom /= 1.015f; changed = 1; }
        if (GetAsyncKeyState(VK_HOME) & 0x8000) { g_zoom = 1.0f; changed = 1; }
        if (changed) {
            if (g_zoom < 0.6f) g_zoom = 0.6f;
            if (g_zoom > 3.0f) g_zoom = 3.0f;
            CAM_HEIGHT = 56.0f * g_zoom;
            apply_far_clip();
            dirty = 1;
        } else if (dirty) {
            save_zoom();
            log_msg("camera zoom %.3f", g_zoom);
            dirty = 0;
        }
    }
}

/* Far clip follows the zoom: 1024 (the original) up to zoom 1, then 1024*zoom.
   A big fixed far plane wastes 16-bit depth precision and makes coplanar track
   decals z-fight (flicker). The immediate at 0x4032F5 is left writable. */
static void apply_far_clip(void)
{
    BYTE *site = (BYTE *)0x004032F5;
    float f = g_zoom > 1.0f ? 1024.0f * g_zoom : 1024.0f;
    if (site[0] == 0x68) *(float *)(site + 1) = f;
}

static void install_camera(void)
{
    static const BYTE back_expect[5] = { 0x68, 0x00, 0x00, 0x80, 0x44 };   /* push 1024.0f */
    static const BYTE blt_expect[5]  = { 0x52, 0x50, 0xFF, 0x51, 0x14 };
    char path[MAX_PATH], v[32];
    BYTE *site;
    DWORD old;

    snprintf(path, sizeof path, "%smmcda.ini", g_home);
    GetPrivateProfileStringA("camera", "zoom", "1.3", v, sizeof v, path);
    g_zoom = (float)atof(v);
    if (g_zoom < 0.6f || g_zoom > 3.0f) g_zoom = 1.3f;
    if (*(volatile float *)0x0044D3BC == 56.0f) CAM_HEIGHT = 56.0f * g_zoom;
    GetPrivateProfileStringA("hud", "scale", "fit", v, sizeof v, path);
    g_hud = atoi(v);                                 /* "fit" -> 0 */
    if (g_hud < 0 || g_hud > 16) g_hud = 0;
    GetPrivateProfileStringA("hud", "anchor", "edges", v, sizeof v, path);
    g_hud_edges = lstrcmpiA(v, "center") != 0;

    site = (BYTE *)0x004032F5;
    if (memcmp(site, back_expect, 5) == 0) {
        VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old);
        apply_far_clip();                         /* page stays RWX for later updates */
    }
    site = (BYTE *)0x00407F25;
    if (memcmp(site, blt_expect, 5) == 0) {
        VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old);
        site[0] = 0xE8;
        *(INT32 *)(site + 1) = (INT32)((BYTE *)hud_blt_thunk - (site + 5));
        VirtualProtect(site, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), site, 5);
        log_msg("hud blit hook installed");
    }
    CloseHandle(CreateThread(NULL, 0, zoom_thread, NULL, 0, NULL));
    log_msg("camera zoom %.3f (PageDown out, PageUp in, Home reset)", g_zoom);
}

/* ---- window follows display mode -------------------------------------------
 * 0x4052c0 (cdecl, w, h, bpp) is the game's SetDisplayMode wrapper. Under dgVoodoo
 * the "fullscreen" mode is shown in a window, but the game never resizes that window
 * (a real mode switch would have), so a new mode got squeezed into the old window.
 * We replace the function: same globals + IDirectDraw::SetDisplayMode, then size
 * the client area to the mode's aspect, about 90% of the work area, centred.
 */
static void install_fov_hook(void);
static int patch_jmp(BYTE *site, const BYTE *expect, size_t n, void *target);
#define G_I32(a)  (*(volatile int *)(a))
#define GAME_HWND (*(HWND *)0x006172A8)

/* ---- UI thread: black side panels + freeze-frame cover -----------------------------
 * dgVoodoo stretches the game to the whole client area, so for 4:3 modes (the 2D menu)
 * the game window is sized to the mode's aspect, centred, and a black "backdrop"
 * window fills the rest of the monitor behind it.
 * Every display-mode change tears down and rebuilds DirectDraw/D3DRM, which shows as
 * a black flash. When teardown starts we screenshot the monitor into a click-through
 * topmost "cover" window and remove it after the new mode has presented a few frames.
 * These windows live on their own thread so they paint while the game thread loads. */
static HBITMAP g_cover_bmp;
static RECT g_cover_rect;
static volatile int g_back_needed;
static RECT g_back_rect;

static LRESULT CALLBACK back_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN:
        if (GAME_HWND) SetForegroundWindow(GAME_HWND);
        return 0;
    }
    return DefWindowProcA(w, m, wp, lp);
}

static LRESULT CALLBACK cover_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps), mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, g_cover_bmp);
        BitBlt(dc, 0, 0, g_cover_rect.right - g_cover_rect.left,
               g_cover_rect.bottom - g_cover_rect.top, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteDC(mem);
        EndPaint(w, &ps);
        return 0;
    }
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcA(w, m, wp, lp);
}

static DWORD WINAPI ui_thread(LPVOID ready)
{
    WNDCLASSA wc = {0};
    MSG msg;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpfnWndProc = back_proc; wc.lpszClassName = "MMCDA_Backdrop";
    RegisterClassA(&wc);
    wc.lpfnWndProc = cover_proc; wc.lpszClassName = "MMCDA_Cover";
    RegisterClassA(&wc);
    g_back = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, "MMCDA_Backdrop", "", WS_POPUP,
                             0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);
    g_cover = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                              WS_EX_TRANSPARENT | WS_EX_LAYERED, "MMCDA_Cover", "", WS_POPUP,
                              0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);
    SetLayeredWindowAttributes(g_cover, 0, 255, LWA_ALPHA);
    SetEvent((HANDLE)ready);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    return 0;
}

/* While the game has focus, it and its backdrop are topmost, so they cover the taskbar
 * even when the game window itself is narrower than the monitor (4:3 menu). When
 * focus moves elsewhere they drop back to normal so other apps come in front. */
static void ui_restack_backdrop(void)
{
    HWND game = GAME_HWND;
    int active, iconic;
    if (!game) return;
    if (GetEnvironmentVariableA("MMCDA_WINDOWED", NULL, 0)) return;
    iconic = IsIconic(game);
    active = !iconic && GetForegroundWindow() == game;
    /* synchronous when called on the game's own thread (mode switches), so the
       freeze-frame below is guaranteed to end up above it */
    SetWindowPos(game, active ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                 (GetWindowThreadProcessId(game, NULL) == GetCurrentThreadId() ? 0 : SWP_ASYNCWINDOWPOS));
    if (g_transition && g_cover && IsWindowVisible(g_cover))
        SetWindowPos(g_cover, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (!g_back) return;
    if (g_back_needed && !iconic)
        SetWindowPos(g_back, game, g_back_rect.left, g_back_rect.top,
                     g_back_rect.right - g_back_rect.left, g_back_rect.bottom - g_back_rect.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS);
    else
        ShowWindowAsync(g_back, SW_HIDE);
}

static void ui_show_cover(void)
{
    HWND game = GAME_HWND;
    MONITORINFO mi = { sizeof mi };
    HDC scr, mem;
    if (!g_cover || !game || !IsWindowVisible(game) || IsIconic(game) || GetForegroundWindow() != game)
        return;
    GetMonitorInfoA(MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST), &mi);
    g_cover_rect = mi.rcMonitor;
    scr = GetDC(NULL); mem = CreateCompatibleDC(scr);
    if (g_cover_bmp) DeleteObject(g_cover_bmp);
    g_cover_bmp = CreateCompatibleBitmap(scr, g_cover_rect.right - g_cover_rect.left,
                                         g_cover_rect.bottom - g_cover_rect.top);
    SelectObject(mem, g_cover_bmp);
    BitBlt(mem, 0, 0, g_cover_rect.right - g_cover_rect.left, g_cover_rect.bottom - g_cover_rect.top,
           scr, g_cover_rect.left, g_cover_rect.top, SRCCOPY);
    DeleteDC(mem); ReleaseDC(NULL, scr);
    InvalidateRect(g_cover, NULL, FALSE);
    SetWindowPos(g_cover, HWND_TOPMOST, g_cover_rect.left, g_cover_rect.top,
                 g_cover_rect.right - g_cover_rect.left, g_cover_rect.bottom - g_cover_rect.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateWindow(g_cover);
}

static void ui_hide_cover(void)
{
    if (g_cover) ShowWindowAsync(g_cover, SW_HIDE);
    InterlockedExchange(&g_transition, 0);
}
#define G_I16(a)  (*(volatile short *)(a))
typedef HRESULT (WINAPI *PFN_SETMODE)(void *dd, DWORD w, DWORD h, DWORD bpp);

static void fit_window(int w, int h)
{
    HWND wnd = *(HWND *)0x006172A8;
    MONITORINFO mi = { sizeof mi };
    RECT rc;
    DWORD style, exstyle;
    double s, aw, ah;
    if (!wnd || w <= 0 || h <= 0) return;
    g_mode_w = w; g_mode_h = h;
    if (w * 5 == h * 8) h = w * 3 / 4;   /* 320x200 / 640x400: CRT showed these at 4:3 */
    GetMonitorInfoA(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi);
    {   /* image size on screen when the client is the whole monitor (borderless) */
        double mw = mi.rcMonitor.right - mi.rcMonitor.left, mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
        double k = min(mw / w, mh / h);
        g_disp_w = (int)(w * k); g_disp_h = (int)(h * k);
    }

    /* Default: borderless fullscreen at the monitor's current Windows resolution.
       dgVoodoo (windowed, ScalingMode=stretched_ar) scales the game into it,
       keeping aspect. MMCDA_WINDOWED=1 keeps the captioned window instead. */
    {
        char v[2];
        if (!GetEnvironmentVariableA("MMCDA_WINDOWED", v, sizeof v)) {
            /* game window at the mode's aspect, centred; black backdrop behind it */
            RECT m = mi.rcMonitor, g;
            LONG gw = g_disp_w, gh = g_disp_h;
            SetRect(&g, m.left + ((m.right - m.left) - gw) / 2, m.top + ((m.bottom - m.top) - gh) / 2, 0, 0);
            g.right = g.left + gw; g.bottom = g.top + gh;
            style = (GetWindowLongA(wnd, GWL_STYLE) & ~(WS_OVERLAPPEDWINDOW | WS_DLGFRAME | WS_BORDER))
                    | WS_POPUP | WS_VISIBLE;
            G_I32(0x00617640) = 1;
            SetWindowLongA(wnd, GWL_STYLE, style);
            SetWindowPos(wnd, HWND_TOP, g.left, g.top, gw, gh, SWP_FRAMECHANGED | SWP_NOACTIVATE);
            G_I32(0x00617640) = 0;
            g_back_rect = m;
            g_back_needed = (gw < m.right - m.left - 1) || (gh < m.bottom - m.top - 1);
            ui_restack_backdrop();
            log_msg("mode %dx%d -> borderless %ldx%ld at %ld,%ld%s", w, h, gw, gh, g.left, g.top,
                    g_back_needed ? " (black sides)" : "");
            return;
        }
    }
    aw = (mi.rcWork.right - mi.rcWork.left) * 0.9;
    ah = (mi.rcWork.bottom - mi.rcWork.top) * 0.9 - GetSystemMetrics(SM_CYCAPTION);
    s = min(aw / w, ah / h);
    SetRect(&rc, 0, 0, (int)(w * s), (int)(h * s));
    style = GetWindowLongA(wnd, GWL_STYLE);
    exstyle = GetWindowLongA(wnd, GWL_EXSTYLE);
    g_disp_w = (int)(w * s); g_disp_h = (int)(h * s);   /* windowed: image fills client */
    AdjustWindowRectEx(&rc, style, FALSE, exstyle);
    G_I32(0x00617640) = 1;   /* game's "ignore WM_SIZE while we resize" flag */
    SetWindowPos(wnd, NULL,
                 mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - (rc.right - rc.left)) / 2,
                 mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - (rc.bottom - rc.top)) / 2,
                 rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
    G_I32(0x00617640) = 0;
    log_msg("mode %dx%d -> window client %dx%d", w, h, (int)(w * s), (int)(h * s));
}

static int __cdecl set_display_mode_hook(int w, int h, int bpp)
{
    void *dd = *(void **)0x00489A18;
    HRESULT hr;
    G_I32(0x00617610) = w;
    G_I32(0x00617614) = h;
    G_I32(0x00617618) = bpp;
    G_I16(0x0062FA48) = (short)w;
    G_I16(0x0062FB08) = (short)h;
    G_I32(0x00617640) = 1;
    hr = ((PFN_SETMODE)(*(void ***)dd)[21])(dd, w, h, bpp);   /* IDirectDraw::SetDisplayMode */
    G_I32(0x00617640) = 0;
    G_I32(0x0062FA40) = hr;
    if (hr == 0) fit_window(w, h);
    if (g_transition) InterlockedExchange(&g_trans_mode_set, 1);
    return hr == 0;
}

/* 0x4137c0: first step of every display teardown (calls the D3D device-release
 * callback). Freeze the current picture before the screen goes black. */
typedef int (__cdecl *PFN_VOIDINT)(void);
static PFN_VOIDINT g_teardown_orig;
static int __cdecl teardown_hook(void)
{
    interp_on_teardown();
    if (!g_transition) {
        g_trans_mode_set = 0; g_trans_presents = 0; g_trans_start = GetTickCount();
        ui_show_cover();
        if (IsWindowVisible(g_cover)) { InterlockedExchange(&g_transition, 1); log_msg("transition: frozen"); }
    }
    return g_teardown_orig();
}

/* 0x4058a0 (cdecl, flags): the game's present (Flip/Blt to primary). Once the new
 * mode has shown a few frames, drop the frozen picture. */
typedef int (__cdecl *PFN_PRESENT)(unsigned int);
static PFN_PRESENT g_present_orig;
static void vr_screen_capture(void);   /* vrexport.h */
static void pad_setup_frame(void);     /* pad.h */
static int __cdecl present_hook(unsigned int f)
{
    pad_setup_frame();
    vr_screen_capture();
    /* The game normally copies only the regions D3DRM drew this frame (its dirty-rect
       list) to the screen. Spots where nothing is drawn (e.g. building doorways with no
       interior) then keep whatever an older frame left there, which flickers. Bit 0
       asks for a full-screen copy every frame; costs nothing on modern GPUs. */
    int r = g_present_orig(f | 1);
    {   /* frame-time statistics (logged every 100 presents with MMCDA_LOG=1) */
        static LARGE_INTEGER freq, last;
        static double sum, worst, best = 1e9;
        static int n;
        LARGE_INTEGER now;
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        g_last_present_ms = now.QuadPart * 1000.0 / freq.QuadPart;
        if (last.QuadPart) {
            double ms = (now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart;
            if (ms > 3.0 && ms < 40.0)   /* vsync interval estimate (EMA) */
                g_present_dt = g_present_dt * 0.95 + ms * 0.05;
            if (ms < 250) {                         /* ignore loading pauses */
                sum += ms; n++;
                if (ms > worst) worst = ms;
                if (ms < best) best = ms;
                if (n == 100) {
                    log_msg("frames: avg %.2f ms (%.1f fps), min %.2f, max %.2f, mode %dx%d",
                            sum / n, 1000.0 * n / sum, best, worst, g_mode_w, g_mode_h);
                    sum = 0; n = 0; worst = 0; best = 1e9;
                }
            }
        }
        last = now;
    }
    /* menu modes (320 wide) release after a few frames; race modes wait for the
       first 3D race frame (see viewport_setplane_hook) */
    if (g_transition && g_trans_mode_set && g_mode_w == 320 &&
        InterlockedIncrement(&g_trans_presents) >= 3) {
        ui_hide_cover();
        log_msg("transition: released after %lu ms", GetTickCount() - g_trans_start);
    }
    return r;
}

static void *make_trampoline(const BYTE *orig, size_t n, BYTE *resume)
{
    BYTE *t = (BYTE *)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return NULL;
    memcpy(t, orig, n);
    t[n] = 0xE9;
    *(INT32 *)(t + n + 1) = (INT32)(resume - (t + n + 5));
    return t;
}

static void install_transition_hooks(void)
{
    static const BYTE td_expect[5] = { 0xA1, 0x58, 0x93, 0x54, 0x00 };          /* mov eax,[0x549358] */
    static const BYTE pr_expect[9] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xC4, 0x03, 0x00, 0x00 };
    /* called under the loader lock (DllMain): start the UI thread but don't wait for it */
    HANDLE ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    char v[2];
    if (GetEnvironmentVariableA("MMCDA_NOSEAMLESS", v, sizeof v)) return;
    CloseHandle(CreateThread(NULL, 0, ui_thread, ready, 0, NULL));
    if (memcmp((BYTE *)0x004137C0, td_expect, 5) == 0 && memcmp((BYTE *)0x004058A0, pr_expect, 9) == 0) {
        g_teardown_orig = (PFN_VOIDINT)make_trampoline(td_expect, 5, (BYTE *)0x004137C5);
        g_present_orig = (PFN_PRESENT)make_trampoline(pr_expect, 9, (BYTE *)0x004058A9);
        if (g_teardown_orig && g_present_orig &&
            patch_jmp((BYTE *)0x004137C0, td_expect, 5, teardown_hook) &&
            patch_jmp((BYTE *)0x004058A0, pr_expect, 9, present_hook))
            log_msg("seamless-transition hooks installed");
    } else log_msg("transition sites mismatch, skipped");
}

static int patch_jmp(BYTE *site, const BYTE *expect, size_t n, void *target)
{
    DWORD old;
    if (memcmp(site, expect, n) != 0) return 0;
    VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old);
    site[0] = 0xE9;
    *(INT32 *)(site + 1) = (INT32)((BYTE *)target - (site + 5));
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return 1;
}

/* ---- display-mode list -------------------------------------------------------
 * 0x406a40 is the EnumDisplayModes callback (stdcall). It stores modes up to
 * 1024x768 into a fixed 50-entry table at 0x6172f0 (the buffer ends where the next
 * globals start, so the cap cannot be raised). Modern drivers / dgVoodoo report
 * every size at 8, 16 and 32 bpp, so the table filled before the larger and 16:9
 * sizes arrived. Keep the 16-bit modes the game renders in, plus 320x200x8 (its
 * built-in fallback).
 */
static HRESULT WINAPI enum_modes_hook(BYTE *desc, void *ctx)
{
    DWORD h = *(DWORD *)(desc + 0x08), w = *(DWORD *)(desc + 0x0C), bpp = *(DWORD *)(desc + 0x54);
    int n = G_I32(0x00617620);
    DWORD *slot;
    if (w > 0x400 || h > 0x300) return 1;
    if (!(bpp == 16 || (bpp == 8 && w == 320 && h == 200))) return 1;
    /* The in-game Display menu only offers 320x200/320x240/640x480/800x600, so the
       800x600 slot is reported as the widescreen mode (MMCDA_WIDE, default
       1024x576; MMCDA_WIDE=off keeps real 800x600). The real wide mode is not
       listed separately. */
    if (bpp == 16) {
        static DWORD ww, wh; static int init;
        if (!init) {
            char v[32] = "1024x576";
            GetEnvironmentVariableA("MMCDA_WIDE", v, sizeof v);
            if (sscanf(v, "%lux%lu", &ww, &wh) != 2 || ww > 0x400 || wh > 0x300) ww = wh = 0;
            init = 1;
        }
        if (ww && w == ww && h == wh) return 1;
        if (ww && w == 800 && h == 600) {
            w = ww; h = wh;
            /* 0x405160 re-selects the mode matching the remembered size right after
               enumeration; keep that match pointing at the swapped entry */
            if (G_I16(0x0062FA48) == 800 && G_I16(0x0062FB08) == 600) {
                G_I16(0x0062FA48) = (short)ww; G_I16(0x0062FB08) = (short)wh;
            }
        }
    }
    if (n >= 50) return 0;
    slot = (DWORD *)(0x006172F0 + n * 16);
    slot[0] = w; slot[1] = h; slot[2] = bpp; slot[3] = 0;
    G_I32(0x00617620) = n + 1;
    log_msg("mode offered %lux%lux%lu", w, h, bpp);
    return n + 1 != 50;
}

/* ---- texture-name buffer overflow ------------------------------------------------
 * 0x42b540 is the D3DRM load-texture callback (cdecl: name, arg, out texture). It
 * copies the name up to the first '.' into a 16-byte stack buffer with no bound
 * (it only uses the first 8 chars, i.e. 8.3 names). When D3DRM hands it a name with
 * a directory in front, the copy runs over the saved EBP/return address (crash with
 * EIP = "2D.b", EBP = "SNOW"). We pass only the base name, then run the original.
 */
typedef int (__cdecl *PFN_TEXCB)(char *name, void *arg, void *out);
static PFN_TEXCB g_texcb_orig;

static int __cdecl texture_callback_hook(char *name, void *arg, void *out)
{
    char *base = name, *p;
    if (name) {
        for (p = name; *p; p++)
            if (*p == '\\' || *p == '/' || *p == ':') base = p + 1;
        if (base != name) log_msg("texture name trimmed: %s -> %s", name, base);
    }
    {
        int r = g_texcb_orig(base, arg, out);
        if (r != 0) log_msg("texture load FAILED %d: %s", r, name ? name : "(null)");
        return r;
    }
}

static void install_texcb_hook(void)
{
    static const BYTE expect[9] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x88, 0x01, 0x00, 0x00 };
    BYTE *site = (BYTE *)0x0042B540, *tramp;
    if (memcmp(site, expect, 9) != 0) { log_msg("texcb site mismatch, hook skipped"); return; }
    tramp = (BYTE *)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return;
    memcpy(tramp, expect, 9);                       /* push ebp; mov ebp,esp; sub esp,0x188 */
    tramp[9] = 0xE9;                                /* jmp 0x42b549 */
    *(INT32 *)(tramp + 10) = (INT32)((site + 9) - (tramp + 14));
    g_texcb_orig = (PFN_TEXCB)tramp;
    log_msg(patch_jmp(site, expect, 9, texture_callback_hook)
            ? "texture-callback hook installed" : "texcb patch failed");
}

/* ---- packed-texture lookup overflow (the real race-load crash) -------------------
 * 0x42b1f0 (cdecl, name) looks a texture up in the packed track archive. It copies
 * the whole name into a 12-byte stack buffer and compares 8 chars against archive
 * entries, so it expects a bare 8.3 base name ("SNOW2D"). Its caller decides between
 * "loose file path" and "archive name" from the flag at 0x504fb0, but this function
 * re-reads that flag, and the race loader changes it while loading; when they
 * disagree a full path ("Arctic\maps\SNOW2D.bmp") arrives and smashes the stack
 * (crash with EBP="SNOW", EIP="2D.b"). Reduce any input to the 8-char base name.
 */
typedef void *(__cdecl *PFN_PAKLOOKUP)(char *name);
static PFN_PAKLOOKUP g_paklookup_orig;

static void *__cdecl pak_lookup_hook(char *name)
{
    char base[16];
    const char *s = name, *p;
    int n = 0;
    for (p = name; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':') s = p + 1;
    while (s[n] && s[n] != '.' && n < 8) { base[n] = s[n]; n++; }
    base[n] = 0;
    {
        void *r = g_paklookup_orig(base);
        if (!r) log_msg("archive texture NOT FOUND: %s (%s)", base, name);
        return r;
    }
}

static void install_pak_hook(void)
{
    static const BYTE expect[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C };
    BYTE *site = (BYTE *)0x0042B1F0, *tramp;
    if (memcmp(site, expect, 6) != 0) { log_msg("pak site mismatch, hook skipped"); return; }
    tramp = (BYTE *)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return;
    memcpy(tramp, expect, 6);                       /* push ebp; mov ebp,esp; sub esp,0xc */
    tramp[6] = 0xE9;                                /* jmp 0x42b1f6 */
    *(INT32 *)(tramp + 7) = (INT32)((site + 6) - (tramp + 11));
    g_paklookup_orig = (PFN_PAKLOOKUP)tramp;
    log_msg(patch_jmp(site, expect, 6, pak_lookup_hook)
            ? "archive-lookup hook installed" : "pak patch failed");
}

#include "interp.h"
#include "tiles.h"
#include "debugmenu.h"
#include "vrexport.h"
#include "pad.h"
#include "sixcars.h"
#include "car.h"
#include "cams.h"
static void interp_note_race(void) { g_race_seen = 1; }
static void interp_on_viewport(void *vp) { install_interp_vtables(vp); }
static void interp_on_teardown(void) { tiles_release(); interp_reset(); vr_on_teardown(); }

static void install_hooks(void)
{
    install_texcb_hook();
    install_pak_hook();
    static const BYTE mode_expect[8] = { 0x55, 0x8B, 0xEC, 0x8B, 0x55, 0x10, 0x8B, 0x45 };
    static const BYTE enum_expect[8] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x8B, 0x48 };
    if ((BYTE *)GetModuleHandleA(NULL) != (BYTE *)0x400000) return;
    install_sixcars();   /* first: moves tables before other hooks copy any code */
    log_msg(patch_jmp((BYTE *)0x004052C0, mode_expect, 8, set_display_mode_hook)
            ? "window-fit hook installed" : "mode site mismatch, window-fit hook skipped");
    log_msg(patch_jmp((BYTE *)0x00406A40, enum_expect, 8, enum_modes_hook)
            ? "mode-list hook installed" : "enum site mismatch, mode-list hook skipped");
    install_fov_hook();
    install_camera();
    install_transition_hooks();
    install_interp();
    install_tiles();
    install_debugmenu();
    install_vrexport();
    install_pad();
    install_car();
}

static void install_fov_hook(void)
{
    static const BYTE expect[5] = { 0x8B, 0x10, 0xFF, 0x52, 0x3C };
    BYTE *site = (BYTE *)0x0042A632;
    DWORD old;
    char buf[2];
    if (GetEnvironmentVariableA("MMCDA_NOWIDE", buf, sizeof buf)) return;
    if ((BYTE *)GetModuleHandleA(NULL) != (BYTE *)0x400000) return;
    if (memcmp(site, expect, 5) != 0) { log_msg("fov site mismatch, hook skipped"); return; }
    VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old);
    site[0] = 0xE8;  /* call rel32: stack already holds (vp, 5000.0f) for a stdcall */
    *(INT32 *)(site + 1) = (INT32)((BYTE *)viewport_setback_hook - (site + 5));
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    log_msg("fov hook installed");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID r)
{
    if (reason == DLL_PROCESS_ATTACH) {
        char *slash;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_lock);
        g_done = CreateEventA(NULL, FALSE, FALSE, NULL);
        GetModuleFileNameA(NULL, g_dir, MAX_PATH);
        if ((slash = strrchr(g_dir, '\\'))) slash[1] = 0;
        /* the launcher keeps settings/log in a folder that survives updates */
        if (GetEnvironmentVariableA("MMCDA_HOME", g_home, MAX_PATH - 2) > 0) {
            size_t n = strlen(g_home);
            if (g_home[n - 1] != '\\') { g_home[n] = '\\'; g_home[n + 1] = 0; }
        } else strcpy(g_home, g_dir);
        install_hooks();
    }
    return TRUE;
}
