/* tracks.h - custom tracks in slots 9-16 of each world (see TRACKS.md).
 * Included by mmcda.c (uses its helpers: log_msg, G_I32, make_trampoline, patch_jmp, g_dir, g_home).
 *
 * The game has 16 map slots per world (WEST1-9, WESTA-G ...) but only ships and offers 1-8.
 * A map file dropped into Track\ under a slot 9-16 name (e.g. AMAZON9.MAP, AMAZONA.MAP) now
 * shows on the track select page (the parrot) after that world's own tracks:
 *
 * 1. The track select page (handler 0x40a620, page 7) is replaced by tracksel_page below, a
 *    copy of it that lists up to 16 tracks: slots 1-8 when unlocked, as before, and slots
 *    9-16 when their map file is present.
 * 2. Its track carousel shows the numbered balls JUKE01-08 (front-end 3D objects 0x2a-0x31).
 *    Balls 9-16 are added as objects 0x39-0x40: eight more records in the front-end object
 *    list (0x445ae8, read by 0x4049b3), whose models Misc\JUKE09.x-16.x are made at start-up
 *    from the player's JUKE01.x with its texture name changed, and whose textures are ball
 *    1's with the number repainted (archive texture lookup 0x42b1f0). The
 *    per-object state table (0x5050f8, 57 x 0x24, followed by other globals) is moved to a
 *    65-entry copy (fe_relocs.h, made by tools/sixcars/gen_relocs.py --fe).
 * 3. Names: slots 9-16 point at an empty name (0x451008); a present map gets the first line
 *    of Track\<name>.TXT, or "Custom <n>".
 */
#include "fe_relocs.h"

#define TS_FE_BALL9   0x39               /* first added object: balls 9-16 = 0x39-0x40 */
#define TS_OBJ_RECS   427                /* records in the game's object list */
#define TS_OBJ_REC    0x48
#define TS_JUKE01_REC 412                /* JUKE01's record */
#define TS_SEL        (*(volatile int *)(0x0043F388 + G_I32(0x0043F36C) * 4))

static BYTE g_fe_rnew[0x24 * 65 + 16];
static BYTE g_ts_objtab[(TS_OBJ_RECS + 8) * TS_OBJ_REC];
static const char *const g_ts_ballname[8] = { "JUKE09", "JUKE10", "JUKE11", "JUKE12", "JUKE13", "JUKE14", "JUKE15", "JUKE16" };
static char g_ts_custom[96];             /* slot 9-16 map present */
static char g_ts_names[96][40];
static int g_ts_list[16];
static int g_ts_on;

static const char *ts_mapname(int t) { return ((const char *const *)0x0043F040)[t]; }

/* which slot 9-16 maps exist; give them names */
static void ts_scan(void)
{
    int t;
    for (t = 0; t < 96; t++) {
        char path[MAX_PATH], line[64];
        FILE *f;
        if ((t & 15) < 8) continue;
        snprintf(path, sizeof path, "%sTrack\\%s.MAP", g_dir, ts_mapname(t));
        g_ts_custom[t] = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
        if (!g_ts_custom[t]) continue;
        snprintf(g_ts_names[t], sizeof g_ts_names[t], "Custom %c", "9ABCDEFG"[(t & 15) - 8]);
        snprintf(path, sizeof path, "%sTrack\\%s.TXT", g_dir, ts_mapname(t));
        if ((f = fopen(path, "r")) != NULL) {
            int first = 1;
            while (fgets(line, sizeof line, f)) {
                char *v = line, *eq;
                line[strcspn(line, "\r\n#")] = 0;
                while (*v == ' ' || *v == '\t') v++;
                if (!*v) continue;
                eq = strchr(v, '=');
                if (eq && _strnicmp(v, "name", 4) == 0) {
                    for (v = eq + 1; *v == ' ' || *v == '\t'; v++) {}
                    if (*v) snprintf(g_ts_names[t], sizeof g_ts_names[t], "%.30s", v);
                } else if (!eq && first)                     /* older form: the first line is the name */
                    snprintf(g_ts_names[t], sizeof g_ts_names[t], "%.30s", v);
                first = 0;
            }
            fclose(f);
        }
        ((const char **)0x00451008)[t] = g_ts_names[t];
        {   /* lap record: byte 0 = holder (0-11), 12 = no record, 13 = locked; a slot never raced
               on is all zeros, which would read as character 0 holding a 0:00:00 record */
            BYTE *rec = (BYTE *)(0x00463A30 + t * 0x34);
            int k, used = 0;
            for (k = 0; k < 0x34; k++) used |= rec[k];
            if (!used) rec[0] = 0x0C;
        }
    }
}

static int ts_avail(int t)
{
    if ((t & 15) >= 8) return g_ts_custom[t];
    return g_unlock_all || *(BYTE *)(0x00463A30 + t * 0x34) < 0x0D;   /* car.h: [race] tracks=all */
}
static int ts_obj(int t) { return (t & 15) < 8 ? 0x2A + (t & 15) : TS_FE_BALL9 + (t & 15) - 8; }
static int ts_slot(int obj) { return obj >= TS_FE_BALL9 ? 8 + obj - TS_FE_BALL9 : obj - 0x2A; }

#define TS_SND(s, v) ((void (__cdecl *)(int, int, int, int))0x004130D0)(0, G_I32(s), v, 0x7F)

