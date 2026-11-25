#define _CRT_SECURE_NO_WARNINGS
/* Motor Mash: ReMashed - patch installer.
 *
 * Run it in an installed Motor Mash folder (the one with MotorMsh.exe). It:
 *   1. checks MotorMsh.exe is the original release (SHA-256), keeps it as
 *      MotorMsh.exe.original and patches it (see tools/patch_motormsh.py);
 *   2. installs the port's files (MMCDA.dll, SDL3.dll, dgVoodoo2 DDraw.dll / D3DImm.dll
 *      and dgVoodoo.conf), embedded in this exe;
 *   3. copies the DirectX 5 Retained Mode files (d3drm*.dll, d3dxof.dll) from the player's
 *      Motor Mash CD, which Windows 11 no longer has;
 *   4. rips the CD's music tracks to Music\TrackNN.wav (the port plays them instead of
 *      CD audio).
 * The CD can be a real drive or a .cue/.bin image of the player's own disc.
 * Nothing from the game is contained in this program.
 *
 * Usage: MotorMashPatch.exe [game folder] [/image disc.cue] [/cd X:] [/restore] [/quiet]
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <ntddcdrm.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "payload.h"

#define ORIG_SHA    "04167837e1b992d7549924725e0978c458affcc0ea330857d81ef960b9101551"
#define PATCHED_SHA "28f791878314b7622afe9457964f293203d486df32bc2bb5902b291fdbc78468"

static const struct { DWORD off; const char *orig, *repl; } k_patches[] = {
    /* overlay surface double Release -> NOP the two early Releases */
    { 0x407620 - 0x400C00, "\x8B\x10\x50\xFF\x52\x08", NULL },
    { 0x40764B - 0x400C00, "\xA1\xA0\x4F\x50\x00\x50\x8B\x08\xFF\x51\x08", NULL },
    /* import WINMM.dll -> MMCDA.dll (CD audio emulation + all the port's features) */
    { 0x43E5FE - 0x401A00, "WINMM.dll", "MMCDA.dll" },
    /* window style WS_POPUP|WS_VISIBLE -> captioned window (dgVoodoo windowed mode) */
    { 0x42CEA3 - 0x400C00, "\x68\x00\x00\x00\x90", "\x68\x00\x00\xCA\x10" },
};
static const int k_patch_len[] = { 6, 11, 9, 5 };

static const char *const k_dx_files[] = { "d3drm.dll", "d3drm8f.dll", "d3drm16f.dll", "d3drm24f.dll",
                                          "d3drm32f.dll", "d3dxof.dll" };
#define NDX (sizeof k_dx_files / sizeof k_dx_files[0])

static int g_quiet;
static char g_dir[MAX_PATH];

static void pause_exit(int code)
{
    if (!g_quiet) { printf("\nPress Enter to close."); getchar(); }
    exit(code);
}
static void fail(const char *fmt, const char *a)
{
    printf("\nERROR: "); printf(fmt, a); printf("\n");
    pause_exit(1);
}

/* ---- small file helpers ---- */
static BYTE *read_file(const char *path, DWORD *len)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    BYTE *buf;
    DWORD n = 0;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    *len = GetFileSize(h, NULL);
    buf = (BYTE *)malloc(*len + 1);
    if (!buf || !ReadFile(h, buf, *len, &n, NULL) || n != *len) { free(buf); buf = NULL; }
    CloseHandle(h);
    return buf;
}
static int write_file(const char *path, const void *data, DWORD len)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD n = 0;
    BOOL ok;
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = WriteFile(h, data, len, &n, NULL) && n == len;
    CloseHandle(h);
    return ok;
}
static int exists(const char *path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }
static void join(char *out, const char *a, const char *b) { snprintf(out, MAX_PATH, "%s\\%s", a, b); }

static void sha256_hex(const BYTE *data, DWORD len, char out[65])
{
    BCRYPT_ALG_HANDLE alg;
    BYTE hash[32];
    int i;
    out[0] = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) return;
    if (BCryptHash(alg, NULL, 0, (PUCHAR)data, len, hash, 32) == 0)
        for (i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", hash[i]);
    BCryptCloseAlgorithmProvider(alg, 0);
}

/* ---- the disc: a real drive or a cue/bin image ---- */
#define MAXTRK 99
struct track { int n, audio, file; long long start, end; };   /* sectors within its file */
static struct disc {
    char drive;                     /* real drive letter, or 0 for an image */
    char files[8][MAX_PATH]; int nfiles;
    int data_off, data_file;        /* data track: user data offset in a raw sector */
    long long data_start;
    struct track t[MAXTRK]; int nt;
} g_disc;

