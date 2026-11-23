/* ---- six racers ---------------------------------------------------------------------
 * The game races at most 4 cars. The car records (0x632DA0, 0x460 each) already have room
 * for 6 (0x633F20-0x6347E0 is unused) and so do the start grid (0x457328, 2 x 3 slots),
 * the character / input-slot tables (0x634C04, 0x634C28) and the input words. Everything
 * else that holds one entry per car is moved to larger copies here: relocs.h (made by
 * tools/sixcars/gen_relocs.py) lists every code site holding an address inside those
 * tables, and each is pointed at the copy at start-up.
 *
 * Code that is written for exactly 4 cars is replaced or extended:
 *   0x41E660 ranking            -> rank_hook (all cars)
 *   0x422FF0 car-vs-car pairs   -> collide_hook (all pairs)
 *   0x4240E0 single-player catch-up (cars 1-3) -> catchup_hook (cars 1..n-1)
 *   0x420F10 lap-time ticker (4) -> laptime_hook
 *   0x420700 race init: zero cars 4-5 first; their HUD character + Atlantis handicap after
 *   0x421000 path setup is called for cars 0-3 only -> also for 4-5
 *   0x4221A0 finish order (4 explicit records) -> extended after it runs
 *   0x41F880 CPU weapon aiming keeps three [4] arrays on the stack -> frame enlarged
 *   0x426420 HUD portraits 40 px apart -> 34 px when 6 cars race
 * Extra cars are added only to Single Race / Practice (game type 1 / 0, race mode 0, not network): the
 * League, Knockout and team code keep 4-slot rosters and points tables.
 * The race-load hook on 0x401070 belongs to debugmenu.h, which calls six_prepare.
 * Settings: mmcda.ini [race] cars=6 (default 6; cars=4 = off).
 */
#include "relocs.h"

/* per-car frame tables used by car.h / cams.h (moved when six cars are enabled) */
static void **g_t_bodyframe = (void **)0x00488568;
static void **g_t_shadframe = (void **)0x00485C18;
static volatile int *g_t_placed = (volatile int *)0x00485C08;

#define SIX_MAX 6
typedef void (__cdecl *PFN_VOID0)(void);
static int g_six_cars = 4;        /* setting */
static int g_six_active;          /* this race has extra cars */
static DWORD g_rib_slot[2];       /* rosette sprite for places 5 / 6 (0 = the game's) */
static int g_rib_ready;
static BYTE *g_rnew[RELOC_TABLES];
static __declspec(align(16)) BYTE g_rpool[4096];

#define SIX_COUNT   (*(volatile int *)0x00634C00)
#define SIX_CHAR(i) (((volatile int *)0x00634C04)[i])
#define SIX_SLOT(i) (((volatile int *)0x00634C28)[i])
#define SIX_CAR(i)  ((BYTE *)0x00632DA0 + (i) * 0x460)
#define SIX_I(c, o) (*(volatile int *)((c) + (o)))
#define RT_I(t, i)  (((volatile int *)g_rnew[t])[i])

static int six_n(void)   /* cars the fixed-4 loops should walk */
{
    int n = SIX_COUNT;
    return n < 4 ? 4 : n > SIX_MAX ? SIX_MAX : n;
}

/* ---- start of a race (0x401070 loads the track and calls the race init) ---- */
static void six_hud_spacing(int px)
{
    static const BYTE lea_shl[6] = { 0x8D, 0x14, 0xB6, 0xC1, 0xE2, 0x03 };
    BYTE *a = (BYTE *)0x0042679C, *b = (BYTE *)0x00426508, code[6] = { 0x6B, 0xD6, 0, 0x90, 0x90, 0x90 };
    DWORD old;
    if (!(memcmp(a, lea_shl, 6) == 0 || (a[0] == 0x6B && a[1] == 0xD6 && a[3] == 0x90)) ||
        b[-2] != 0x83 || b[-1] != 0xC6) return;
    VirtualProtect(a, 6, PAGE_EXECUTE_READWRITE, &old);
    if (px == 40) memcpy(a, lea_shl, 6); else { code[2] = (BYTE)px; memcpy(a, code, 6); }   /* imul edx,esi,px */
    VirtualProtect(a, 6, old, &old);
    VirtualProtect(b, 1, PAGE_EXECUTE_READWRITE, &old);
    *b = (BYTE)px;                                                   /* add esi,px */
    VirtualProtect(b, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), a, 0x20);
    /* The HUD overlay is 320x240, but its sprite drawer 0x4075B0 clips at y 200 unless the
       game's hi-res flag is set ("and esi,28h" -> 0 or 40 extra rows). With 6 portraits
       34 px apart the last one and its rosette reach y 238: clip at 240 ("push 28h; pop esi"). */
    {
        static const BYTE and28[3] = { 0x83, 0xE6, 0x28 }, push28[3] = { 0x6A, 0x28, 0x5E };
        BYTE *c = (BYTE *)0x00407672;
        if (memcmp(c, and28, 3) == 0 || memcmp(c, push28, 3) == 0) {
            VirtualProtect(c, 3, PAGE_EXECUTE_READWRITE, &old);
            memcpy(c, px == 40 ? and28 : push28, 3);
            VirtualProtect(c, 3, old, &old);
            FlushInstructionCache(GetCurrentProcess(), c, 3);
        }
    }
}