/* 0x40a620 with 16 slots (decompiled; the changes are ts_avail, ts_obj/ts_slot and the list) */
static void __cdecl tracksel_page(void)
{
    static int all[16];
    int key, n, t, w, v, track;
    if (G_I32(0x005050D8) == 0) {
        int sel;
        ts_scan();
        TS_SND(0x005076E8, 2000);
        G_I32(0x0050592C) = TS_SEL;
        ((void (__cdecl *)(int, int *, int, int, int))0x00411A50)(0, (int *)0x00452748, 6, TS_SEL >> 4, (int)0xFFFFFED4);
        sel = TS_SEL;
        for (t = 0; t < 6; t++) ((int *)0x00504FF0)[t] = -1;
        w = sel & 0xF0;
        for (n = 0, t = w; t < w + 16; t++)
            if (ts_avail(t)) {
                if ((sel & 15) == t - w) ((int *)0x00504FF0)[w >> 4] = n;
                n++;
            }
        G_I32(0x005072F0) = n;
        G_I32(0x00505E24) = 0x3F800000;
        G_I32(0x005050E8) = 1;
        G_I32(0x0050700C) = 0x3F333333;
        G_I32(0x00505070) = 0;
        G_I32(0x00505E38) = 0;
        ((void (__cdecl *)(int, int))0x0042BC90)(10, 1);
        G_I32(0x005050D8) = 1;
    }
    if (G_I32(0x00505E38)) {
        if (((int (__cdecl *)(volatile int *, volatile int *))0x00410050)((int *)0x00505E38, (int *)0x00505088))
            TS_SEL = G_I32(0x0050592C);
        goto tail;
    }
    key = G_I32(0x00505930);
    if (key == 0x200) {
        TS_SND(0x005076EC, 3000);
        G_I32(0x00505E38) = 1;
        G_I32(0x00505088) = 0;
        goto tail;
    }
    if (G_I32(0x005050E8) == 0) {                      /* the track carousel has the focus */
        if (key == 0x40) {
            v = G_I32(0x0050507C) - 1;
            G_I32(0x0050507C) = v < 0 ? G_I32(0x005072F0) - 1 : v;
        } else if (key == 0x80) {
            v = G_I32(0x0050507C) + 1;
            G_I32(0x0050507C) = v == G_I32(0x005072F0) ? 0 : v;
        } else if (key == 1 || key == 0x1000) {
            TS_SND(0x005076E4, 3000);
            G_I32(0x005072F4) = 2;
            ((void (__cdecl *)(void))0x00410300)();
        } else if (key == 0x8000) {
            TS_SND(0x005076F4, 2000);
            G_I32(0x005050E8) = 1;
            G_I32(0x00505070) = 4;
            ((int *)0x00504FF0)[TS_SEL >> 4] = G_I32(0x0050507C);
        }
        goto tail;
    }
    if (key == 0x40) {                                   /* the world carousel has the focus */
        TS_SEL -= 0x10;
        if (TS_SEL < 0) TS_SEL = 0x50;
    } else if (key == 0x80) {
        TS_SEL += 0x10;
        if (TS_SEL > 0x5F) TS_SEL = 0;
    } else if (key == 1 || key == 0x1000) {
        TS_SND(0x005076E4, 3000);
        ((void (__cdecl *)(void))0x00410300)();
        G_I32(0x005072F4) = 2;
    } else if (key == 0x2000) {
        TS_SND(0x005076F4, 3000);
        G_I32(0x005050E8) = 0;
        G_I32(0x00505070) = 4;
    }
    for (t = 0; t < 16; t++) all[t] = ts_obj(t);
    ((void (__cdecl *)(int *, int))0x00411F40)(all, 16);  /* hide every ball */
    w = TS_SEL & 0xF0;
    for (n = 0, t = w; t < w + 16; t++)
        if (ts_avail(t)) g_ts_list[n++] = ts_obj(t);
    if (n == 0) g_ts_list[n++] = ts_obj(w);
    G_I32(0x005072F0) = n;
    v = ((int *)0x00504FF0)[w >> 4];
    G_I32(0x0050507C) = v != -1 && v < n ? v : 0;
    ((void (__cdecl *)(int, int *, int, int, int))0x00411A50)(1, g_ts_list, n, G_I32(0x0050507C), 300);
tail:
    {
        int ontrack = G_I32(0x005050E8) == 0, a = G_I32(0x00505070);
        if (ontrack) {
            G_I32(0x00505E24) = ((int *)0x004526EC)[a];
            G_I32(0x0050700C) = ((int *)0x004526E8)[a];
            ((void (__cdecl *)(int, int))0x00411B30)(1, G_I32(0x0050700C));
            v = G_I32(0x00505E24);
        } else {
            G_I32(0x00505E24) = ((int *)0x004526E8)[a];
            G_I32(0x0050700C) = ((int *)0x004526EC)[a];
            ((void (__cdecl *)(int, int))0x00411B30)(0, G_I32(0x00505E24));
            v = G_I32(0x0050700C);
        }
        G_I32(0x00505930) = 0;
        ((void (__cdecl *)(int, int))0x00411B30)(!ontrack, v);
        if (a != 0) G_I32(0x00505070) = a - 2;
    }
    ((void (__cdecl *)(void))0x00410C80)();
    if (G_I32(0x00505E38) == 0) {
        n = G_I32(0x005072F0);
        v = G_I32(0x0050507C);
        track = ts_slot(g_ts_list[v >= 0 && v < n ? v : 0]);
        TS_SEL = track + (TS_SEL & 0xF0);
        ((void (__cdecl *)(int))0x00410CB0)(1);
        G_I32(0x00505038) = 0x004522E8;
        ((void (__cdecl *)(const void *, int))0x00411620)((const void *)0x00451188, TS_SEL >> 4);
        G_I32(0x00505038) = 0x00452368;
        ((void (__cdecl *)(const void *, int))0x00411620)((const void *)0x00451008, TS_SEL);
        ((void (__cdecl *)(int))0x00411300)(G_I32(0x005050E8));
        return;
    }
    ((void (__cdecl *)(int))0x00410150)(G_I32(0x00505088));
}

/* ---- ball textures 9-16: ball 1's texture with the number repainted ----
 * Front-end textures come from the archive lookup 0x42b1f0 (pak_lookup_hook in mmcda.c),
 * which decodes the found image to 8-bit pixels at [0x54aef0], size in 0x6302e8/0x6302ec,
 * palette at 0x54df04 + texture slot * 0x454. */
static int g_ts_paint, g_ts_dump, g_ts_dump_num, g_ts_ball_num;

/* 0x42b540 gives a texture a transparent colour when its name is in the list at 0x61bb58
   (12-byte entries, count 0x61b9f8) - JUKE0001-0008 are, the new balls are not. While a new
   ball is built, JUKE0001's entry carries its name. */
static char *ts_texcb_before(const char *base)
{
    int i, n = G_I32(0x0061B9F8);
    if (!g_ts_on || _strnicmp(base, "JUKE00", 6) != 0 || !(base[6] == '0' ? base[7] == '9' : base[6] == '1' && base[7] >= '0' && base[7] <= '6'))
        return NULL;
    for (i = 0; i < n && i < 512; i++) {
        char *e = (char *)0x0061BB58 + 12 * i;
        if (memcmp(e, "JUKE0001", 8) == 0) {
            memcpy(e, "JUKE00", 6); e[6] = (char)toupper((BYTE)base[6]); e[7] = (char)toupper((BYTE)base[7]);
            return e;
        }
    }
    return NULL;
}
static void ts_texcb_after(char *keyed) { if (keyed) memcpy(keyed, "JUKE0001", 8); }

static void ts_paint_number(BYTE *pix, int w, int h, int num);
static void ts_cache_ball(int num, const BYTE *pix, const BYTE *pal);

static void ts_pak_before(char *base)
{
    if (strncmp(base, "JUKE00", 6) == 0 && base[6] >= '0' && base[6] <= '9' && base[7] >= '0' && base[7] <= '9') {
        int num = (base[6] - '0') * 10 + (base[7] - '0');
        if (GetEnvironmentVariableA("MMCDA_JUKEDUMP", NULL, 0)) g_ts_dump = num;
        if (num >= 1 && num <= 8) g_ts_ball_num = num;
        if (num >= 9 && num <= 16 && g_ts_on) {
            base[6] = '0'; base[7] = '1';
            g_ts_paint = num;
        }
    }
}