static int msf(const char *s)
{
    int m = 0, sec = 0, f = 0;
    sscanf(s, "%d:%d:%d", &m, &sec, &f);
    return (m * 60 + sec) * 75 + f;
}

static long long file_sectors(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return 0;
    return (((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow) / 2352;
}

static int open_cue(const char *cue)
{
    FILE *f = fopen(cue, "r");
    char line[512], dir[MAX_PATH], *p;
    int cur = -1, idx0[MAXTRK];
    if (!f) return 0;
    memset(&g_disc, 0, sizeof g_disc);
    strcpy(dir, cue);
    if ((p = strrchr(dir, '\\')) || (p = strrchr(dir, '/'))) p[1] = 0; else dir[0] = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line, name[MAX_PATH], type[32];
        int n;
        while (*s == ' ' || *s == '\t') s++;
        if (!_strnicmp(s, "FILE", 4) && g_disc.nfiles < 8) {
            char *q1 = strchr(s, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (!q1 || !q2) continue;
            *q2 = 0;
            snprintf(g_disc.files[g_disc.nfiles], MAX_PATH, "%s%s", dir, q1 + 1);
            if (!exists(g_disc.files[g_disc.nfiles])) {          /* common: .img listed, .bin on disk */
                char alt[MAX_PATH], *dot;
                strcpy(alt, g_disc.files[g_disc.nfiles]);
                if ((dot = strrchr(alt, '.'))) { strcpy(dot, ".bin"); if (exists(alt)) strcpy(g_disc.files[g_disc.nfiles], alt); }
            }
            g_disc.nfiles++;
        } else if (sscanf(s, "TRACK %d %31s", &n, type) == 2 && g_disc.nt < MAXTRK) {
            cur = g_disc.nt++;
            g_disc.t[cur].n = n;
            g_disc.t[cur].audio = !_stricmp(type, "AUDIO");
            g_disc.t[cur].file = g_disc.nfiles - 1;
            idx0[cur] = -1;
            if (!g_disc.t[cur].audio && g_disc.data_off == 0) {
                g_disc.data_off = strstr(type, "MODE2") ? 24 : 16;
                g_disc.data_file = g_disc.nfiles - 1;
            }
        } else if (sscanf(s, "INDEX %d %31s", &n, name) == 2 && cur >= 0) {
            if (n == 0) idx0[cur] = msf(name);
            if (n == 1) g_disc.t[cur].start = msf(name);
        }
    }
    fclose(f);
    for (cur = 0; cur < g_disc.nt; cur++) {          /* each track ends where the next begins */
        struct track *t = &g_disc.t[cur];
        if (cur + 1 < g_disc.nt && g_disc.t[cur + 1].file == t->file)
            t->end = idx0[cur + 1] >= 0 ? idx0[cur + 1] : g_disc.t[cur + 1].start;
        else t->end = file_sectors(g_disc.files[t->file]);
        if (!t->audio && t->file == g_disc.data_file && !g_disc.data_start) g_disc.data_start = t->start;
    }
    return g_disc.nfiles > 0 && g_disc.nt > 0;
}

static int open_drive(char letter)
{
    char dev[16];
    HANDLE h;
    CDROM_TOC toc;
    DWORD n;
    int i;
    memset(&g_disc, 0, sizeof g_disc);
    g_disc.drive = letter;
    snprintf(dev, sizeof dev, "\\\\.\\%c:", letter);
    h = CreateFileA(dev, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!DeviceIoControl(h, IOCTL_CDROM_READ_TOC, NULL, 0, &toc, sizeof toc, &n, NULL)) { CloseHandle(h); return 0; }
    CloseHandle(h);
    for (i = 0; i <= toc.LastTrack - toc.FirstTrack && g_disc.nt < MAXTRK; i++) {
        TRACK_DATA *a = &toc.TrackData[i], *b = &toc.TrackData[i + 1];
        struct track *t = &g_disc.t[g_disc.nt++];
        t->n = a->TrackNumber;
        t->audio = !(a->Control & 4);
        t->start = (a->Address[1] * 60 + a->Address[2]) * 75 + a->Address[3] - 150;
        t->end = (b->Address[1] * 60 + b->Address[2]) * 75 + b->Address[3] - 150;
        if (i + 1 <= toc.LastTrack - toc.FirstTrack) t->end -= 150;       /* next track's 2 s pregap */
    }
    return g_disc.nt > 0;
}

/* read raw 2352-byte audio sectors [lba, lba+n) of track t into buf */
static int read_audio(HANDLE *hdrive, FILE **fp, const struct track *t, long long lba, int n, BYTE *buf)
{
    if (g_disc.drive) {
        RAW_READ_INFO ri;
        DWORD got;
        int tries;
        ri.DiskOffset.QuadPart = lba * 2048;
        ri.SectorCount = n;
        ri.TrackMode = CDDA;
        for (tries = 0; tries < 3; tries++)
            if (DeviceIoControl(*hdrive, IOCTL_CDROM_RAW_READ, &ri, sizeof ri, buf, n * 2352, &got, NULL)) return 1;
        return 0;
    }
    if (!fp[t->file]) fp[t->file] = fopen(g_disc.files[t->file], "rb");
    if (!fp[t->file]) return 0;
    _fseeki64(fp[t->file], lba * 2352, SEEK_SET);
    return fread(buf, 2352, n, fp[t->file]) == (size_t)n;
}

/* ISO9660 lookup in the image's data track: "DIRECTX/D3DRM.DLL" -> contents */
static BYTE *iso_read(FILE *f, long long lba, DWORD len)
{
    BYTE *out = (BYTE *)malloc(len + 2048), sec[2352];
    DWORD i;
    if (!out) return NULL;
    for (i = 0; i * 2048 < len; i++) {
        _fseeki64(f, (g_disc.data_start + lba + i) * 2352 + g_disc.data_off, SEEK_SET);
        if (fread(sec, 1, 2048, f) != 2048) { free(out); return NULL; }
        memcpy(out + i * 2048, sec, 2048);
    }
    return out;
}
static BYTE *iso_find(const char *path, DWORD *outlen)
{
    FILE *f = g_disc.nfiles ? fopen(g_disc.files[g_disc.data_file], "rb") : NULL;
    BYTE *pvd, *dir = NULL;
    DWORD lba, len;
    char part[64];
    const char *p = path;
    if (!f) return NULL;
    pvd = iso_read(f, 16, 2048);
    if (!pvd || memcmp(pvd + 1, "CD001", 5)) { free(pvd); fclose(f); return NULL; }
    lba = *(DWORD *)(pvd + 156 + 2); len = *(DWORD *)(pvd + 156 + 10);
    free(pvd);
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t pl = slash ? (size_t)(slash - p) : strlen(p);
        DWORD o = 0;
        int found = 0;
        memcpy(part, p, pl); part[pl] = 0;
        dir = iso_read(f, lba, len);
        while (dir && o < len) {
            BYTE rl = dir[o];
            if (!rl) { o = (o / 2048 + 1) * 2048; continue; }
            {
                BYTE nl = dir[o + 32];
                char name[64];
                memcpy(name, dir + o + 33, nl < 63 ? nl : 63); name[nl < 63 ? nl : 63] = 0;
                if (strchr(name, ';')) *strchr(name, ';') = 0;
                if (!_stricmp(name, part)) { lba = *(DWORD *)(dir + o + 2); len = *(DWORD *)(dir + o + 10); found = 1; break; }
            }
            o += rl;
        }
        free(dir); dir = NULL;
        if (!found) { fclose(f); return NULL; }
        p += pl + (slash ? 1 : 0);
    }
    dir = iso_read(f, lba, len);
    fclose(f);
    *outlen = len;
    return dir;
}

static int disc_has_game(void)
{
    if (g_disc.drive) {
        char a[MAX_PATH], b[MAX_PATH];
        snprintf(a, sizeof a, "%c:\\MOTORMSH.EXE", g_disc.drive);
        snprintf(b, sizeof b, "%c:\\DIRECTX\\D3DRM.DLL", g_disc.drive);
        return exists(a) && exists(b);
    } else {
        DWORD len;
        BYTE *d = iso_find("MOTORMSH.EXE", &len);
        free(d);
        return d != NULL;
    }
}

static int find_disc(const char *image, char want_drive)
{
    char d;
    if (image) {
        if (!open_cue(image)) { printf("  Could not read disc image %s\n", image); return 0; }
        if (!disc_has_game()) { printf("  %s is not a Motor Mash disc image.\n", image); return 0; }
        printf("  Using disc image %s\n", image);
        return 1;
    }
    for (d = 'A'; d <= 'Z'; d++) {
        char root[4] = { d, ':', '\\', 0 };
        if (want_drive && d != want_drive) continue;
        if (GetDriveTypeA(root) != DRIVE_CDROM) continue;
        if (open_drive(d) && disc_has_game()) { printf("  Found the Motor Mash CD in drive %c:\n", d); return 1; }
    }
    return 0;
}

static int obtain_disc(const char *image, char drive)
{
    char line[MAX_PATH];
    if (find_disc(image, drive)) return 1;
    if (g_quiet) return 0;
    for (;;) {
        size_t n;
        printf("\n  The Motor Mash CD is needed (for the DirectX 5 files and the music).\n"
               "  Insert it and press Enter, or type the path of a .cue image of your disc,\n"
               "  or type S to skip: ");
        if (!fgets(line, sizeof line, stdin)) return 0;
        n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
        if (n && line[0] == '"') { memmove(line, line + 1, n); n = strlen(line); if (n && line[n - 1] == '"') line[--n] = 0; }
        if (!_stricmp(line, "s")) return 0;
        if (find_disc(n ? line : NULL, 0)) return 1;
        printf("  Not found.\n");
    }
}

/* ---- steps ---- */
static void patch_exe(void)
{
    char exe[MAX_PATH], bak[MAX_PATH], h[65];
    DWORD len, blen;
    BYTE *data, *bdata;
    int i;
    join(exe, g_dir, "MotorMsh.exe");
    join(bak, g_dir, "MotorMsh.exe.original");
    if (!(data = read_file(exe, &len))) fail("%s not found. Run this in your Motor Mash folder.", exe);
    sha256_hex(data, len, h);
    if (!strcmp(h, PATCHED_SHA)) { printf("  MotorMsh.exe is already patched.\n"); free(data); return; }
    if (strcmp(h, ORIG_SHA)) {
        if ((bdata = read_file(bak, &blen))) {               /* a modified exe, but we have the original */
            sha256_hex(bdata, blen, h);
            if (!strcmp(h, ORIG_SHA)) { free(data); data = bdata; len = blen; printf("  Using MotorMsh.exe.original.\n"); }
            else free(bdata);
        }
        sha256_hex(data, len, h);
        if (strcmp(h, ORIG_SHA))
            fail("MotorMsh.exe is not the original Motor Mash release (SHA-256 %s). "
                 "This patch needs the original Windows 95 MotorMsh.exe from the CD.", h);
    }
    if (!exists(bak)) {
        if (!write_file(bak, data, len)) fail("Could not write %s (is the folder writable?).", bak);
        printf("  Saved the original as MotorMsh.exe.original\n");
    }
    for (i = 0; i < (int)(sizeof k_patches / sizeof k_patches[0]); i++) {
        if (memcmp(data + k_patches[i].off, k_patches[i].orig, k_patch_len[i])) fail("%s", "Unexpected bytes in MotorMsh.exe.");
        if (k_patches[i].repl) memcpy(data + k_patches[i].off, k_patches[i].repl, k_patch_len[i]);
        else memset(data + k_patches[i].off, 0x90, k_patch_len[i]);
    }
    sha256_hex(data, len, h);
    if (strcmp(h, PATCHED_SHA)) fail("%s", "Patch check failed (internal error).");
    if (!write_file(exe, data, len)) fail("Could not write %s. Close the game and try again.", exe);
    printf("  Patched MotorMsh.exe\n");
    free(data);
}

static void install_payload(void)
{
    int i;
    for (i = 0; i < PAYLOAD_COUNT; i++) {
        HRSRC r = FindResourceA(NULL, MAKEINTRESOURCEA(k_payload[i].id), MAKEINTRESOURCEA(10) /* RT_RCDATA */);
        HGLOBAL g = r ? LoadResource(NULL, r) : NULL;
        char path[MAX_PATH];
        if (!g) fail("%s", "Missing embedded file (damaged download?).");
        join(path, g_dir, k_payload[i].name);
        if (k_payload[i].keep && exists(path)) { printf("  Kept your %s\n", k_payload[i].name); continue; }
        if (!write_file(path, LockResource(g), SizeofResource(NULL, r)))
            fail("Could not write %s. Close the game and try again.", path);
        printf("  Installed %s\n", k_payload[i].name);
    }
}

static int dx_present(void)
{
    char p[MAX_PATH];
    unsigned i;
    for (i = 0; i < NDX; i++) { join(p, g_dir, k_dx_files[i]); if (!exists(p)) return 0; }
    return 1;
}

static int copy_dx(void)
{
    unsigned i;
    for (i = 0; i < NDX; i++) {
        char dst[MAX_PATH];
        join(dst, g_dir, k_dx_files[i]);
        if (g_disc.drive) {
            char src[MAX_PATH];
            snprintf(src, sizeof src, "%c:\\DIRECTX\\%s", g_disc.drive, k_dx_files[i]);
            if (!CopyFileA(src, dst, FALSE)) { printf("  Could not copy %s\n", src); return 0; }
        } else {
            char ip[64];
            DWORD len;
            BYTE *d;
            unsigned k;
            snprintf(ip, sizeof ip, "DIRECTX/%s", k_dx_files[i]);
            for (k = 0; ip[k]; k++) ip[k] = (char)toupper((unsigned char)ip[k]);
            if (!(d = iso_find(ip, &len)) || !write_file(dst, d, len)) { free(d); printf("  Could not copy %s\n", ip); return 0; }
            free(d);
        }
        printf("  Copied %s\n", k_dx_files[i]);
    }
    return 1;
}

static int music_present(void)
{
    char p[MAX_PATH];
    join(p, g_dir, "Music\\Track03.wav");
    if (exists(p)) return 1;
    join(p, g_dir, "Music\\Track03.ogg");
    return exists(p);
}

static int rip_music(void)
{
    HANDLE hdrive = INVALID_HANDLE_VALUE;
    FILE *fp[8] = { 0 };
    BYTE *buf = (BYTE *)malloc(26 * 2352);
    char mdir[MAX_PATH];
    int i, ok = 1, ripped = 0;
    if (!buf) return 0;
    join(mdir, g_dir, "Music");
    CreateDirectoryA(mdir, NULL);
    if (g_disc.drive) {
        char dev[16];
        snprintf(dev, sizeof dev, "\\\\.\\%c:", g_disc.drive);
        hdrive = CreateFileA(dev, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (hdrive == INVALID_HANDLE_VALUE) { free(buf); return 0; }
    }
    for (i = 0; i < g_disc.nt && ok; i++) {
        const struct track *t = &g_disc.t[i];
        char path[MAX_PATH], name[32];
        long long lba, total = t->end - t->start;
        DWORD bytes, hdr[11], n;
        HANDLE out;
        if (!t->audio || total <= 0) continue;
        snprintf(name, sizeof name, "Track%02d.wav", t->n);
        join(path, mdir, name);
        out = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (out == INVALID_HANDLE_VALUE) { ok = 0; break; }
        bytes = (DWORD)(total * 2352);
        hdr[0] = 'FFIR'; hdr[1] = 36 + bytes; hdr[2] = 'EVAW'; hdr[3] = ' tmf'; hdr[4] = 16;
        hdr[5] = 0x00020001; hdr[6] = 44100; hdr[7] = 44100 * 4; hdr[8] = 0x00100004; hdr[9] = 'atad'; hdr[10] = bytes;
        WriteFile(out, hdr, sizeof hdr, &n, NULL);
        for (lba = t->start; lba < t->end; ) {
            int cnt = (int)(t->end - lba > 26 ? 26 : t->end - lba);
            if (!read_audio(&hdrive, fp, t, lba, cnt, buf)) { ok = 0; break; }
            WriteFile(out, buf, cnt * 2352, &n, NULL);
            lba += cnt;
            printf("\r  Ripping %s  %3d%%", name, (int)((lba - t->start) * 100 / total));
        }
        CloseHandle(out);
        if (!ok) { DeleteFileA(path); printf("\n  Reading track %d failed.\n", t->n); break; }
        printf("\r  Ripped %s (%d:%02d)        \n", name, (int)(total / 75 / 60), (int)(total / 75 % 60));
        ripped++;
    }
    for (i = 0; i < 8; i++) if (fp[i]) fclose(fp[i]);
    if (hdrive != INVALID_HANDLE_VALUE) CloseHandle(hdrive);
    free(buf);
    return ok && ripped > 0;
}

static void restore(void)
{
    char exe[MAX_PATH], bak[MAX_PATH], h[65];
    DWORD len;
    BYTE *d;
    int i;
    join(exe, g_dir, "MotorMsh.exe");
    join(bak, g_dir, "MotorMsh.exe.original");
    if (!(d = read_file(bak, &len))) fail("%s not found, nothing to restore.", bak);
    sha256_hex(d, len, h);
    if (strcmp(h, ORIG_SHA)) fail("%s is not the original exe; not restoring.", bak);
    if (!write_file(exe, d, len)) fail("Could not write %s. Close the game and try again.", exe);
    free(d);
    for (i = 0; i < PAYLOAD_COUNT; i++) {                   /* dgVoodoo's DDraw.dll would hijack the original */
        char p[MAX_PATH];
        join(p, g_dir, k_payload[i].name);
        if (!k_payload[i].keep) DeleteFileA(p);
    }
    printf("  Restored the original MotorMsh.exe and removed the patch's DLLs.\n"
           "  (The DirectX 5 files and Music folder were left in place.)\n");
}

/* ---- install from the disc (no game installed yet) ----
 * The original installer copies these CD folders and files unchanged (checked against an
 * installed copy); the rest of the CD is the DOS version, drivers and setup. */
static const char *const k_install[][2] = {
    { "AMAZON", "Amazon" }, { "ARCTIC", "Arctic" }, { "AUDIO", "Audio" }, { "CITY", "City" },
    { "MAIN", "Main" }, { "MISC", "Misc" }, { "NATLANTI", "Natlanti" }, { "NIGHTMAR", "Nightmar" },
    { "TRACK", "Track" }, { "WEST", "West" },
    { "MOTORMSH.EXE", "MotorMsh.exe" }, { "CLOCK.INI", "Clock.ini" }, { "SET.INI", "Set.ini" },
};
static long long g_copied;

static int copy_tree(const char *src, const char *dst)       /* real drive */
{
    WIN32_FIND_DATAA fd;
    char pat[MAX_PATH], s[MAX_PATH], d[MAX_PATH];
    HANDLE h;
    int ok = 1;
    if (!(GetFileAttributesA(src) & FILE_ATTRIBUTE_DIRECTORY)) {
        if (!CopyFileA(src, dst, FALSE)) return 0;
        SetFileAttributesA(dst, FILE_ATTRIBUTE_NORMAL);      /* CD files are read-only */
        g_copied++;
        return 1;
    }
    CreateDirectoryA(dst, NULL);
    snprintf(pat, sizeof pat, "%s\\*", src);
    if ((h = FindFirstFileA(pat, &fd)) == INVALID_HANDLE_VALUE) return 1;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        join(s, src, fd.cFileName); join(d, dst, fd.cFileName);
        ok &= copy_tree(s, d);
    } while (ok && FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
}

/* image: copy the ISO9660 entry (file or directory) at lba/len/isdir to dst */
static int iso_copy(FILE *f, DWORD lba, DWORD len, int isdir, const char *dst)
{
    BYTE *d = iso_read(f, lba, len);
    DWORD o = 0;
    int ok = 1;
    if (!d) return 0;
    if (!isdir) {
        ok = write_file(dst, d, len);
        free(d);
        g_copied++;
        return ok;
    }
    CreateDirectoryA(dst, NULL);
    while (ok && o < len) {
        BYTE rl = d[o], nl;
        char name[64], path[MAX_PATH];
        if (!rl) { o = (o / 2048 + 1) * 2048; continue; }
        nl = d[o + 32];
        if (!(nl == 1 && (d[o + 33] == 0 || d[o + 33] == 1))) {
            memcpy(name, d + o + 33, nl < 63 ? nl : 63); name[nl < 63 ? nl : 63] = 0;
            if (strchr(name, ';')) *strchr(name, ';') = 0;
            join(path, dst, name);
            ok = iso_copy(f, *(DWORD *)(d + o + 2), *(DWORD *)(d + o + 10), d[o + 25] & 2, path);
        }
        o += rl;
    }
    free(d);
    return ok;
}
static int iso_locate(const char *name, DWORD *lba, DWORD *len, int *isdir)
{
    FILE *f = fopen(g_disc.files[g_disc.data_file], "rb");
    BYTE *pvd, *d;
    DWORD o = 0, rl_, rlen;
    int found = 0;
    if (!f) return 0;
    pvd = iso_read(f, 16, 2048);
    if (!pvd) { fclose(f); return 0; }
    rl_ = *(DWORD *)(pvd + 156 + 2); rlen = *(DWORD *)(pvd + 156 + 10);
    free(pvd);
    d = iso_read(f, rl_, rlen);
    fclose(f);
    while (d && o < rlen && !found) {
        BYTE rl = d[o], nl;
        char nm[64];
        if (!rl) { o = (o / 2048 + 1) * 2048; continue; }
        nl = d[o + 32];
        memcpy(nm, d + o + 33, nl < 63 ? nl : 63); nm[nl < 63 ? nl : 63] = 0;
        if (strchr(nm, ';')) *strchr(nm, ';') = 0;
        if (!_stricmp(nm, name)) { *lba = *(DWORD *)(d + o + 2); *len = *(DWORD *)(d + o + 10); *isdir = d[o + 25] & 2; found = 1; }
        o += rl;
    }
    free(d);
    return found;
}

static int install_game(const char *dest)
{
    unsigned i;
    char cmd[MAX_PATH * 2];
    snprintf(cmd, sizeof cmd, "%s", dest);
    SHCreateDirectoryExA(NULL, cmd, NULL);
    if (!(GetFileAttributesA(dest) & FILE_ATTRIBUTE_DIRECTORY) || !exists(dest)) {
        printf("  Could not create %s\n", dest);
        return 0;
    }
    for (i = 0; i < sizeof k_install / sizeof k_install[0]; i++) {
        char dst[MAX_PATH];
        int ok;
        join(dst, dest, k_install[i][1]);
        printf("  Copying %-14s", k_install[i][1]);
        if (g_disc.drive) {
            char src[MAX_PATH];
            snprintf(src, sizeof src, "%c:\\%s", g_disc.drive, k_install[i][0]);
            ok = copy_tree(src, dst);
        } else {
            DWORD lba, len;
            int isdir;
            FILE *f = fopen(g_disc.files[g_disc.data_file], "rb");
            ok = f && iso_locate(k_install[i][0], &lba, &len, &isdir) && iso_copy(f, lba, len, isdir, dst);
            if (f) fclose(f);
        }
        if (!ok) { printf("failed\n"); return 0; }
        printf("ok (%lld files so far)\n", g_copied);
    }
    return 1;
}

static void desktop_shortcut(void)
{
    IShellLinkA *sl;
    IPersistFile *pf;
    char exe[MAX_PATH], lnk[MAX_PATH];
    WCHAR wlnk[MAX_PATH];
    CoInitialize(NULL);
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkA, (void **)&sl))) return;
    join(exe, g_dir, "MotorMsh.exe");
    sl->lpVtbl->SetPath(sl, exe);
    sl->lpVtbl->SetWorkingDirectory(sl, g_dir);
    sl->lpVtbl->SetDescription(sl, "Motor Mash");
    if (SUCCEEDED(sl->lpVtbl->QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
        if (SHGetSpecialFolderPathA(NULL, lnk, CSIDL_DESKTOPDIRECTORY, FALSE)) {
            strncat(lnk, "\\Motor Mash.lnk", sizeof lnk - strlen(lnk) - 1);
            MultiByteToWideChar(CP_ACP, 0, lnk, -1, wlnk, MAX_PATH);
            if (SUCCEEDED(pf->lpVtbl->Save(pf, wlnk, TRUE))) printf("  Created a desktop shortcut.\n");
        }
        pf->lpVtbl->Release(pf);
    }
    sl->lpVtbl->Release(sl);
}