/* extra slots chosen in the main menu: 0-11 character, EXTRA_NONE, EXTRA_RANDOM */
#define EXTRA_NONE   12
#define EXTRA_RANDOM 13
static int g_extra[2] = { EXTRA_RANDOM, EXTRA_RANDOM };
static int g_six_last;            /* car count of the last race we expanded (restart) */

static void six_prepare(void)
{
    int n = SIX_COUNT, i, k, used = 0, c, want = 4;
    g_six_active = 0;
    g_rib_ready = 0; g_rib_slot[0] = g_rib_slot[1] = 0;
    for (k = 0; k < SIX_MAX - 4; k++) if (g_extra[k] != EXTRA_NONE) want++;
    if (want > g_six_cars) want = g_six_cars;
    if (want > 4 && !G_I32(0x0054AEC4) && G_I32(0x00634C44) < 0x60 &&
        G_I32(0x00634C20) == 0 && G_I32(0x0043F36C) <= 1 && (n == 4 || (n > 4 && n == g_six_last))) {
        for (i = 0; i < 4; i++) if (SIX_CHAR(i) >= 0 && SIX_CHAR(i) < 12) used |= 1 << SIX_CHAR(i);
        c = (int)(GetTickCount() % 12);
        for (i = 4, k = 0; i < want && k < SIX_MAX - 4; k++) {
            int e = g_extra[k];
            if (e == EXTRA_NONE) continue;
            if (e < 0 || e >= 12 || (used & (1 << e))) {          /* random, or taken */
                while (used & (1 << c)) c = (c + 1) % 12;
                e = c;
            }
            used |= 1 << e;
            SIX_CHAR(i) = e;
            SIX_SLOT(i) = i;                   /* CPU input slot = car index */
            RT_I(RT_RANK, i) = i;
            i++;
        }
        SIX_COUNT = g_six_last = want;
        g_six_active = 1;
        log_msg("six: race with %d cars (extra characters %d %d)", want, SIX_CHAR(4), want > 5 ? SIX_CHAR(5) : -1);
    } else if (g_six_cars > 4)
        log_msg("six: %d-car race (mode %d, game type %d)", n, G_I32(0x00634C20), G_I32(0x0043F36C));
    six_hud_spacing(g_six_active ? 34 : 40);   /* 6 rows + rosette fit in 240 */
}

/* ---- main menu: two more racer slots (rows 11 and 12) above the top two portraits ----
 * The front-end item table (0x451FE0, 0x38 per row: home x,y, nav up/down/left/right,
 * enabled, sprite, current x,y, state, ...) is moved to a 13-row copy (relocs.h). The game
 * navigates into the new rows through their nav links, but its draw (0x410CB0) and text
 * code only know rows 0-10, so the main-menu screen 0x4083F0 is wrapped: Enter on a new
 * row cycles Random -> free characters -> None; the portraits and the name are added to the
 * frame's sprite list; the gun man points as for the portrait below. Saved in
 * mmcda.ini [race] extra1 / extra2. */
#define MENU_ROW(r)   ((volatile int *)(g_rnew[RT_MENU] + (r) * 0x38))
#define MENU_SEL      (*(volatile int *)0x0050731C)
#define MENU_KEY      (*(volatile int *)0x00505930)
#define SPR_COUNT     (*(volatile int *)0x00505054)
#define SPR(i)        ((volatile int *)(0x00505ED0 + (i) * 12))
static int g_in_mainmenu, g_spr_after_draw = -1;

static void six_save_extras(void)
{
    char path[MAX_PATH], v[8];
    snprintf(path, sizeof path, "%smmcda.ini", g_home);
    snprintf(v, sizeof v, "%d", g_extra[0]); WritePrivateProfileStringA("race", "extra1", v, path);
    snprintf(v, sizeof v, "%d", g_extra[1]); WritePrivateProfileStringA("race", "extra2", v, path);
}

static int six_slots_on(void)
{
    return g_six_cars > 4 && G_I32(0x0043F36C) <= 1 && G_I32(0x00505BD8) == 4 && !G_I32(0x0054AEC4);
}

static int six_extra_sprite(int e)
{
    return e == EXTRA_RANDOM ? 223 /* "?" */ : e + 0x7D;    /* portraits: character + 0x7D, 12 = empty */
}