static void ts_pak_after(void *found)
{
    BYTE *pix = *(BYTE **)0x0054AEF0;
    int w = G_I16(0x006302E8), h = G_I16(0x006302EC);
    g_ts_dump_num = g_ts_dump;
    g_ts_dump = 0;
    if (found && g_ts_ball_num && pix && w == 64 && h == 64)
        ts_cache_ball(g_ts_ball_num, pix, (const BYTE *)0x0054DF04 + G_I32(0x0054A0B4) * 0x454);
    g_ts_ball_num = 0;
    if (found && g_ts_paint && pix) ts_paint_number(pix, w, h, g_ts_paint);
    g_ts_paint = 0;
    if (found && g_ts_dump_num && pix && w > 0 && h > 0 && w <= 256 && h <= 256) {
        /* debug: save the decoded image as a BMP (palette as the texture builder reads it) */
        char path[MAX_PATH];
        FILE *f;
        const BYTE *pal = (const BYTE *)0x0054DF04 + G_I32(0x0054A0B4) * 0x454;
        snprintf(path, sizeof path, "%sjuke%02d.bmp", g_home, g_ts_dump_num);
        if ((f = fopen(path, "wb")) != NULL) {
            DWORD hdr[13] = { 0 };
            int y, i;
            BYTE b[4];
            fwrite("BM", 1, 2, f);
            hdr[0] = 54 + 1024 + w * h; hdr[2] = 54 + 1024; hdr[3] = 40; hdr[4] = w; hdr[5] = h;
            hdr[6] = 1 | (8 << 16); hdr[8] = w * h; hdr[11] = 256;
            fwrite(hdr, 4, 13, f);
            for (i = 0; i < 256; i++) { b[0] = pal[i * 4 + 2]; b[1] = pal[i * 4 + 1]; b[2] = pal[i * 4]; b[3] = 0; fwrite(b, 1, 4, f); }
            for (y = h - 1; y >= 0; y--) fwrite(pix + y * w, 1, w, f);
            fclose(f);
        }
        log_msg("tracks: saved %s (%dx%d, depth %d)", path, w, h, G_I16(0x006302F6));
    }
    g_ts_dump_num = 0;
}

/* Balls 1-8 as RGB, kept as they load (they load before 9-16), to rebuild the plain ball. */
static BYTE g_ts_ball[8][64 * 64 * 3];
static int g_ts_have;                    /* bit k: ball k+1 cached */

static void ts_cache_ball(int num, const BYTE *pix, const BYTE *pal)
{
    int i;
    for (i = 0; i < 64 * 64; i++) {
        const BYTE *c = pal + pix[i] * 4;
        g_ts_ball[num - 1][i * 3] = c[0]; g_ts_ball[num - 1][i * 3 + 1] = c[1]; g_ts_ball[num - 1][i * 3 + 2] = c[2];
    }
    g_ts_have |= 1 << (num - 1);
}

/* A stroke font for the ball labels 9, A-G (the slot names: AMAZON9, AMAZONA .. AMAZONG):
   x 0-10, y 0-16 (y down); -1 lifts the pen; 99 ends. */
static const float k_ts_glyph[8][40] = {
    { 2,15, 5,16, 8,14, 9,10, 9,5, 8,1.5f, 5,0, 2,1.5f, 1,4.5f, 2,7.5f, 5,8.5f, 8,7.5f, 9,6, 99 },
    { 0.5f,16, 5,0, 9.5f,16, -1,-1, 2.3f,10.5f, 7.7f,10.5f, 99 },
    { 1.5f,16, 1.5f,0, 6,0, 8.5f,1.5f, 8.5f,6, 6,7.7f, 1.5f,7.7f, -1,-1, 6,7.7f, 9,9.5f, 9,14, 6.5f,16, 1.5f,16, 99 },
    { 9,2, 6.5f,0, 3.5f,0, 1,3, 0.5f,8, 1,13, 3.5f,16, 6.5f,16, 9,14, 99 },
    { 1.5f,0, 1.5f,16, 5.5f,16, 8.5f,13.5f, 9.5f,8, 8.5f,2.5f, 5.5f,0, 1.5f,0, 99 },
    { 9,0, 1.5f,0, 1.5f,16, 9,16, -1,-1, 1.5f,8, 7,8, 99 },
    { 9,0, 1.5f,0, 1.5f,16, -1,-1, 1.5f,8, 7,8, 99 },
    { 9,2, 6.5f,0, 3.5f,0, 1,3, 0.5f,8, 1,13, 3.5f,16, 6.5f,16, 9,14, 9,9, 5.5f,9, 99 },
};

static float ts_segdist2(float px, float py, float ax, float ay, float bx, float by)
{
    float dx = bx - ax, dy = by - ay, t = dx * dx + dy * dy;
    t = t > 0 ? ((px - ax) * dx + (py - ay) * dy) / t : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    dx = ax + t * dx - px; dy = ay + t * dy - py;
    return dx * dx + dy * dy;
}

/* coverage (0-1, 4x4 samples) of glyph g drawn with a round pen of radius pen, offset ox,oy */
static void ts_glyph(float *cov, int g, float pen, float ox, float oy)
{
    const float *p = k_ts_glyph[g];
    const float x0 = 21 + ox, y0 = 15 + oy, bw = 21, bh = 34;
    int x, y, sx, sy, k;
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++) {
            int hit = 0;
            for (sy = 0; sy < 4; sy++)
                for (sx = 0; sx < 4; sx++) {
                    float px = x + (sx + 0.5f) / 4, py = y + (sy + 0.5f) / 4;
                    for (k = 0; p[k + 2] < 98; k += 2) {
                        if (p[k] < 0 || p[k + 2] < 0) continue;
                        if (ts_segdist2(px, py, x0 + p[k] * bw / 10, y0 + p[k + 1] * bh / 16,
                                        x0 + p[k + 2] * bw / 10, y0 + p[k + 3] * bh / 16) <= pen * pen) { hit++; break; }
                    }
                }
            cov[y * 64 + x] = hit / 16.0f;
        }
}

static int ts_cmp_byte(const void *a, const void *b) { return *(const BYTE *)a - *(const BYTE *)b; }