static void read_line(char *buf, size_t n)
{
    size_t k;
    if (!fgets(buf, (int)n, stdin)) { buf[0] = 0; return; }
    k = strlen(buf);
    while (k && (buf[k - 1] == '\n' || buf[k - 1] == '\r' || buf[k - 1] == ' ')) buf[--k] = 0;
    if (k && buf[0] == '"') { memmove(buf, buf + 1, k); k = strlen(buf); if (k && buf[k - 1] == '"') buf[--k] = 0; }
}

int main(int argc, char **argv)
{
    const char *image = NULL, *install_to = NULL;
    char drive = 0, exe[MAX_PATH];
    int i, want_restore = 0, ok_dx, ok_music, have_disc = 0, installed = 0;
    printf("Motor Mash: ReMashed - installer and patch\n"
           "==========================================\n"
           "You need your own Motor Mash CD (or an image of it).\n\n");
    g_dir[0] = 0;
    for (i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "/image") && i + 1 < argc) image = argv[++i];
        else if (!_stricmp(argv[i], "/cd") && i + 1 < argc) drive = (char)toupper((unsigned char)argv[++i][0]);
        else if (!_stricmp(argv[i], "/install") && i + 1 < argc) install_to = argv[++i];
        else if (!_stricmp(argv[i], "/restore")) want_restore = 1;
        else if (!_stricmp(argv[i], "/quiet")) g_quiet = 1;
        else strncpy(g_dir, argv[i], MAX_PATH - 1);
    }
    if (!g_dir[0]) {                                         /* default: the folder this exe is in */
        char *s;
        GetModuleFileNameA(NULL, g_dir, MAX_PATH);
        if ((s = strrchr(g_dir, '\\'))) *s = 0;
    }
    if (install_to) strncpy(g_dir, install_to, MAX_PATH - 1);
    join(exe, g_dir, "MotorMsh.exe");

    /* not installed here: install from the disc, or point at an existing install */
    if (!exists(exe) && !want_restore) {
        char line[MAX_PATH];
        printf("Motor Mash is not installed in %s.\n", g_dir);
        if (!install_to && !g_quiet) {
            printf("  I  = install it from the Motor Mash CD (or a disc image)\n"
                   "  F  = it is already installed: choose its folder\n"
                   "Choice [I]: ");
            read_line(line, sizeof line);
        } else line[0] = 'I', line[1] = 0;
        if (!line[0] || toupper((unsigned char)line[0]) == 'I') {
            printf("\nLooking for the Motor Mash CD...\n");
            if (!obtain_disc(image, drive)) fail("%s", "No Motor Mash disc found; nothing installed.");
            have_disc = 1;
            if (!install_to && !g_quiet) {
                char def[MAX_PATH];
                snprintf(def, sizeof def, "C:\\Games\\Motor Mash");
                printf("\nInstall to %s ?\nPress Enter, or type another folder: ", def);
                read_line(line, sizeof line);
                strncpy(g_dir, line[0] ? line : def, MAX_PATH - 1);
            }
            printf("\n[0/4] Installing Motor Mash to %s\n", g_dir);
            if (!install_game(g_dir)) fail("%s", "Copying the game from the disc failed.");
            installed = 1;
            join(exe, g_dir, "MotorMsh.exe");
        }
    }
    if (!exists(exe) && !g_quiet) {
        BROWSEINFOA bi;
        PIDLIST_ABSOLUTE pidl;
        printf("MotorMsh.exe is not in this folder. Choose your Motor Mash folder...\n");
        CoInitialize(NULL);
        memset(&bi, 0, sizeof bi);
        bi.lpszTitle = "Choose the folder Motor Mash is installed in (it contains MotorMsh.exe)";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        if ((pidl = SHBrowseForFolderA(&bi)) != NULL) { SHGetPathFromIDListA(pidl, g_dir); CoTaskMemFree(pidl); }
        join(exe, g_dir, "MotorMsh.exe");
    }
    if (!exists(exe)) fail("MotorMsh.exe not found in %s.", g_dir);
    printf("Game folder: %s\n\n", g_dir);

    if (want_restore) { restore(); pause_exit(0); }

    printf("[1/4] Patching the game\n");
    patch_exe();
    printf("\n[2/4] Installing the port's files\n");
    install_payload();

    ok_dx = dx_present();
    ok_music = music_present();
    if (ok_dx) printf("\n[3/4] DirectX 5 files: already present\n");
    if (ok_music) printf("\n[4/4] Music: already present\n");
    if (!ok_dx || !ok_music) {
        if (!have_disc) printf("\nLooking for the Motor Mash CD...\n");
        if (have_disc || obtain_disc(image, drive)) {
            if (!ok_dx) { printf("\n[3/4] Copying the DirectX 5 files from the CD\n"); ok_dx = copy_dx(); }
            if (!ok_music) { printf("\n[4/4] Ripping the music from the CD (takes a few minutes)\n"); ok_music = rip_music(); }
        }
    }
    printf("\n");
    if (!ok_dx) printf("WARNING: the DirectX 5 files (d3drm.dll etc.) are missing; the game will not start\n"
                       "         until you run this again with the Motor Mash CD.\n");
    if (!ok_music) printf("Note: no music was installed; run this again with the CD to add it.\n");
    if (ok_dx && !g_quiet) {
        char line[16];
        printf("Create a desktop shortcut? [Y/n]: ");
        read_line(line, sizeof line);
        if (toupper((unsigned char)line[0]) != 'N') desktop_shortcut();
        printf("\n");
    }
    if (ok_dx && installed) printf("Motor Mash is installed in %s and no longer needs the CD.\n", g_dir);
    if (ok_dx) printf("Done. Start the game with MotorMsh.exe.\n"
                      "Settings: in the menu, the Calibrate option opens Controller Setup (controller,\n"
                      "game speed, tracks...). To undo the patch run: MotorMashPatch.exe /restore\n");
    pause_exit(ok_dx ? 0 : 2);
    return 0;
}