typedef void (__cdecl *PFN_VOID1)(int);
static PFN_VOID1 g_menulist_orig;      /* 0x410CB0(int) - builds the menu sprite list */
static void __cdecl menulist_hook(int first)
{
    int row = MENU_SEL, k;
    if (g_in_mainmenu && (row == 11 || row == 12)) MENU_SEL = row - 7;   /* gun man points up-right */
    g_menulist_orig(first);
    MENU_SEL = row;
    if (!g_in_mainmenu) return;
    for (k = 0; k < 2; k++) {
        volatile int *m = MENU_ROW(11 + k);
        if (!m[6]) continue;
        SPR(SPR_COUNT)[0] = m[8]; SPR(SPR_COUNT)[1] = m[9] - (row == 11 + k ? 2 : 0);
        SPR(SPR_COUNT)[2] = six_extra_sprite(g_extra[k]);
        SPR_COUNT++;
    }
    g_spr_after_draw = SPR_COUNT;
}

/* The main-menu background (16-bit modes) is an 8-bit picture the game draws in software
   (0x4075B0): image slot *(0x61D3E8); per slot (0x410 bytes from 0x48F37C) the pixel pointer,
   then width at +0x404 and height at +0x408; palette 0x488BE0. */
#define BGIMG_PIX(s)  (*(BYTE **)(0x0048F37C + (s) * 0x410))
#define BGIMG_W(s)    (*(int *)(0x0048F780 + (s) * 0x410))
#define BGIMG_H(s)    (*(int *)(0x0048F784 + (s) * 0x410))

static void six_dump_bg(void)
{
    int s = G_I32(0x0061D3E8), w, h;
    BYTE *pix;
    char name[MAX_PATH];
    FILE *f;
    if (s < 0 || s > 0x200) return;
    pix = BGIMG_PIX(s); w = BGIMG_W(s); h = BGIMG_H(s);
    log_msg("six: menu background slot %d %dx%d at %p", s, w, h, pix);
    if (!pix || w <= 0 || h <= 0 || w > 1024 || h > 1024) return;
    snprintf(name, sizeof name, "%sbg_%dx%d.raw", g_home, w, h);
    if ((f = fopen(name, "wb"))) { fwrite(pix, 1, (size_t)w * h, f); fwrite((void *)0x00488BE0, 4, 256, f); fclose(f); }
}

/* Frame boxes for the new slots: copy the boxes of the top two portraits (with a 2 px
   margin) 38 px up in the background picture; put the original pixels back when the slots
   are hidden (League etc.) or the picture changes. */
static const RECT k_box_src[2] = { { 240, 53, 278, 90 }, { 280, 65, 318, 102 } };
#define BOX_DY 38
#define BOX_DX 2
static BYTE *g_box_img;                       /* picture currently patched */
static BYTE g_box_save[2][40 * 40];
static void six_bg_boxes(int on)
{
    int s = G_I32(0x0061D3E8), w, h, k, y;
    BYTE *pix;
    if (s < 0 || s > 0x200) return;
    pix = BGIMG_PIX(s); w = BGIMG_W(s); h = BGIMG_H(s);
    if (g_box_img && (g_box_img != pix || !on)) {           /* undo */
        if (g_box_img == pix)
            for (k = 0; k < 2; k++) {
                const RECT *r = &k_box_src[k];
                int bw = r->right - r->left;
                for (y = r->top; y < r->bottom; y++)
                    memcpy(pix + (y - BOX_DY) * w + r->left - BOX_DX, g_box_save[k] + (y - r->top) * bw, bw);
            }
        g_box_img = NULL;
    }
    if (!on || g_box_img || !pix || w != 320 || h < 120 || s != 5) return;
    for (k = 0; k < 2; k++) {
        const RECT *r = &k_box_src[k];
        int bw = r->right - r->left;
        for (y = r->top; y < r->bottom; y++) {
            BYTE *d = pix + (y - BOX_DY) * w + r->left - BOX_DX;
            memcpy(g_box_save[k] + (y - r->top) * bw, d, bw);
            memcpy(d, pix + y * w + r->left, bw);
        }
    }
    g_box_img = pix;
}

/* 0x411620(table, row): text from a per-row (x5 languages) string table. The main menu
   labels rows above 7 with entry row-4, which is NULL for row 11; mainmenu_hook writes the
   new rows' labels itself. */
static void __cdecl label_hook(const char *const *tab, int idx)
{
    const int *style = *(const int **)0x00505038;
    if (g_in_mainmenu && tab == (const char *const *)0x00451478 && idx >= 7) return;
    if (style[6]) idx = G_I32(0x0043F1F4) + idx * 5;
    if (tab[idx]) ((void (__cdecl *)(const char *))0x004116C0)(tab[idx]);
}