static void ts_hue(BYTE *c, float turn)                 /* rotate an RGB colour's hue (HLS) */
{
    float r = c[0] / 255.0f, g = c[1] / 255.0f, b = c[2] / 255.0f, mx, mn, l, s, h, q, pp, t[3];
    int i;
    mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    l = (mx + mn) / 2;
    if (mx == mn) return;
    s = l <= 0.5f ? (mx - mn) / (mx + mn) : (mx - mn) / (2 - mx - mn);
    h = mx == r ? (g - b) / (mx - mn) : mx == g ? 2 + (b - r) / (mx - mn) : 4 + (r - g) / (mx - mn);
    h = h / 6 + turn;
    h -= (float)floor(h);
    q = l <= 0.5f ? l * (1 + s) : l + s - l * s;
    pp = 2 * l - q;
    t[0] = h + 1 / 3.0f; t[1] = h; t[2] = h - 1 / 3.0f;
    for (i = 0; i < 3; i++) {
        float v, x = t[i] - (float)floor(t[i]);
        v = x < 1 / 6.0f ? pp + (q - pp) * 6 * x : x < 0.5f ? q : x < 2 / 3.0f ? pp + (q - pp) * (2 / 3.0f - x) * 6 : pp;
        c[i] = (BYTE)(v * 255 + 0.5f);
    }
}

/* Ball 9-16 = ball 1 without its 1 (filled in from around it), with the slot's label in the
   balls' style (pale outline, darker body, drop shadow) and its palette turned green. */
