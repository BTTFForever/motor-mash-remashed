/* ==== Steam Controller (2026) trackpad haptics, written straight to the controller =======
 * Included by mmcda.c before pad.h's users (pad.h calls sc_haptic).
 *
 * The controller's haptic commands are short HID output reports (SDL main,
 * SDL_hidapi_steam_triton.c / controller_structs.h): 0x81 pulse, 0x82 command, 0x83 LFO
 * tone, 0x84 log sweep, 0x85 script. SDL 3.4.20 does not pass them through
 * (SDL_SendGamepadEffect answers "not supported"), so open the controller's vendor HID
 * interface ourselves, shared with SDL, and write them. Valve VID 0x28DE: 0x1302 wired,
 * 0x1304 the Puck receiver; the vendor interface has usage page 0xFF00 or above.
 */
#include <setupapi.h>
#include <hidsdi.h>
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

static HANDLE g_sc_dev = INVALID_HANDLE_VALUE;
static int g_sc_pid;
static USHORT g_sc_outlen;          /* the interface's output report length (Windows pads to it) */
static DWORD g_sc_tried;

static int sc_open(void)
{
    GUID hid;
    HDEVINFO set;
    SP_DEVICE_INTERFACE_DATA ifd;
    DWORD i;
    if (g_sc_dev != INVALID_HANDLE_VALUE) return 1;
    if (g_sc_tried && GetTickCount() - g_sc_tried < 5000) return 0;    /* retry every 5 s */
    g_sc_tried = GetTickCount();
    HidD_GetHidGuid(&hid);
    set = SetupDiGetClassDevsA(&hid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 0;
    ifd.cbSize = sizeof ifd;
    for (i = 0; SetupDiEnumDeviceInterfaces(set, NULL, &hid, i, &ifd); i++) {
        BYTE buf[1024];
        SP_DEVICE_INTERFACE_DETAIL_DATA_A *det = (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)buf;
        HANDLE h;
        HIDD_ATTRIBUTES at;
        PHIDP_PREPARSED_DATA pp;
        HIDP_CAPS caps;
        det->cbSize = sizeof *det;
        if (!SetupDiGetDeviceInterfaceDetailA(set, &ifd, det, sizeof buf, NULL, NULL)) continue;
        h = CreateFileA(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        at.Size = sizeof at;
        if (HidD_GetAttributes(h, &at) && at.VendorID == 0x28DE && (at.ProductID == 0x1302 || at.ProductID == 0x1304) &&
            HidD_GetPreparsedData(h, &pp)) {
            int good = HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS && caps.UsagePage >= 0xFF00 && caps.OutputReportByteLength;
            HidD_FreePreparsedData(pp);
            if (good) {
                g_sc_dev = h; g_sc_outlen = caps.OutputReportByteLength; g_sc_pid = at.ProductID;
                log_msg("steamhap: haptics interface %04X:%04X, output reports %u bytes", at.VendorID, at.ProductID, caps.OutputReportByteLength);
                break;
            }
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    return g_sc_dev != INVALID_HANDLE_VALUE;
}

/* write one haptic output report (report ID in byte 0); 1 on success */
static int sc_haptic(const BYTE *rep, int n)
{
    BYTE buf[128] = { 0 };
    DWORD done = 0;
    if (!sc_open()) return 0;
    if (n > (int)sizeof buf) return 0;
    memcpy(buf, rep, n);
    if (HidD_SetOutputReport(g_sc_dev, buf, g_sc_outlen > n ? g_sc_outlen : n)) return 1;
    if (WriteFile(g_sc_dev, buf, g_sc_outlen > n ? g_sc_outlen : n, &done, NULL)) return 1;
    {
        static int nfail;
        if (nfail++ < 5) log_msg("steamhap: write of report %02X failed (error %lu)", rep[0], GetLastError());
    }
    CloseHandle(g_sc_dev); g_sc_dev = INVALID_HANDLE_VALUE;       /* reopen next time */
    return 0;
}

/* ---- audio through the haptics (PCM) ------------------------------------------------------
 * Protocol as documented by the open-source LiveHaptics (MIT) and SteamHapticsPlayer tools:
 *   0x86 mode report [0x86, op, side, format]: op 1 = off, 2 = on, for sides 2 and 5 (the two
 *        pads); format 8 = 8-bit u-law 8 kHz (Puck / wireless), 0 = 16-bit 8 kHz (wired 0x1302).
 *        The controller wants ~200 ms between switching off and on.
 *   0x88 audio report, 64 bytes: u-law: [0x88, 31, 31 left samples, 31 right samples], one
 *        every 31 / 8000 s (3.875 ms); 16-bit: [0x88, 30, 15 left (LE), 15 right (at 33)], one
 *        every 1.875 ms.
 * Reimplemented here from that description. */

static BYTE sc_ulaw(int s)          /* G.711 u-law from 16-bit linear */
{
    int sign = (s >> 8) & 0x80, exp = 7, mask;
    if (sign) s = -s;
    if (s > 32635) s = 32635;
    s += 0x84;
    for (mask = 0x4000; exp > 0 && !(s & mask); exp--, mask >>= 1) {}
    return (BYTE)~(sign | (exp << 4) | ((s >> (exp + 3)) & 0x0F));
}

static int sc_write64(const BYTE *r)
{
    BYTE buf[128] = { 0 };
    DWORD done = 0;
    if (g_sc_dev == INVALID_HANDLE_VALUE) return 0;
    memcpy(buf, r, 64);
    return WriteFile(g_sc_dev, buf, g_sc_outlen > 64 ? g_sc_outlen : 64, &done, NULL) != 0;
}

static void sc_pcm_mode(int on)
{
    BYTE r[64] = { 0 };
    int fmt = g_sc_pid == 0x1302 ? 0 : 8, side;
    for (side = 2; side <= 5; side += 3) { r[0] = 0x86; r[1] = 1; r[2] = (BYTE)side; r[3] = 0; sc_write64(r); }
    if (!on) return;
    Sleep(10);
    for (side = 2; side <= 5; side += 3) { r[0] = 0x86; r[1] = 2; r[2] = (BYTE)side; r[3] = (BYTE)fmt; sc_write64(r); }
    Sleep(200);
}

/* a WAV file as 8 kHz mono 16-bit (linear resample); caller frees; NULL if unreadable */
static short *sc_load_wav(const char *path, int *count)
{
    FILE *f = fopen(path, "rb");
    BYTE *d; long n; int ch = 1, rate = 22050, bits = 16, i, frames, out;
    const BYTE *pcm = NULL; DWORD pcmlen = 0, p;
    short *res;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    d = (BYTE *)malloc(n); if (!d) { fclose(f); return NULL; }
    fread(d, 1, n, f); fclose(f);
    for (p = 12; p + 8 <= (DWORD)n; p += 8 + ((*(DWORD *)(d + p + 4) + 1) & ~1u)) {
        DWORD sz = *(DWORD *)(d + p + 4);
        if (!memcmp(d + p, "fmt ", 4)) { ch = *(WORD *)(d + p + 10); rate = *(DWORD *)(d + p + 12); bits = *(WORD *)(d + p + 22); }
        else if (!memcmp(d + p, "data", 4)) { pcm = d + p + 8; pcmlen = sz; if (p + 8 + sz > (DWORD)n) pcmlen = n - p - 8; }
    }
    if (!pcm || (bits != 8 && bits != 16) || ch < 1) { free(d); return NULL; }
    frames = pcmlen / (ch * bits / 8);
    out = (int)((double)frames * 8000 / rate);
    res = (short *)malloc(sizeof(short) * (out + 1));
    for (i = 0; res && i < out; i++) {
        double t = (double)i * rate / 8000; int a = (int)t, b = a + 1 < frames ? a + 1 : a; double k = t - a;
        double va = bits == 16 ? ((short *)pcm)[a * ch] : (pcm[a * ch] - 128) * 256;
        double vb = bits == 16 ? ((short *)pcm)[b * ch] : (pcm[b * ch] - 128) * 256;
        res[i] = (short)(va + (vb - va) * k);
    }
    free(d);
    *count = res ? out : 0;
    return res;
}

/* play samples (8 kHz mono) on both pads, blocking; gain multiplies the level */
static void sc_play_pcm(const short *s, int count, float gain)
{
    LARGE_INTEGER fq, t0, now;
    int pos = 0, pkt = 0, per = g_sc_pid == 0x1302 ? 15 : 31;
    double period = (double)per / 8000;
    QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0);
    while (pos < count + per * 8) {                  /* + a little silence at the end */
        BYTE r[64] = { 0 };
        int i;
        r[0] = 0x88;
        r[1] = (BYTE)(per == 31 ? 31 : 30);
        for (i = 0; i < per; i++, pos++) {
            int v = pos < count ? (int)(s[pos] * gain) : 0;
            if (v > 32767) v = 32767; if (v < -32768) v = -32768;
            if (per == 31) { r[2 + i] = sc_ulaw(v); r[33 + i] = sc_ulaw(v); }
            else { *(short *)(r + 2 + i * 2) = (short)v; *(short *)(r + 33 + i * 2) = (short)v; }
        }
        if (!sc_write64(r)) { log_msg("steamhap: audio write failed (error %lu)", GetLastError()); return; }
        pkt++;
        do { QueryPerformanceCounter(&now); if ((double)(now.QuadPart - t0.QuadPart) / fq.QuadPart < pkt * period - 0.002) Sleep(1); }
        while ((double)(now.QuadPart - t0.QuadPart) / fq.QuadPart < pkt * period);
    }
}

/* ---- the controller as a speaker: a small mixer for the game's own sound files ----------
 * sc_sound(name, rate, gain) plays Audio\<name>.wav on the pads, stepping through the file
 * at `rate` samples a second (the rate the game itself plays it at: its freq * 12), mixed
 * with up to 3 others. A thread keeps the pads in audio mode and streams while anything
 * plays; files are loaded once and kept. */
#define SC_VOICES 4
typedef struct { char name[16]; short *s; int n; } sc_clip;
static sc_clip g_sc_clips[96];
static int g_sc_nclips;
static struct { const short *s; int n; double pos, step; float gain; int loop, tag; volatile DWORD keep; } g_sc_voice[SC_VOICES];
static CRITICAL_SECTION g_sc_lock;
static HANDLE g_sc_wake;
static volatile int g_sc_speaker = 100; /* setting: the Steam Controller plays the game's sounds, volume % (0 off) */

/* the whole file as 16-bit mono at its own rate */
static short *sc_load_raw(const char *path, int *count)
{
    FILE *f = fopen(path, "rb");
    BYTE *d; long n; int ch = 1, bits = 16, i, frames;
    const BYTE *pcm = NULL; DWORD pcmlen = 0, p;
    short *res;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    d = (BYTE *)malloc(n); if (!d) { fclose(f); return NULL; }
    fread(d, 1, n, f); fclose(f);
    for (p = 12; p + 8 <= (DWORD)n; p += 8 + ((*(DWORD *)(d + p + 4) + 1) & ~1u)) {
        DWORD sz = *(DWORD *)(d + p + 4);
        if (!memcmp(d + p, "fmt ", 4)) { ch = *(WORD *)(d + p + 10); bits = *(WORD *)(d + p + 22); }
        else if (!memcmp(d + p, "data", 4)) { pcm = d + p + 8; pcmlen = sz; if (p + 8 + sz > (DWORD)n) pcmlen = n - p - 8; }
    }
    if (!pcm || (bits != 8 && bits != 16) || ch < 1) { free(d); return NULL; }
    frames = pcmlen / (ch * bits / 8);
    res = (short *)malloc(sizeof(short) * (frames + 1));
    for (i = 0; res && i < frames; i++) res[i] = bits == 16 ? ((short *)pcm)[i * ch] : (short)((pcm[i * ch] - 128) * 256);
    free(d);
    *count = res ? frames : 0;
    return res;
}

static sc_clip *sc_clip_get(const char *name)
{
    int i;
    char path[MAX_PATH];
    for (i = 0; i < g_sc_nclips; i++) if (!lstrcmpiA(g_sc_clips[i].name, name)) return g_sc_clips[i].s ? &g_sc_clips[i] : NULL;
    if (g_sc_nclips >= (int)(sizeof g_sc_clips / sizeof g_sc_clips[0])) return NULL;
    i = g_sc_nclips++;
    lstrcpynA(g_sc_clips[i].name, name, sizeof g_sc_clips[i].name);
    snprintf(path, sizeof path, "%sAudio\\%s.wav", g_dir, name);
    g_sc_clips[i].s = sc_load_raw(path, &g_sc_clips[i].n);
    if (!g_sc_clips[i].s) log_msg("steamhap: no sound file %s", path);
    return g_sc_clips[i].s ? &g_sc_clips[i] : NULL;
}

static DWORD WINAPI sc_audio_thread(LPVOID p)
{
    LARGE_INTEGER fq, t0, now;
    int pkt = 0, mode_on = 0, idle = 0, per;
    double period;
    (void)p;
    QueryPerformanceFrequency(&fq);
    for (;;) {
        int any, v, i;
        EnterCriticalSection(&g_sc_lock);
        for (any = 0, v = 0; v < SC_VOICES; v++) if (g_sc_voice[v].s) any = 1;
        LeaveCriticalSection(&g_sc_lock);
        if (!any) {
            if (mode_on && ++idle > 400) {           /* ~4 s quiet: leave audio mode (frees the pads) */
                sc_pcm_mode(0); mode_on = 0;
            }
            WaitForSingleObject(g_sc_wake, 10);
            pkt = 0;
            continue;
        }
        idle = 0;
        if (!sc_open()) { Sleep(200); continue; }
        if (!mode_on) { sc_pcm_mode(1); mode_on = 1; }
        per = g_sc_pid == 0x1302 ? 15 : 31;
        period = (double)per / 8000;
        if (!pkt) QueryPerformanceCounter(&t0);
        {
            BYTE r[64] = { 0 };
            r[0] = 0x88; r[1] = (BYTE)(per == 31 ? 31 : 30);
            EnterCriticalSection(&g_sc_lock);
            for (i = 0; i < per; i++) {
                double acc = 0;
                int s;
                for (v = 0; v < SC_VOICES; v++) {
                    if (!g_sc_voice[v].s) continue;
                    if (g_sc_voice[v].loop && (int)(GetTickCount() - g_sc_voice[v].keep) > 0) { g_sc_voice[v].s = NULL; continue; }
                    if ((int)g_sc_voice[v].pos >= g_sc_voice[v].n) {
                        if (!g_sc_voice[v].loop) { g_sc_voice[v].s = NULL; continue; }
                        g_sc_voice[v].pos -= g_sc_voice[v].n;                       /* a looping sound */
                    }
                    acc += g_sc_voice[v].s[(int)g_sc_voice[v].pos] * g_sc_voice[v].gain;
                    g_sc_voice[v].pos += g_sc_voice[v].step;
                }
                s = (int)acc;
                if (s > 32767) s = 32767; if (s < -32768) s = -32768;
                if (per == 31) { r[2 + i] = sc_ulaw(s); r[33 + i] = sc_ulaw(s); }
                else { *(short *)(r + 2 + i * 2) = (short)s; *(short *)(r + 33 + i * 2) = (short)s; }
            }
            LeaveCriticalSection(&g_sc_lock);
            if (!sc_write64(r)) { mode_on = 0; Sleep(200); continue; }
        }
        pkt++;
        do { QueryPerformanceCounter(&now); if ((double)(now.QuadPart - t0.QuadPart) / fq.QuadPart < pkt * period - 0.002) Sleep(1); }
        while ((double)(now.QuadPart - t0.QuadPart) / fq.QuadPart < pkt * period);
    }
}

/* play a game sound file on the pads (rate: samples per second the game plays it at).
   loop: keep repeating while sc_keep(tag) is called (each game tick), else it stops ~150 ms
   after the last call; tag: the game's sample number. */
static void sc_sound(const char *name, int rate, float gain, int loop, int tag)
{
    static int started;
    sc_clip *c;
    int v, pick = 0;
    double least = 1e30;
    if (g_sc_speaker <= 0 || !name || !*name) return;
    gain *= g_sc_speaker / 100.0f;
    if (!started) {
        started = 1;
        InitializeCriticalSection(&g_sc_lock);
        g_sc_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        CloseHandle(CreateThread(NULL, 0, sc_audio_thread, NULL, 0, NULL));
    }
    if (!(c = sc_clip_get(name))) return;
    EnterCriticalSection(&g_sc_lock);
    for (v = 0; v < SC_VOICES; v++)                  /* a loop already playing: just keep it */
        if (loop && g_sc_voice[v].s && g_sc_voice[v].loop && g_sc_voice[v].tag == tag) {
            g_sc_voice[v].keep = GetTickCount() + 300;
            LeaveCriticalSection(&g_sc_lock);
            return;
        }
    for (v = 0; v < SC_VOICES; v++) {                /* a free voice, else the one nearest its end */
        double left;
        if (!g_sc_voice[v].s) { pick = v; least = -1; break; }
        left = (g_sc_voice[v].n - g_sc_voice[v].pos) / g_sc_voice[v].step;
        if (left < least) { least = left; pick = v; }
    }
    g_sc_voice[pick].s = c->s; g_sc_voice[pick].n = c->n; g_sc_voice[pick].pos = 0;
    g_sc_voice[pick].step = (rate > 0 ? rate : 11025) / 8000.0; g_sc_voice[pick].gain = gain;
    g_sc_voice[pick].loop = loop; g_sc_voice[pick].tag = tag; g_sc_voice[pick].keep = GetTickCount() + 300;
    LeaveCriticalSection(&g_sc_lock);
    SetEvent(g_sc_wake);
}

/* keep a looping sound going (called each game tick while the game still plays it) */
static void sc_keep(int tag)
{
    int v;
    if (g_sc_speaker <= 0 || !g_sc_wake) return;
    EnterCriticalSection(&g_sc_lock);
    for (v = 0; v < SC_VOICES; v++)
        if (g_sc_voice[v].s && g_sc_voice[v].loop && g_sc_voice[v].tag == tag) g_sc_voice[v].keep = GetTickCount() + 150;
    LeaveCriticalSection(&g_sc_lock);
}