static PFN_VOID0 g_mainmenu_orig;
static void __cdecl mainmenu_hook(void)
{
    static int dumped;
    if (!dumped && G_I32(0x005050D8) && GetEnvironmentVariableA("MMCDA_DUMPBG", NULL, 0)) { dumped = 1; six_dump_bg(); }
    int on = six_slots_on(), row, k;
    MENU_ROW(11)[6] = MENU_ROW(12)[6] = on;
    six_bg_boxes(on);
    if (!on && (MENU_SEL == 11 || MENU_SEL == 12)) MENU_SEL = MENU_SEL - 7;
    row = MENU_SEL;
    if ((row == 11 || row == 12) && G_I32(0x00507008) == 0 && (MENU_KEY == 1 || MENU_KEY == 0x1000)) {
        int *e = &g_extra[row - 11], used = 0, i, tries;
        for (i = 0; i < 4; i++) { int c = ((volatile int *)0x0043F22C)[i]; if (c >= 0 && c < 12) used |= 1 << c; }
        k = g_extra[12 - row];                                 /* the other slot */
        if (k >= 0 && k < 12) used |= 1 << k;
        for (tries = 0; tries < 16; tries++) {                 /* Random, 0..11 (free), None, Random... */
            *e = *e == EXTRA_RANDOM ? 0 : *e == EXTRA_NONE ? EXTRA_RANDOM : *e + 1;
            if (*e >= 12 || !(used & (1 << *e))) break;
        }
        ((void (__cdecl *)(int, int, int, int))0x004130D0)(0, G_I32(0x005076CC), 3000, 0x7F);
        MENU_KEY = 0;
        six_save_extras();
    }
    g_in_mainmenu = 1; g_spr_after_draw = -1;
    g_mainmenu_orig();
    g_in_mainmenu = 0;
    row = MENU_SEL;
    if ((row == 11 || row == 12) && g_spr_after_draw >= 0 && G_I32(0x00507008) == 0) {
        static const char *const k_extra_label[2] = { "None", "Random" };
        int e = g_extra[row - 11];
        SPR_COUNT = g_spr_after_draw;                          /* drop the game's label for this row */
        G_I32(0x00505038) = 0x00452248;                        /* text style used for racer names */
        ((void (__cdecl *)(const char *))0x004116C0)(e < 12 ? (const char *)0x0043F2FC + e * 9 : k_extra_label[e - 12]);
    }
}

static void six_menu_setup(void)
{
    volatile int *r11 = MENU_ROW(11), *r12 = MENU_ROW(12), *r4 = MENU_ROW(4), *r5 = MENU_ROW(5);
    char path[MAX_PATH];
    int k;
    /* rows 6/7 sit 2 px right and 38 px below rows 4/5: continue that upwards */
    r11[0] = r4[0] - 2; r11[1] = r4[1] - 38; r12[0] = r5[0] - 2; r12[1] = r5[1] - 38;
    r11[2] = -1; r11[3] = 4; r11[4] = r4[4]; r11[5] = 12;
    r12[2] = -1; r12[3] = 5; r12[4] = 11;    r12[5] = -1;
    r11[6] = r12[6] = 0; r11[7] = r12[7] = 0x7D + 12;
    r11[8] = r11[0]; r11[9] = r11[1]; r12[8] = r12[0]; r12[9] = r12[1];
    r4[2] = 11; r5[2] = 12;
    /* the main menu's "current = home" loop covers rows 0-12 */
    *(DWORD *)0x00408422 = (DWORD)g_rnew[RT_MENU] + 13 * 0x38 + 0x20;
    snprintf(path, sizeof path, "%smmcda.ini", g_home);
    for (k = 0; k < 2; k++) {
        g_extra[k] = GetPrivateProfileIntA("race", k ? "extra2" : "extra1", EXTRA_RANDOM, path);
        if (g_extra[k] < 0 || g_extra[k] > EXTRA_RANDOM) g_extra[k] = EXTRA_RANDOM;
    }
}

/* ---- race init 0x420700 ---- */
static PFN_VOID0 g_raceinit_orig;
static void __cdecl raceinit_hook(void)
{
    int i, t;
    if (g_six_active) {
        memset(SIX_CAR(4), 0, 2 * 0x460);
        memset(g_rnew[RT_HUD] + 4 * 0x30, 0, 8 * 0x30);
        for (t = 0; t < RELOC_TABLES; t++)
            if (g_rtab[t].elem == 4 && t != RT_RANK && t != RT_BODYMESH && t != RT_SHADMESH &&
                t != RT_BODYFRAME && t != RT_SHADFRAME)
                for (i = 4; i < SIX_MAX; i++) RT_I(t, i) = 0;
    }
    g_raceinit_orig();
    if (g_six_active) {
        for (i = 4; i < SIX_COUNT; i++) *(int *)(g_rnew[RT_HUD] + i * 0x30 + 0xC) = SIX_CHAR(i);
        /* Atlantis (world 2): the game slows CPU cars 1-3 (top speed -100) */
        if (G_I32(0x00634C20) == 0 && *(short *)(*(BYTE **)0x00465638 + 8) == 2)
            for (i = 4; i < SIX_COUNT; i++)
                if (SIX_I(SIX_CAR(i), 0x3E8) == 1) SIX_I(SIX_CAR(i), 0x3D0) -= 100;
    }
}

/* 0x421000(car): racing-line lookahead, called for the 4 records explicitly */
typedef void (__cdecl *PFN_CAR)(BYTE *);
static PFN_CAR g_path_orig;
static void __cdecl path_hook(BYTE *car)
{
    int i;
    g_path_orig(car);
    if (g_six_active && car == SIX_CAR(3))
        for (i = 4; i < SIX_COUNT; i++) g_path_orig(SIX_CAR(i));
}