static void ts_paint_number(BYTE *pix, int w, int h, int num)
{
    static float body[64 * 64], inner[64 * 64], shad[64 * 64], plain[64 * 64 * 3];
    static BYTE bg[64 * 64 * 3], grow[64 * 64], mask[8][64 * 64];
    static const float light[3] = { 205, 160, 215 };
    static const int off[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    BYTE *pal = (BYTE *)0x0054DF04 + G_I32(0x0054A0B4) * 0x454;
    int i, c, k, x, y, it, clear;
    if (w != 64 || h != 64 || g_ts_have != 0xFF) { log_msg("tracks: ball %d left as ball 1 (balls cached %02x)", num, g_ts_have); return; }
    clear = pix[0];                                      /* the corner: transparent background */
    /* where each ball's number is: away from the median of all eight, refined four times */
    for (i = 0; i < 64 * 64 * 3; i++) {
        BYTE v[8];
        for (k = 0; k < 8; k++) v[k] = g_ts_ball[k][i];
        qsort(v, 8, 1, ts_cmp_byte);
        bg[i] = (BYTE)((v[3] + v[4]) / 2);
    }
    for (it = 0; it < 4; it++) {
        for (k = 0; k < 8; k++) {
            for (i = 0; i < 64 * 64; i++) {
                int d = 0;
                for (c = 0; c < 3; c++) d += abs(g_ts_ball[k][i * 3 + c] - bg[i * 3 + c]);
                grow[i] = d > 60;
            }
            memset(mask[k], 0, sizeof mask[k]);           /* grown by a pixel */
            for (y = 0; y < 64; y++)
                for (x = 0; x < 64; x++)
                    if (grow[y * 64 + x]) {
                        int dx, dy;
                        for (dy = -1; dy <= 1; dy++)
                            for (dx = -1; dx <= 1; dx++)
                                if (x + dx >= 0 && x + dx < 64 && y + dy >= 0 && y + dy < 64) mask[k][(y + dy) * 64 + x + dx] = 1;
                    }
        }
        if (it == 3) break;
        for (i = 0; i < 64 * 64; i++)
            for (c = 0; c < 3; c++) {
                BYTE v[8]; int n = 0;
                for (k = 0; k < 8; k++) if (!mask[k][i]) v[n++] = g_ts_ball[k][i * 3 + c];
                if (n) { qsort(v, n, 1, ts_cmp_byte); bg[i * 3 + c] = (BYTE)((v[(n - 1) / 2] + v[n / 2]) / 2); }
            }
    }
    /* the plain ball: ball 1 with its number's pixels filled in from their neighbours */
    for (i = 0; i < 64 * 64 * 3; i++) plain[i] = g_ts_ball[0][i];
    for (it = 0; it < 300; it++)
        for (i = 0; i < 64 * 64; i++) {
            float s[3] = { 0, 0, 0 };
            int n = 0, j, d;
            if (!mask[0][i] || pix[i] == clear) continue;
            x = i % 64; y = i / 64;
            for (d = 0; d < 4; d++) {
                int xx = x + off[d][0], yy = y + off[d][1];
                if (xx < 0 || xx > 63 || yy < 0 || yy > 63) continue;
                j = yy * 64 + xx;
                if (pix[j] == clear) continue;
                for (c = 0; c < 3; c++) s[c] += plain[j * 3 + c];
                n++;
            }
            if (n) for (c = 0; c < 3; c++) plain[i * 3 + c] = s[c] / n;
        }
    /* the label */
    k = num == 9 ? 0 : num - 9;
    ts_glyph(body, k, 3.6f, 0, 0);
    ts_glyph(inner, k, 1.4f, 0, 0);
    ts_glyph(shad, k, 3.6f, 1.5f, 1.5f);
    for (i = 0; i < 64 * 64; i++) {
        float ring = body[i] - inner[i], sh = shad[i] - body[i], rgb[3];
        int e, best = 0, bestd = 1 << 30;
        if (pix[i] == clear) continue;
        if (ring < 0) ring = 0;
        if (sh < 0) sh = 0;
        for (c = 0; c < 3; c++) {
            float v = plain[i * 3 + c] * (1 - 0.2f * body[i]);
            v = v * (1 - ring * 0.85f) + light[c] * ring * 0.85f;
            v *= 1 - sh * 0.6f;
            rgb[c] = v < 0 ? 0 : v > 255 ? 255 : v;
        }
        for (e = 0; e < 256; e++) {
            int d = 0;
            if (e == clear) continue;
            for (c = 0; c < 3; c++) { int q = pal[e * 4 + c] - (int)rgb[c]; d += q * q; }
            if (d < bestd) { bestd = d; best = e; }
        }
        pix[i] = (BYTE)best;
    }
    /* green, to tell custom tracks from the game's own */
    for (i = 0; i < 256; i++) if (i != clear) ts_hue(pal + i * 4, 200 / 360.0f);
    log_msg("tracks: painted ball %d", num);
}

/* ---- scripted features (lifts, traps) for slots 9-16 ----
 * Each track has a setup routine (table 0x454d98, run at race start) and an update routine
 * (0x454f38, run every tick); slots 9-16 point at an empty one. Both are made of calls to
 * shared building blocks (catalogue in TRACKS.md). Custom slots get ts_feat_setup/update:
 *  - the world's tile-driven features always run: Ice thin ice, Atlantis height-lock list and
 *    ambience, Night grab traps (tile 0x40), Amazon lifts (tile 0x57) and ambience, City
 *    lifts (tile 0x0c), launch pads (tile 0x3a) and ambience;
 *  - placed traps come from Track\<name>.TXT, one per line, "key = column, row[, ...]":
 *      falling_floor (Amazon), crusher, spring (Ice), snapper, whirlpool = col,row,height
 *      (Atlantis), swiper (City).
 * State slots (0x635a80, 512 dwords) and animated-cell entries (0x630a80, 15 usable) are
 * handed out after the ones the tile-driven features take. */
enum { TF_FLOOR, TF_CRUSHER, TF_SPRING, TF_SNAPPER, TF_WHIRL, TF_SWIPER, TF_TORNADO, TF_TRAIN, TF_ROLLER,
       TF_FLYER, TF_FISH, TF_WALKER, TF_ROAMER, TF_SPRAY, TF_TYPES };
/* worlds: bit per world (1 West, 2 Ice, 4 Atlantis, 8 Night, 16 Amazon, 32 City); max: how many
   a track may have (object features use fixed objects of the custom model row, below) */
static const struct { const char *key; int worlds, nargs, slots, anim, max; } k_tf[TF_TYPES] = {
    { "falling_floor", 16, 2, 3, 1, 15 },
    { "crusher",       2, 2, 8, 1, 15 },
    { "spring",        2, 2, 7, 1, 15 },
    { "snapper",       4, 2, 7, 1, 15 },
    { "whirlpool",     4, 3, 110, 0, 2 },            /* 100 + per-car records of cars 4-5; sound channel 1-2 */
    { "swiper",        32, 2, 20, 1, 15 },
    { "tornado",       1, 2, 60, 0, 1 },             /* object 9, sound channel 4 */
    { "train",         1, 5, 11, 0, 2 },             /* objects 4-8 / 11-15, channels 0-1 / 2-3 */
    { "roller",        1, 2, 10, 0, 1 },             /* object 10, channel 5 */
    { "flyer",         16, 3, 4, 0, 3 },             /* objects 4-6 */
    { "fish",          4, 3, 9, 0, 4 },              /* objects 1-4 */
    { "walker",        4, 3, 9, 0, 1 },              /* object 0 */
    { "roamer",        8 | 32, 3, 9, 0, 1 },         /* object 7 */
    { "spray",         2, 2, 20, 1, 6 },             /* shares a pool of objects 4-9 */
};
typedef struct { int type, a[6], slot, k, obj; } TS_FEAT;
/* Night follower: gate ranges (from, to, ..., -1) where an object follows the car */
static short g_ts_follow[2 * 16 + 1];
static int g_ts_nfollow, g_ts_spraypool;
/* Atlantis depth plan: (gate from, gate to, swim level) triples, 0xff-terminated, read by the AI
   from a per-track table (0x459f58, moved to g_ts_water). The AI swims up or down to the
   level of the zone it is in. */
static DWORD g_ts_water[96];
static const BYTE k_ts_nodepth[4] = { 0xFF, 0, 0, 0 };
static BYTE g_ts_depth[3 * 80 + 1];
static int g_ts_ndepth;
static TS_FEAT g_tf[24];
static int g_ntf, g_tf_world, g_tf_slot_follow;
#define TS_S(i)  (((volatile int *)0x00635A80)[i])
#define TS_CALL(a) ((void (__cdecl *)())(a))

static int ts_count_tiles(const int *ids, int n)
{
    const unsigned short *map = *(const unsigned short **)0x00465638;
    int w = G_I32(0x00488000), h = G_I32(0x00465634), i, k, c = 0;
    if (!map) return 0;
    for (i = 0; i < w * h; i++)
        for (k = 0; k < n; k++)
            if ((map[8 + i] & 0x7F) == ids[k]) { c++; break; }
    return c;
}

static void ts_load_features(int t)
{
    char path[MAX_PATH], line[128];
    FILE *f;
    g_ntf = 0;
    g_ts_ndepth = 0;
    g_ts_nfollow = 0;
    snprintf(path, sizeof path, "%sTrack\\%s.TXT", g_dir, ts_mapname(t));
    if ((f = fopen(path, "r")) == NULL) return;
    while (fgets(line, sizeof line, f) && g_ntf < (int)(sizeof g_tf / sizeof g_tf[0])) {
        char *eq, *v = line;
        int ty, n;
        line[strcspn(line, "\r\n#")] = 0;
        while (*v == ' ' || *v == '\t') v++;
        if (!(eq = strchr(v, '='))) continue;
        if (_strnicmp(v, "follower", 8) == 0) {
            int lo, hi;
            if (sscanf(eq + 1, " %d , %d", &lo, &hi) == 2 && g_ts_nfollow < 16) {
                g_ts_follow[2 * g_ts_nfollow] = (short)lo; g_ts_follow[2 * g_ts_nfollow + 1] = (short)hi; g_ts_nfollow++;
            }
            continue;
        }
        if (_strnicmp(v, "depth", 5) == 0) {
            int lo, hi, lv;
            if (sscanf(eq + 1, " %d , %d , %d", &lo, &hi, &lv) == 3 && g_ts_ndepth < 80 && lo >= 0 && hi <= 254 && lv >= 0 && lv <= 3) {
                g_ts_depth[3 * g_ts_ndepth] = (BYTE)lo; g_ts_depth[3 * g_ts_ndepth + 1] = (BYTE)hi;
                g_ts_depth[3 * g_ts_ndepth + 2] = (BYTE)lv; g_ts_ndepth++;
            }
            continue;
        }
        for (ty = 0; ty < TF_TYPES; ty++)
            if (_strnicmp(v, k_tf[ty].key, strlen(k_tf[ty].key)) == 0) break;
        if (ty == TF_TYPES) continue;
        memset(&g_tf[g_ntf], 0, sizeof g_tf[0]);
        {
            int *a = g_tf[g_ntf].a, used = 0, j;
            n = sscanf(eq + 1, " %d , %d , %d , %d , %d", &a[0], &a[1], &a[2], &a[3], &a[4]);
            for (j = 0; j < g_ntf; j++) used += g_tf[j].type == ty;
            if (used >= k_tf[ty].max) { log_msg("tracks: %s: at most %d %s", path, k_tf[ty].max, k_tf[ty].key); continue; }
            g_tf[g_ntf].obj = used;                      /* nth of its kind */
        }
        if (!(k_tf[ty].worlds & (1 << (t >> 4)))) { log_msg("tracks: %s: %s is not a feature of this world, skipped", path, k_tf[ty].key); continue; }
        if (n < k_tf[ty].nargs) { log_msg("tracks: %s: %s needs %d numbers", path, k_tf[ty].key, k_tf[ty].nargs); continue; }
        g_tf[g_ntf++].type = ty;
    }
    fclose(f);
}

/* No depth lines: follow the tile height under each gate's racing point, changing level 4
   gates early as the shipped tracks do (the map buffer holds the whole file: header, tiles,
   racing line, gate grid, then the gates). */
static void ts_auto_depth(void)
{
    const short *map = *(const short **)0x00465638, *g;
    int w = G_I32(0x00488000), h = G_I32(0x00465634), n, i, lv[256], cur = -1;
    if (!map || w <= 0 || h <= 0) return;
    g = map + 8 + w * h + (16 * w * h) / 2 + 16 * w * h;
    n = (g[0] + 8) / 8;
    if (n > 0 && g[8 * (n - 1) + 1] == -1) n--;              /* the end record */
    if (n > 255) n = 255;
    for (i = 0; i < n; i++) {
        int cx = g[8 * i + 6] / 8, cy = g[8 * i + 7] / 8;
        lv[i] = cx >= 0 && cx < w && cy >= 0 && cy < h ? (map[8 + cy * w + cx] >> 12) & 3 : 0;
    }
    g_ts_ndepth = 0;
    for (i = 0; i < n && g_ts_ndepth < 80; i++) {
        int l = lv[i + 4 < n ? i + 4 : (i + 4) - n];
        if (l != cur) {
            g_ts_depth[3 * g_ts_ndepth] = (BYTE)i; g_ts_depth[3 * g_ts_ndepth + 2] = (BYTE)l;
            if (g_ts_ndepth) g_ts_depth[3 * g_ts_ndepth - 2] = (BYTE)(i - 1);
            g_ts_ndepth++;
            cur = l;
        }
        g_ts_depth[3 * g_ts_ndepth - 2] = (BYTE)i;
    }
}

static void __cdecl ts_feat_setup(void)
{
    static const int ice[6] = { 0x35, 0x36, 0x3A, 0x3B, 0x7A, 0x7B }, grab[1] = { 0x40 }, lift_m[1] = { 0x57 },
                     lift_c[1] = { 0x0C }, pad[1] = { 0x3A };
    int t = G_I32(0x00634C44), w = t >> 4, slot = 2, k = 0, i;
    g_tf_world = w;
    g_ts_spraypool = 0;
    ts_load_features(t);
    switch (w) {                                         /* the world's tile-driven features */
    case 1: TS_CALL(0x00416DD0)(100, 1); slot = 100 + 10 * ts_count_tiles(ice, 6); break;
    case 2: TS_CALL(0x004185C0)();
            if (!g_ts_ndepth) ts_auto_depth();
            g_ts_depth[3 * g_ts_ndepth] = 0xFF;
            if (g_ts_water[t]) g_ts_water[t] = (DWORD)g_ts_depth;
            log_msg("tracks: %d depth zones", g_ts_ndepth);
            break;
    case 3: k = ts_count_tiles(grab, 1); TS_CALL(0x00419D30)(0x006381E0, 0x40); TS_CALL(0x0041A280)(0x006381E0, 10);
            slot = 10 + 20 * k; break;
    case 4: TS_CALL(0x00417FA0)(0, 30); slot = 30 + 30 * ts_count_tiles(lift_m, 1); break;
    case 5: TS_CALL(0x0041B690)(0, 30); TS_CALL(0x0041AC60)(1, (TS_S(0) + 1) * 30);
            slot = (ts_count_tiles(lift_c, 1) + 1) * 30 + 10 * ts_count_tiles(pad, 1); break;
    }
    for (i = 0; i < g_ntf; i++) {                        /* placed traps */
        TS_FEAT *e = &g_tf[i];
        const int *a = e->a;
        if (slot + k_tf[e->type].slots > 512 || (k_tf[e->type].anim && k > 14)) {
            log_msg("tracks: too many features, %s at %d,%d left out", k_tf[e->type].key, a[0], a[1]);
            g_ntf = i;
            break;
        }
        e->slot = slot; slot += k_tf[e->type].slots;
        e->k = k_tf[e->type].anim ? k++ : 0;
        switch (e->type) {
        case TF_FLOOR:   TS_CALL(0x00418030)(a[0], a[1], e->k, e->slot); break;
        case TF_CRUSHER: TS_CALL(0x00416590)(a[0], a[1], e->k, e->slot); break;
        case TF_SPRING:  TS_CALL(0x00416900)(a[0], a[1], e->k, e->slot); break;
        case TF_SNAPPER: TS_CALL(0x004186A0)(a[0], a[1], e->k, e->slot); break;
        case TF_WHIRL:   TS_CALL(0x00418F10)(e->slot); break;
        case TF_SWIPER:  TS_CALL(0x0041B760)(0, a[0], a[1], e->slot, e->k); break;
        case TF_TORNADO: TS_CALL(0x00416220)(e->slot, 9, a[0] * 0x3F8 + 0x1FC, -0x200, -(a[1] * 0x3F8 + 0x1FC), 0); break;
        case TF_TRAIN: {   /* start column, row, direction (0 +x, 1 down, 2 -x, 3 up), min, max */
            int tbl[6];
            tbl[0] = a[0]; tbl[1] = 0; tbl[2] = a[1]; tbl[3] = a[2] & 3; tbl[4] = a[3]; tbl[5] = a[4];
            TS_CALL(0x004156E0)(e->slot, tbl);
            e->obj = e->obj ? 11 : 4;
            break;
        }
        case TF_ROLLER:  TS_CALL(0x00415FE0)(e->slot, 10, a[0] * 0x3F8 + 0x1FC, -0x100, -(a[1] * 0x3F8 + 0x1FC)); break;
        case TF_FLYER:   e->obj += 4; TS_CALL(0x00418300)(e->slot, e->obj, a[0], a[1], a[2]); break;
        case TF_FISH:    e->obj += 1; TS_CALL(0x00418D40)(a[0], a[1], a[2], e->obj, e->slot); break;
        case TF_WALKER:  TS_CALL(0x00418AE0)(a[0], a[1], a[2], 0, e->slot); break;
        case TF_ROAMER:
            if (w == 3) TS_CALL(0x0041A330)(e->slot, 7, a[0] * 0x3F8 + 0x1FC, -(a[1] * 0x3F8 + 0x1FC), a[2]);
            else TS_CALL(0x0041BC10)(e->slot, 7, a[0] * 0x3F8 + 0x1FC, -(a[1] * 0x3F8 + 0x1FC), a[2]);
            break;
        case TF_SPRAY:
            if (!g_ts_spraypool) {                       /* one pool of 6 spray objects (4-9) */
                if (slot + 0x2A > 512) break;
                g_ts_spraypool = slot; slot += 0x2A;
                TS_CALL(0x00416EB0)(4, g_ts_spraypool);
            }
            TS_CALL(0x00417030)(a[0], a[1], e->k, e->slot);
            break;
        }
        log_msg("tracks: %s at %d,%d (state %d, cell anim %d)", k_tf[e->type].key, a[0], a[1], e->slot, e->k);
    }
    g_tf_slot_follow = slot;                             /* the Night follower: 9 dwords */
}

static void __cdecl ts_feat_update(void)
{
    int w = g_tf_world, i;
    switch (w) {
    case 1: TS_CALL(0x00416E70)(100, 1); break;
    case 2: TS_CALL(0x00419340)(); break;
    case 3: TS_CALL(0x0041A6C0)(); TS_CALL(0x0041A2E0)(0x006381E0, 10); break;
    case 4: TS_CALL(0x0041BF90)(); for (i = 0; i < TS_S(0) && i < 16; i++) TS_CALL(0x00417790)(30 * (i + 1)); break;
    case 5: TS_CALL(0x0041BF90)(); TS_CALL(0x0041B720)(30); TS_CALL(0x0041ACE0)((TS_S(0) + 1) * 30); break;
    }
    for (i = 0; i < g_ntf; i++) {
        TS_FEAT *e = &g_tf[i];
        const int *a = e->a;
        switch (e->type) {
        case TF_FLOOR:   TS_CALL(0x00418070)(a[0], a[1], e->k, e->slot); break;
        case TF_CRUSHER: TS_CALL(0x00416610)(e->k, e->slot); break;
        case TF_SPRING:  TS_CALL(0x00416980)(e->k, e->slot); break;
        case TF_SNAPPER: TS_CALL(0x00418750)(e->k, e->slot); break;
        case TF_WHIRL:   TS_CALL(0x00418FF0)(a[0], a[2], a[1], e->slot, e->obj); break;
        case TF_SWIPER:  TS_CALL(0x0041B950)(e->slot, e->k); break;
        case TF_TORNADO: TS_CALL(0x00416280)(e->slot, 9); break;
        case TF_TRAIN:   TS_CALL(0x00415780)(e->slot, e->obj, e->obj == 4 ? 0 : 1); break;
        case TF_ROLLER:  TS_CALL(0x00416050)(e->slot, 10); break;
        case TF_FLYER:   TS_CALL(0x00418360)(e->slot, e->obj); break;
        case TF_FISH:    TS_CALL(0x00418DC0)(e->obj, e->slot); break;
        case TF_WALKER:  TS_CALL(0x00418B60)(0, e->slot); break;
        case TF_ROAMER:  TS_CALL(w == 3 ? 0x0041A390 : 0x0041BC70)(e->slot, 7); break;
        case TF_SPRAY:   if (g_ts_spraypool) TS_CALL(0x004170B0)(e->k, e->slot, g_ts_spraypool); break;
        }
    }
    if (g_ts_spraypool) TS_CALL(0x00416F90)(4, g_ts_spraypool);
    if (w == 3 && g_ts_nfollow) {
        g_ts_follow[2 * g_ts_nfollow] = -1;
        TS_CALL(0x00419A50)(g_tf_slot_follow, 4, g_ts_follow);
    }
}

/* The small ball in the parrot's hand on the mode screen is front-end sprite 0x96 + (slot & 7)
   (0x41101a: and edx,7 / add edx,96h). There is no small ball for slots 9-16, so a custom
   track gets id 0x89, which maps to no sprite (0x4868e0[0x89] = -1): nothing is drawn. */
static __declspec(naked) void ts_icon_thunk(void)
{
    __asm {
        test edx, 8
        jnz custom
        and edx, 7
        add edx, 96h
        ret
    custom:
        mov edx, 89h
        ret
    }
}

/* Clock.ini holds the time-trial ghosts, 8 per world: 0x409210 (load) and 0x40c8bf (save,
   after a new lap record) find a track's ghost at world * 8 + (slot & 7), so a custom slot
   would read, and overwrite, a shipped track's ghost. Opening the file (0x42f0d0) fails for
   custom slots; both callers already skip the ghost then. */
typedef int (__cdecl *PFN_FOPEN)(const char *name, int mode);
static PFN_FOPEN g_ts_fopen_orig;
static int __cdecl ts_fopen_hook(const char *name, int mode)
{
    if (g_ts_on && name && (G_I32(0x00634C44) & 15) >= 8 && _stricmp(name, "Clock.ini") == 0) {
        G_I32(0x00504FC8) = 100;
        return 0;
    }
    return g_ts_fopen_orig(name, mode);
}

/* Misc\JUKE09.x .. JUKE16.x from JUKE01.x, with the texture name changed */
static int ts_make_models(void)
{
    char path[MAX_PATH];
    BYTE *src;
    long n;
    FILE *f;
    int k, made = 0;
    snprintf(path, sizeof path, "%sMisc\\JUKE01.x", g_dir);
    if ((f = fopen(path, "rb")) == NULL) return 0;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    src = (BYTE *)malloc(n);
    if (!src || fread(src, 1, n, f) != (size_t)n) { fclose(f); free(src); return 0; }
    fclose(f);
    for (k = 0; k < 8; k++) {
        long i;
        int hits = 0;
        snprintf(path, sizeof path, "%sMisc\\%s.x", g_dir, g_ts_ballname[k]);
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) { made++; continue; }
        for (i = 0; i + 8 <= n; i++)
            if (_strnicmp((char *)src + i, "juke0001", 8) == 0) {
                src[i + 6] = (char)('0' + (k + 9) / 10);
                src[i + 7] = (char)('0' + (k + 9) % 10);
                hits++;
            }
        if (hits && (f = fopen(path, "wb")) != NULL) {
            fwrite(src, 1, n, f);
            fclose(f);
            made++;
        }
        for (i = 0; i + 8 <= n; i++)                   /* back to juke0001 for the next copy */
            if (_strnicmp((char *)src + i, "juke00", 6) == 0 && src[i + 6] >= '0' && src[i + 6] <= '1') {
                src[i + 6] = '0'; src[i + 7] = '1';
            }
    }
    free(src);
    return made == 8;
}