/* ---- 0x41E660 ranking: progress = checkpoints, capped by lap; bubble sort; place ---- */
static void __cdecl rank_hook(void)
{
    int n = six_n(), prog[SIX_MAX], i, swapped;
    short ncp = **(short **)0x00631A38;
    int k = (ncp + ((ncp >> 31) & 7)) / 8 - 1;   /* same rounding as the game */
    if (k == 0) k = 1;
    for (i = 0; i < n; i++) {
        BYTE *c = SIX_CAR(i);
        int v = SIX_I(c, 0x408), q = v / k, m = SIX_I(c, 0x418) < q ? SIX_I(c, 0x418) : q;
        prog[i] = (m - q) * k + v;
    }
    do {
        swapped = 0;
        for (i = 1; i < n; i++) {
            int a = RT_I(RT_RANK, i - 1), b = RT_I(RT_RANK, i);
            if (a < 0 || a >= n || b < 0 || b >= n) continue;
            if (prog[a] < prog[b] && SIX_I(SIX_CAR(a), 0x420) == 0 && SIX_I(SIX_CAR(b), 0x420) == 0) {
                RT_I(RT_RANK, i - 1) = b; RT_I(RT_RANK, i) = a; swapped = 1;
            }
        }
    } while (swapped);
    for (i = 0; i < n; i++) {
        int c = RT_I(RT_RANK, i);
        if (c >= 0 && c < n) SIX_I(SIX_CAR(c), 0x41C) = i;
    }
}

/* ---- 0x422FF0 car-vs-car collisions ---- */
typedef int (__cdecl *PFN_COLL)(int, int, int);
static void __cdecl collide_hook(void)
{
    PFN_COLL coll = (PFN_COLL)0x00423120;
    int n = six_n(), i, j;
    for (i = 0; i < n - 1; i++)
        for (j = i + 1; j < n; j++) {
            BYTE *a = SIX_CAR(i), *b = SIX_CAR(j);
            if (SIX_I(a, 0x0C) == 0 && SIX_I(b, 0x0C) == 0 && SIX_I(a, 0x20) == 1 && SIX_I(b, 0x20) == 1)
                coll(i, j, 0);
        }
}

/* ---- 0x4240E0 catch-up in a 1-player race ---- */
static void __cdecl catchup_hook(void)
{
    int n = six_n(), i, prev[SIX_MAX], solo = G_I32(0x00634C20) == 0 && G_I32(0x00634C24) == 1;
    int lead = SIX_I(SIX_CAR(0), 0x408);
    for (i = 1; i < n; i++) prev[i] = SIX_I(SIX_CAR(i), 4);
    if (solo)
        for (i = 1; i < n; i++) {
            BYTE *c = SIX_CAR(i);
            if (SIX_I(c, 4) != 0 && SIX_I(c, 0x3D0) < SIX_I(c, 0x3C0) && SIX_I(c, 0) == 1)
                SIX_I(c, 0x3C0) = SIX_I(c, 0x3D0);
        }
    for (i = 0; i < n; i++) SIX_I(SIX_CAR(i), 4) = 0;
    if (solo) {
        for (i = 1; i < n; i++) {
            BYTE *c = SIX_CAR(i);
            int d = lead - SIX_I(c, 0x408);
            if (d >= 4 && SIX_I(c, 0) == 0) SIX_I(c, 4) = 1;
            if (d > 1 && d < 4 && SIX_I(c, 0) == 0) SIX_I(c, 4) = 2;
        }
        for (i = 1; i < n; i++) {
            BYTE *c = SIX_CAR(i);
            if (prev[i] == 1 && SIX_I(c, 4) == 0) SIX_I(c, 0x3C0) = SIX_I(c, 0x3D0);
        }
    }
}

/* ---- 0x420F10 race clock and per-car lap clocks (min / sec / 1/100 ticking by 3) ---- */
static void six_tick_clock(volatile int *mn, volatile int *sc, volatile int *hu)
{
    *hu += 3;
    if (*hu > 0x5A) { *hu = 0; if (++*sc == 0x3C) { *sc = 0; ++*mn; } }
    if (*mn == 10) { *mn = 9; *sc = 0x3B; *hu = 99; }
}
static void __cdecl laptime_hook(void)
{
    int i, n = six_n();
    if (G_I32(0x00634D44)) G_I32(0x00634D44)--;
    if (G_I32(0x00634D1C) == 0 && G_I32(0x00634D14) == 0) {
        six_tick_clock((volatile int *)0x00634D38, (volatile int *)0x00634D3C, (volatile int *)0x00634D40);
        for (i = 0; i < n; i++)
            six_tick_clock(&RT_I(RT_LAPMIN, i), &RT_I(RT_LAPSEC, i), &RT_I(RT_LAPHUN, i));
    }
}

/* ---- finishing rosettes for 5th and 6th ----
 * The race HUD draws a rosette next to a finished car's portrait: sprite *(0x488564) + place
 * (0x402925). Only 1st-4th exist (0x50-0x53); 5th / 6th showed the laurel and "LAP" sprites.
 * Sprites are 4-bit images in the slot table at 0x48F378 (0x410 per slot: header, pixels,
 * 16-colour palette (565 in the low word), width, height). Two new rosettes are built from
 * the 4th one (purple and grey ribbon, digit redrawn) in free slots near the end of the table,
 * and a stub at 0x402925 uses them for places 4 and 5. */
#define SPR_REC(s)   ((BYTE *)0x0048F378 + (s) * 0x410)
#define RIB_SLOT0    0x1C0
static BYTE g_rib_pix[2][16 * 24 / 2];

static WORD rib_tint(WORD v, int k)
{
    int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31, l = (r * 2 + g + b * 2) / 5;   /* 0..~37 */
    l = l * 3 / 2; l = l > 31 ? 31 : l;                                                   /* brighter than the brown */
    if (k == 0) return (WORD)(l << 11 | (l / 2) << 5 | l);                            /* purple */
    return (WORD)(l << 11 | (l * 2) << 5 | l);                                            /* grey */
}

static void six_make_ribbons(void)
{
    static const char *const k_dig[2][6] = {
        { ".9999.", ".9....", ".999..", "....9.", "....9.", ".999.." },   /* 5 */
        { "..99..", ".9....", ".999..", ".9..9.", ".9..9.", "..99.." },   /* 6 */
    };
    BYTE *src = SPR_REC(0x53);
    int k, x, y, base = G_I32(0x00488564);
    g_rib_slot[0] = g_rib_slot[1] = 0;
    if (base != 0x50 || *(int *)(src + 0x408) != 16 || *(int *)(src + 0x40C) != 24 || !*(BYTE **)(src + 4)) return;
    for (k = 0; k < 2; k++) {
        BYTE *dst = SPR_REC(RIB_SLOT0 + k), *pix = g_rib_pix[k];
        if (*(BYTE **)(dst + 4) && *(BYTE **)(dst + 4) != pix) return;   /* slot in use */
        memcpy(pix, *(BYTE **)(src + 4), sizeof g_rib_pix[k]);
        for (y = 0; y < 6; y++)
            for (x = 0; x < 6; x++) {
                BYTE *b = pix + (4 + y) * 8 + (5 + x) / 2;
                int hi = !((5 + x) & 1), v = hi ? *b >> 4 : *b & 15;
                if (v == 9) v = 0xB;                                /* clear the old digit */
                if (k_dig[k][y][x] == '9') v = 9;
                *b = (BYTE)(hi ? (*b & 0x0F) | v << 4 : (*b & 0xF0) | v);
            }
        memcpy(dst, src, 0x410);
        *(BYTE **)(dst + 4) = pix;
        for (x = 3; x <= 8; x++) {                                  /* ribbon colours */
            DWORD *e = (DWORD *)(dst + 8 + x * 4);
            *e = (*e & 0xFFFF0000) | rib_tint((WORD)*e, k);
        }
        g_rib_slot[k] = RIB_SLOT0 + k;
    }
}

static void six_install_ribbon_stub(void)
{
    static const BYTE expect[5] = { 0x6A, 0x14, 0x03, 0xC2, 0x50 };   /* push 14h; add eax,edx; push eax */
    BYTE *st, *site = (BYTE *)0x00402925;
    DWORD old, tbl = (DWORD)g_rib_slot - 16;
    if (memcmp(site, expect, 5) != 0) { log_msg("six: rosette site mismatch"); return; }
    st = (BYTE *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!st) return;
    {
        BYTE c[] = {
            0x83, 0xF8, 0x04,             /* cmp eax,4 */
            0x72, 20,                     /* jb normal */
            0x83, 0xF8, 0x05,             /* cmp eax,5 */
            0x77, 15,                     /* ja normal */
            0x8B, 0x0C, 0x85, 0, 0, 0, 0, /* mov ecx,[eax*4+tbl] */
            0x85, 0xC9,                   /* test ecx,ecx */
            0x74, 4,                      /* jz normal */
            0x89, 0xC8,                   /* mov eax,ecx */
            0xEB, 0x02,                   /* jmp out */
            0x03, 0xC2,                   /* normal: add eax,edx */
            0x6A, 0x14, 0x50,             /* out: push 14h; push eax */
            0xE9, 0, 0, 0, 0 };           /* jmp 0x40292A */
        memcpy(c + 13, &tbl, 4);
        memcpy(st, c, sizeof c);
        *(INT32 *)(st + sizeof c - 4) = (INT32)(0x0040292A - (DWORD)(st + sizeof c));
    }
    VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old);
    site[0] = 0xE9;
    *(INT32 *)(site + 1) = (INT32)(st - (site + 5));
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
}

/* ---- 0x4221A0 race tick: finish order of the extra cars ----
 * When the race ends (0x634D18 == 1 and the 0x634D14 countdown reaches 0) the game writes
 * finish[place of car k] = k for cars 0-3. With 6 cars, the results / points screen
 * (0x40D180) indexes 4-entry roster tables by finish[0..3], so the four roster cars are
 * listed first, in their finishing order, and the extra cars after them. */