static void install_tracks(void)
{
    DWORD old;
    int s, bad = 0, k;
    if (G_I32(0x00450EDC) != 0x0040A620) { log_msg("tracks: page table differs, custom tracks off"); return; }
    if (G_I32(0x00404461) != 0x00445AE8 || G_I32(0x00404A33) != TS_OBJ_RECS * TS_OBJ_REC ||
        strcmp(*(const char **)(0x00445AE8 + TS_JUKE01_REC * TS_OBJ_REC), "JUKE01") != 0) {
        log_msg("tracks: object list differs, custom tracks off");
        return;
    }
    for (s = 0; s < FE_RELOC_SITES; s++)
        if (*(DWORD *)g_fe_rsite[s].at != g_fe_rsite[s].old) bad++;
    if (bad) { log_msg("tracks: %d object-table sites differ, custom tracks off", bad); return; }
    if (!ts_make_models()) { log_msg("tracks: could not make the ball models, custom tracks off"); return; }

    /* 1. the object state table -> 65 entries */
    memcpy(g_fe_rnew, (BYTE *)g_fe_rtab[0].base, g_fe_rtab[0].elem * g_fe_rtab[0].oldn);
    VirtualProtect((BYTE *)0x401000, 0x3B200, PAGE_EXECUTE_READWRITE, &old);
    for (s = 0; s < FE_RELOC_SITES; s++)
        *(DWORD *)g_fe_rsite[s].at = (DWORD)g_fe_rnew + (g_fe_rsite[s].old - g_fe_rtab[0].base);
    /* 2. the object list + balls 9-16 */
    memcpy(g_ts_objtab, (BYTE *)0x00445AE8, TS_OBJ_RECS * TS_OBJ_REC);
    for (k = 0; k < 8; k++) {
        BYTE *r = g_ts_objtab + (TS_OBJ_RECS + k) * TS_OBJ_REC;
        memcpy(r, (BYTE *)0x00445AE8 + TS_JUKE01_REC * TS_OBJ_REC, TS_OBJ_REC);
        *(const char **)r = g_ts_ballname[k];
    }
    *(DWORD *)0x00404461 = (DWORD)g_ts_objtab;
    *(DWORD *)0x00404A33 = (TS_OBJ_RECS + 8) * TS_OBJ_REC;
    /* the page change (0x408348) resets the object state table: 0x201 dwords = 57 objects */
    if (G_I32(0x00408349) == 0x201) *(DWORD *)0x00408349 = 65 * 9;
    else log_msg("tracks: object reset site differs, balls 9-16 may stay on screen");
    {   /* AI speed zones: 0x421cbf reads a per-track list (gate from, gate to, top speed, ...,
           -1) from 0x45a240[track], a table of only 88 entries, so City slots 9-16 read the next
           table's numbers as pointers and crash. Give it 96 entries: custom slots of every
           world get the empty list the other worlds' slots 9-16 already use (0x45a098). */
        static DWORD speed[96];
        int t;
        if (G_I32(0x00421CC2) == 0x0045A240) {
            for (t = 0; t < 96; t++) speed[t] = t < 88 && (t & 15) < 8 ? ((DWORD *)0x0045A240)[t] : 0x0045A098;
            *(DWORD *)0x00421CC2 = (DWORD)speed;
        } else log_msg("tracks: AI speed table site differs, City custom slots may crash");
    }
    {   /* Atlantis (underwater, [0x631a54] = 1): 0x421d13 and 0x422448 read a byte list (gate
           from, gate to, ..., 0xff) from 0x459f58[track]; only Atlantis 1-8 (tracks 32-39) have
           entries, so Atlantis slots 9-16 read garbage. Every other track gets an empty list. */
        int t;
        if (G_I32(0x00421D16) == 0x00459F58 && G_I32(0x0042244B) == 0x00459F58) {
            for (t = 0; t < 96; t++) g_ts_water[t] = t >= 32 && t < 40 ? ((DWORD *)0x00459F58)[t] : (DWORD)k_ts_nodepth;
            *(DWORD *)0x00421D16 = (DWORD)g_ts_water;
            *(DWORD *)0x0042244B = (DWORD)g_ts_water;
        } else log_msg("tracks: Atlantis zone table sites differ, Atlantis custom slots may misbehave");
    }
    {   /* the parrot's small ball: and edx,7 / add edx,96h -> call ts_icon_thunk, 4 x nop */
        static const BYTE ic_expect[9] = { 0x83, 0xE2, 0x07, 0x81, 0xC2, 0x96, 0x00, 0x00, 0x00 };
        BYTE *at = (BYTE *)0x00411021;
        if (memcmp(at, ic_expect, 9) == 0) {
            at[0] = 0xE8;
            *(INT32 *)(at + 1) = (INT32)((BYTE *)ts_icon_thunk - (at + 5));
            memset(at + 5, 0x90, 4);
        } else log_msg("tracks: parrot icon site differs");
    }
    VirtualProtect((BYTE *)0x401000, 0x3B200, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)0x401000, 0x3B200);
    {
        static const BYTE fo_expect[7] = { 0x55, 0x8B, 0xEC, 0x0F, 0xBF, 0x45, 0x0C };
        g_ts_fopen_orig = (PFN_FOPEN)make_trampoline(fo_expect, 7, (BYTE *)0x0042F0D7);
        if (!g_ts_fopen_orig || !patch_jmp((BYTE *)0x0042F0D0, fo_expect, 7, ts_fopen_hook))
            log_msg("tracks: Clock.ini guard failed: time-trial ghosts of custom slots share slots 1-8");
    }

    {   /* 3D objects and pickups for slots 9-16. Per-track object models load from
           0x444168[track * 16 + object] (a model id of the world, 0 = none; only an unbroken
           run from object 0 is drawn) and pickups from 0x45b650[track * 9] (first object,
           then 4 pickup kinds). Slots 9-16 had neither, so no power-ups appeared. They take
           their world's slot 1 row; West gets one with room for two trains (4-8, 11-15), the
           tornado (9) and the roller (10). Objects 0-3 are the pickups (5-8 in Atlantis). */
        static const int west[16] = { 133, 133, 133, 133, 128, 130, 129, 129, 129, 131, 132, 128, 130, 129, 129, 129 };
        int t, i;
        for (t = 0; t < 96; t++)
            if ((t & 15) >= 8) {
                for (i = 0; i < 16; i++)
                    ((int *)0x00444168)[t * 16 + i] = (t >> 4) == 0 ? west[i] : ((int *)0x00444168)[(t & 0xF0) * 16 + i];
                memcpy((BYTE *)0x0045B650 + t * 9, (BYTE *)0x0045B650 + (t & 0xF0) * 9, 9);
            }
    }
    {   /* lifts and traps for slots 9-16 */
        int t;
        for (t = 0; t < 96; t++)
            if ((t & 15) >= 8) {
                ((DWORD *)0x00454D98)[t] = (DWORD)ts_feat_setup;
                ((DWORD *)0x00454F38)[t] = (DWORD)ts_feat_update;
            }
    }
    /* 3. the track select page */
    VirtualProtect((BYTE *)0x00450EDC, 4, PAGE_READWRITE, &old);
    *(DWORD *)0x00450EDC = (DWORD)tracksel_page;
    VirtualProtect((BYTE *)0x00450EDC, 4, old, &old);
    ts_scan();
    g_ts_on = 1;
    {
        int t, n = 0;
        for (t = 0; t < 96; t++) n += g_ts_custom[t];
        log_msg("tracks: custom track slots on, %d custom map(s) found", n);
    }
}