static PFN_VOID0 g_tick_orig;
static void __cdecl tick_hook(void)
{
    int pend = G_I32(0x00634D18) == 1 && G_I32(0x00634D14) == 1;
    if (g_six_active && !g_rib_ready) { g_rib_ready = 1; six_make_ribbons(); }
    g_tick_orig();
    if (pend && g_six_active && G_I32(0x00634C20) == 0 && G_I32(0x00634D18) == -1) {
        int order[SIX_MAX], i, k = 0, n = SIX_COUNT;
        for (i = 0; i < SIX_MAX; i++) order[i] = -1;
        for (i = 0; i < n; i++) {
            int p = SIX_I(SIX_CAR(i), 0x41C);
            if (p >= 0 && p < n) order[p] = i;
        }
        for (i = 0; i < n; i++) if (order[i] >= 0 && order[i] < 4) RT_I(RT_FINISH, k++) = order[i];
        for (i = 0; i < n; i++) if (order[i] >= 4) RT_I(RT_FINISH, k++) = order[i];
        log_msg("six: finish order %d %d %d %d %d %d", order[0], order[1], order[2], order[3], order[4], order[5]);
    }
}

/* ---- 0x4017E0 sound load: it overwrites sample-table entry i with car i's character for
 * each car; entries 4-5 are other samples, keep them. */
typedef int (__cdecl *PFN_INT1)(int);
static PFN_INT1 g_sndload_orig;
static int __cdecl sndload_hook(int keep)
{
    int r = g_sndload_orig(keep);
    if (keep == 0 && SIX_COUNT > 4) { G_I32(0x005076D0) = 4; G_I32(0x005076D4) = 5; }
    return r;
}

/* ---- install ---- */
static int six_wrap(DWORD at, const BYTE *pro, size_t n, void *hook, void **orig)
{
    if (memcmp((BYTE *)at, pro, n) != 0) return 0;
    *orig = make_trampoline(pro, n, (BYTE *)at + n);
    return *orig && patch_jmp((BYTE *)at, pro, n, hook);
}

/* start-up result, always written (sixcars.txt next to mmcda.ini) so a launch that comes up
   with 4 cars can be explained without MMCDA_LOG */
static void six_status(const char *fmt, ...)
{
    char path[MAX_PATH], line[256];
    va_list ap;
    FILE *f;
    SYSTEMTIME t;
    va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    log_msg("%s", line);
    snprintf(path, sizeof path, "%ssixcars.txt", g_home);
    if (!(f = fopen(path, "w"))) return;
    GetLocalTime(&t);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d pid %lu\n%s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
            t.wSecond, GetCurrentProcessId(), line);
    fclose(f);
}

static void install_sixcars(void)
{
    char path[MAX_PATH];
    int t, s, bad = 0, ok = 0;
    BYTE *p = g_rpool;
    DWORD old;
    snprintf(path, sizeof path, "%smmcda.ini", g_home);
    g_six_cars = GetPrivateProfileIntA("race", "cars", SIX_MAX, path);   /* default on; cars=4 turns it off */
    if (g_six_cars < 4 || g_six_cars > SIX_MAX) g_six_cars = 4;
    if (g_six_cars == 4) { six_status("six: off ([race] cars=%d read from %s)", g_six_cars, path); return; }

    /* 1. verify every site, then move the tables */
    for (s = 0; s < RELOC_SITES; s++)
        if (*(DWORD *)g_rsite[s].at != g_rsite[s].old) bad++;
    if (bad) { six_status("six: %d relocation sites differ, six cars disabled", bad); g_six_cars = 4; return; }
    for (t = 0; t < RELOC_TABLES; t++) {
        DWORD sz = g_rtab[t].elem * g_rtab[t].newn;
        g_rnew[t] = p;
        memcpy(p, (BYTE *)g_rtab[t].base, g_rtab[t].elem * g_rtab[t].oldn);
        p += (sz + 15) & ~15u;
    }
    VirtualProtect((BYTE *)0x401000, 0x3B200, PAGE_EXECUTE_READWRITE, &old);
    for (s = 0; s < RELOC_SITES; s++) {
        int i = g_rsite[s].tab;
        *(DWORD *)g_rsite[s].at = (DWORD)g_rnew[i] + (g_rsite[s].old - g_rtab[i].base);
    }
    six_menu_setup();
    VirtualProtect((BYTE *)0x401000, 0x3B200, old, &old);
    g_t_bodyframe = (void **)g_rnew[RT_BODYFRAME];
    g_t_shadframe = (void **)g_rnew[RT_SHADFRAME];
    g_t_placed = (volatile int *)g_rnew[RT_PLACED];

    /* 2. weapon aiming 0x41F880: stack arrays [4] at ebp-44/-34/-24 -> [6] at -5C/-44/-2C */
    {
        static const DWORD ecx_sites[] = { 0x41FB14, 0x41FB28, 0x41FB6F, 0x41FB83, 0x41FBCA, 0x41FBDE,
                                           0x41FC2B, 0x41FC3F, 0x41FC82, 0x41FC96 };
        int k, good = 1;
        good &= *(BYTE *)0x41F885 == 0x44 && *(BYTE *)0x41F8A5 == 0xCC && *(BYTE *)0x41F927 == 0xBC &&
                *(BYTE *)0x41F984 == 0xDC;
        for (k = 0; k < 10; k++) {
            BYTE d = *(BYTE *)(ecx_sites[k] + 3);
            good &= *(BYTE *)ecx_sites[k] == 0x8B && (d == 0xDC || d == 0xCC);
        }
        if (good) {
            VirtualProtect((BYTE *)0x41F880, 0x500, PAGE_EXECUTE_READWRITE, &old);
            *(BYTE *)0x41F885 = 0x5C; *(BYTE *)0x41F8A5 = 0xBC; *(BYTE *)0x41F927 = 0xA4; *(BYTE *)0x41F984 = 0xD4;
            for (k = 0; k < 10; k++) {
                BYTE *d = (BYTE *)(ecx_sites[k] + 3);
                *d = *d == 0xDC ? 0xD4 : 0xBC;
            }
            VirtualProtect((BYTE *)0x41F880, 0x500, old, &old);
            ok++;
        } else log_msg("six: weapon-aim frame mismatch");
    }

    /* 2b. lifts / teleporters (0x417790, 0x41AE50): a hazard keeps the cars it has taken in
       4 slots as car index (| 0x8000 for "going down") and reads them back with "and eax,3",
       so car 5 (index 4) moved the player and car 6 moved car 2. Read them with 0x7F, the
       mask the same code uses when it compares slots. */
    {
        static const DWORD sites[8] = { 0x417B0F, 0x417C58, 0x417DC8, 0x417E9E,
                                        0x41B1DB, 0x41B32E, 0x41B4A8, 0x41B57E };
        int k, good = 0;
        VirtualProtect((BYTE *)0x417000, 0x5000, PAGE_EXECUTE_READWRITE, &old);
        for (k = 0; k < 8; k++) {
            BYTE *c = (BYTE *)sites[k];
            if (c[0] == 0x83 && c[1] == 0xE0 && c[2] == 0x03) { c[2] = 0x7F; good++; }
        }
        VirtualProtect((BYTE *)0x417000, 0x5000, old, &old);
        if (good == 8) ok++; else log_msg("six: teleporter mask sites %d/8", good);
    }

    /* 3. hooks */
    {
        static const BYTE ri[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14 };
        static const BYTE pa[7] = { 0x55, 0x8B, 0xEC, 0x51, 0x8B, 0x45, 0x08 };
        static const BYTE tk[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x20 };
        static const BYTE sl[8] = { 0x55, 0x8B, 0xEC, 0xA1, 0xC0, 0x89, 0x48, 0x00 };
        static const BYTE rk[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14 };
        static const BYTE co[5] = { 0xA1, 0xAC, 0x2D, 0x63, 0x00 };
        static const BYTE cu[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10 };
        static const BYTE lt[5] = { 0xA1, 0x44, 0x4D, 0x63, 0x00 };
        static const BYTE mm[5] = { 0xA1, 0xD8, 0x50, 0x50, 0x00 };
        static const BYTE lb[8] = { 0x55, 0x8B, 0xEC, 0xA1, 0x38, 0x50, 0x50, 0x00 };
        static const BYTE ml[8] = { 0x55, 0x8B, 0xEC, 0xA1, 0x38, 0x73, 0x50, 0x00 };
        g_on_raceload = six_prepare;   /* called from debugmenu.h's 0x401070 hook */
        ok++;
        ok += six_wrap(0x420700, ri, 6, raceinit_hook, (void **)&g_raceinit_orig);
        ok += six_wrap(0x421000, pa, 7, path_hook, (void **)&g_path_orig);
        ok += six_wrap(0x4221A0, tk, 6, tick_hook, (void **)&g_tick_orig);
        ok += six_wrap(0x4017E0, sl, 8, sndload_hook, (void **)&g_sndload_orig);
        ok += patch_jmp((BYTE *)0x41E660, rk, 6, rank_hook);
        ok += patch_jmp((BYTE *)0x422FF0, co, 5, collide_hook);
        ok += patch_jmp((BYTE *)0x4240E0, cu, 6, catchup_hook);
        ok += patch_jmp((BYTE *)0x420F10, lt, 5, laptime_hook);
        ok += six_wrap(0x4083F0, mm, 5, mainmenu_hook, (void **)&g_mainmenu_orig);
        ok += six_wrap(0x410CB0, ml, 8, menulist_hook, (void **)&g_menulist_orig);
        ok += patch_jmp((BYTE *)0x411620, lb, 8, label_hook);
        six_install_ribbon_stub();
    }
    six_status("six: up to %d cars in a single race (%d sites moved, %d/14 patches)", g_six_cars, RELOC_SITES, ok);
}
