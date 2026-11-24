/* ==== In-game debug menu (F10) ==========================================================
 * Included by mmcda.c after tiles.h.
 *
 * Text uses the game's own font: FUN_004116c0(text) lays a string out with the per-char
 * width table 0x451be0 and the char->glyph-sprite table 0x4517e0, using the style block
 * *(0x505038) = {fixed advance, x, y, ?, ?, align(0 centre, 1 left, 2 right), ...}, and
 * appends (x, y, glyph) entries to the menu sprite list 0x505ed0 (count 0x505054).
 * FUN_00404c10 then draws that list, mapping each glyph through the loaded-sprite table
 * 0x4868e0 (-1 = not loaded) into the sprite sink FUN_0042ef40(sprite, x, y).
 * In races the menu glyphs may not be loaded; we draw directly through the same sink
 * when they are and log when they aren't.
 *
 * Keyboard: the game reads DirectInput state into 0x54a0c0 (256 bytes) in 0x415360;
 * while the menu or free camera is active we clear that buffer so keys don't drive.
 * Track override: 0x401070 (race loader) reads the track index 0x634c44 into the name
 * table 0x43f040 (16 slots per world, files exist for slots 1-8).
 */
#define TEXT_STYLE_PTR (*(int **)0x00505038)
#define MENU_LIST_N    (*(volatile int *)0x00505054)
#define GLYPH_OF(ch)   (((int *)0x004517E0)[(BYTE)(ch)])
#define WIDTH_OF(ch)   (((int *)0x00451BE0)[(BYTE)(ch)])
#define LOADED_SPRITE  ((int *)0x004868E0)
#define TRACK_INDEX    (*(volatile int *)0x00634C44)

static volatile int g_dbg_open, g_dbg_sel, g_track_override = -1;
static int g_fc_init;                                   /* free camera seeded from game camera */
static int g_dbg_style[8] = { 0, 20, 30, 0, 0, 1, 0, 0 };      /* left-aligned */
static int g_race_font_logged;

static const char *const k_worlds[6] = { "WEST", "ICE", "ATLAN", "NIGHT", "AMAZON", "CITY" };
static const int k_draw_dist[] = { -1, 2, 4, 8, 16, 0 };
#define N_DRAW_DIST 6

/* ---- items ---------------------------------------------------------------------------- */
enum { IT_TRACK, IT_CAMERA, IT_ZOOM, IT_HUD, IT_LAYOUT, IT_DIST, IT_SMOOTH, IT_FILTER, IT_BACKFACE, IT_SAVE, IT_COUNT };

static void track_name(int idx, char *out, size_t n)
{
    if (idx < 0) { snprintf(out, n, "Default"); return; }
    snprintf(out, n, "%s%d", k_worlds[idx / 16], idx % 16 + 1);
}

static void item_text(int it, char *out, size_t n)
{
    char t[32];
    int i;
    switch (it) {
    case IT_TRACK:  track_name(g_track_override, t, sizeof t); snprintf(out, n, "Track       %s", t); break;
    case IT_CAMERA: snprintf(out, n, "Camera      %s", g_cam_mode == 0 ? "Chase" : g_cam_mode == 1 ? "Overview" : "Free fly"); break;
    case IT_ZOOM:   snprintf(out, n, "Zoom        %d.%d", (int)g_zoom, (int)(g_zoom * 10 + 0.5f) % 10); break;
    case IT_HUD:    if (g_hud) snprintf(out, n, "HUD         %dx", g_hud); else snprintf(out, n, "HUD         Fit"); break;
    case IT_LAYOUT: snprintf(out, n, "HUD layout  %s", g_hud_edges ? "Edges" : "Centre"); break;
    case IT_DIST:
        for (i = 0; i < N_DRAW_DIST && k_draw_dist[i] != g_tiles_radius; i++) ;
        if (g_tiles_radius < 0) snprintf(out, n, "Draw dist   All");
        else if (g_tiles_radius == 0) snprintf(out, n, "Draw dist   Original");
        else snprintf(out, n, "Draw dist   %d tiles", g_tiles_radius);
        break;
    case IT_SMOOTH: snprintf(out, n, "Smooth      %s", g_interp_on ? "On" : "Off"); break;
    case IT_FILTER: snprintf(out, n, "Textures    %s", g_vr_filter ? "Smooth" : "Sharp"); break;
    case IT_BACKFACE: snprintf(out, n, "Back faces  %s", g_vr_backfaces ? "Shown" : "Hidden"); break;
    case IT_SAVE:   snprintf(out, n, "Save settings"); break;
    }
}

static void item_change(int it, int dir)
{
    int i;
    switch (it) {
    case IT_TRACK: {                               /* Default, then the 48 tracks with files */
        int t = g_track_override;
        if (t < 0) t = dir > 0 ? 0 : 6 * 16 - 9;
        else {
            t += dir;
            if (t % 16 >= 8) t = dir > 0 ? (t / 16 + 1) * 16 : (t / 16) * 16 + 7;
            if (t < 0 || t >= 6 * 16) t = -1;
        }
        g_track_override = t;
        break; }
    case IT_CAMERA: g_cam_mode = (g_cam_mode + 3 + dir) % 3; g_overview = g_cam_mode == 1; g_fc_init = 0; break;
    case IT_ZOOM:
        g_zoom += 0.1f * dir;
        if (g_zoom < 0.6f) g_zoom = 0.6f;
        if (g_zoom > 3.0f) g_zoom = 3.0f;
        CAM_HEIGHT = 56.0f * g_zoom; apply_far_clip();
        break;
    case IT_HUD: g_hud = (g_hud + 5 + dir) % 5; break;
    case IT_LAYOUT: g_hud_edges = !g_hud_edges; break;
    case IT_DIST:
        for (i = 0; i < N_DRAW_DIST && k_draw_dist[i] != g_tiles_radius; i++) ;
        i = (i + N_DRAW_DIST + dir) % N_DRAW_DIST;
        g_tiles_radius = k_draw_dist[i];
        g_tiles_wc = g_tiles_wr = 0x7fffffff;      /* force a visibility refresh */
        break;
    case IT_SMOOTH: g_interp_on = !g_interp_on; break;
    case IT_FILTER: g_vr_filter = !g_vr_filter; break;
    case IT_BACKFACE: g_vr_backfaces = !g_vr_backfaces; log_msg("debug menu: back faces %s", g_vr_backfaces ? "shown" : "hidden"); break;
    case IT_SAVE: save_zoom(); log_msg("debug menu: settings saved"); break;
    }
}

/* polled from the key thread while the game window has focus */
static void debugmenu_poll(void)
{
    static int prev[256];
    static const int keys[] = { VK_F10, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_ESCAPE };
    int k, i;
    for (i = 0; i < (int)(sizeof keys / sizeof keys[0]); i++) {
        int vk = keys[i], down = (GetAsyncKeyState(vk) & 0x8000) != 0, pressed = down && !prev[vk];
        prev[vk] = down;
        if (!pressed) continue;
        if (vk == VK_F10) { g_dbg_open = !g_dbg_open; log_msg("debug menu %s", g_dbg_open ? "open" : "closed"); continue; }
        if (!g_dbg_open) continue;
        if (vk == VK_ESCAPE) g_dbg_open = 0;
        else if (vk == VK_UP) g_dbg_sel = (g_dbg_sel + IT_COUNT - 1) % IT_COUNT;
        else if (vk == VK_DOWN) g_dbg_sel = (g_dbg_sel + 1) % IT_COUNT;
        else if (vk == VK_LEFT) item_change(g_dbg_sel, -1);
        else if (vk == VK_RIGHT || vk == VK_RETURN) item_change(g_dbg_sel, +1);
    }
    (void)k;
}

/* ---- text output -------------------------------------------------------------------- */
/* front end: through the game's own layout routine into the menu sprite list */
static void menu_text(int x, int y, const char *s)
{
    int *saved = TEXT_STYLE_PTR;
    char buf[64];
    int i;
    /* FUN_004116c0 has no glyph for some punctuation; keep what the font has */
    for (i = 0; s[i] && i < 63; i++) buf[i] = (GLYPH_OF(s[i]) >= 0 || s[i] == ' ') ? s[i] : ' ';
    buf[i] = 0;
    g_dbg_style[1] = x; g_dbg_style[2] = y;
    TEXT_STYLE_PTR = g_dbg_style;
    ((void (__cdecl *)(const char *))0x004116C0)(buf);
    TEXT_STYLE_PTR = saved;
}

/* races: straight into the sprite sink, if the glyphs are loaded */
static int race_font_ready(void)
{
    int g = GLYPH_OF('A');
    return g >= 0 && g < 0x1AB && LOADED_SPRITE[g] >= 0;
}

static void race_text(int x, int y, const char *s)
{
    for (; *s; s++) {
        int g = GLYPH_OF(*s);
        if (*s != ' ' && g >= 0 && g < 0x1AB && LOADED_SPRITE[g] >= 0)
            ((void (__cdecl *)(int, int, int))0x0042EF40)(LOADED_SPRITE[g], x, y);
        x += WIDTH_OF(*s) > 0 ? WIDTH_OF(*s) : 5;
    }
}

/* label and value in separate columns (the font is proportional, so spaces don't align) */
#define MENU_X      16
#define MENU_LABEL  26
#define MENU_VALUE  118
#define MENU_RIGHT  236
static void debugmenu_draw(void (*text)(int, int, const char *))
{
    char line[64], *val;
    int i, y = 24;
    text(MENU_X, y, "DEBUG"); text(MENU_VALUE, y, "F10 close"); y += 16;
    for (i = 0; i < IT_COUNT; i++, y += 13) {
        item_text(i, line, sizeof line);
        val = strstr(line, "  ");                 /* "Label     Value" -> two columns */
        if (val) { *val = 0; val += 2; while (*val == ' ') val++; }
        text(MENU_X, y, i == g_dbg_sel ? ">" : " ");
        text(MENU_LABEL, y, line);
        if (val && *val) text(MENU_VALUE, y, val);
    }
    text(MENU_LABEL, y + 4, "Arrows select / change");
}

/* ---- captured game font (for races, where the font sprites are not loaded) ---------------
 * Sprite records: 0x48f378 + slot*0x410 = { +0 header ptr (byte +0xe: bit depth, <=0x10
 * means 4-bit packed pixels), +4 pixel ptr, +8 palette[256] already in the screen's pixel
 * format, +0x408 width, +0x40c height } (layout from the HUD drawer 0x4075b0).
 * Captured once while the front end has the font loaded. */
#define SPR_REC(slot) ((BYTE *)0x0048F378 + (slot) * 0x410)
struct fglyph { int w, h, adv; BYTE *px; DWORD pal[16 + 240]; };
static struct fglyph g_font[128];
static int g_font_ready, g_font_bpp, g_font_h;

static DWORD conv_px(DWORD v, int from, int to);
static void font_capture(void)
{
    int c, n = 0;
    /* only on the 320-wide front end: in races these slots hold other sprites */
    if (g_font_ready || g_mode_w != 320 || g_race_active || !race_font_ready()) return;
    g_font_bpp = *(volatile int *)0x00504DA0;            /* screen bpp the palettes are in */
    for (c = 33; c < 127; c++) {
        int g = GLYPH_OF(c), slot, w, h, x, y, packed;
        BYTE *rec, *hdr, *src;
        if (g < 0 || g >= 0x1AB || (slot = LOADED_SPRITE[g]) < 0) continue;
        rec = SPR_REC(slot);
        hdr = *(BYTE **)rec; src = *(BYTE **)(rec + 4);
        w = *(int *)(rec + 0x408); h = *(int *)(rec + 0x40C);
        if (!hdr || !src || w <= 0 || h <= 0 || w > 64 || h > 64) continue;
        packed = hdr[0xE] <= 0x10;
        g_font[c].px = (BYTE *)malloc((size_t)w * h);
        if (!g_font[c].px) continue;
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                int i = y * w + x;
                g_font[c].px[i] = packed ? ((x & 1) ? src[i / 2] & 0xF : (src[i / 2] >> 4) & 0xF) : src[i];
            }
        memcpy(g_font[c].pal, rec + 8, sizeof g_font[c].pal);
        g_font[c].w = w; g_font[c].h = h; g_font[c].adv = WIDTH_OF(c);
        if (h > g_font_h) g_font_h = h;
        n++;
    }
    g_font[' '].adv = WIDTH_OF(' ') > 0 ? WIDTH_OF(' ') : 5;
    g_font_ready = n > 20;
    log_msg("debug menu: captured %d font glyphs (%d bpp, height %d)", n, g_font_bpp, g_font_h);
}

/* paint into a locked surface; colours converted between 16-bit 565 and 32-bit if needed */
static DWORD conv_px(DWORD v, int from, int to)
{
    if (from == to) return v;
    if (from == 16 && to == 32)
        return ((v & 0xF800) << 8) | ((v & 0x07E0) << 5) | ((v & 0x001F) << 3);
    if (from == 32 && to == 16)
        return ((v >> 8) & 0xF800) | ((v >> 5) & 0x07E0) | ((v >> 3) & 0x001F);
    return v;
}

struct lockinfo { BYTE *bits; int pitch, w, h, bpp; };

static void put_px(struct lockinfo *lk, int x, int y, DWORD v)
{
    if (x < 0 || y < 0 || x >= lk->w || y >= lk->h) return;
    if (lk->bpp == 16) *(WORD *)(lk->bits + y * lk->pitch + x * 2) = (WORD)v;
    else if (lk->bpp == 32) *(DWORD *)(lk->bits + y * lk->pitch + x * 4) = v;
}

static void darken(struct lockinfo *lk, int x0, int y0, int x1, int y1)
{
    int x, y;
    for (y = max(0, y0); y < min(lk->h, y1); y++)
        for (x = max(0, x0); x < min(lk->w, x1); x++) {
            if (lk->bpp == 16) { WORD *p = (WORD *)(lk->bits + y * lk->pitch + x * 2); *p = (WORD)((*p >> 2) & 0x39E7); }
            else if (lk->bpp == 32) { DWORD *p = (DWORD *)(lk->bits + y * lk->pitch + x * 4); *p = (*p >> 2) & 0x3F3F3F; }
        }
}

static int g_txt_scale;
static struct lockinfo *g_txt_lock;
static void buf_text(int x, int y, const char *s)   /* x, y in 320x240 menu units */
{
    struct lockinfo *lk = g_txt_lock;
    int S = g_txt_scale, ox = x * S, oy = y * S;
    for (; *s; s++) {
        struct fglyph *G = (BYTE)*s < 128 ? &g_font[(BYTE)*s] : NULL;
        if (G && G->px) {
            int gx, gy, sx, sy;
            for (gy = 0; gy < G->h; gy++)
                for (gx = 0; gx < G->w; gx++) {
                    BYTE i = G->px[gy * G->w + gx];
                    DWORD v;
                    if (!i) continue;
                    v = conv_px(G->pal[i], g_font_bpp, lk->bpp);
                    for (sy = 0; sy < S; sy++) for (sx = 0; sx < S; sx++)
                        put_px(lk, ox + gx * S + sx, oy + gy * S + sy, v);
                }
        }
        ox += S * (G && G->adv > 0 ? G->adv : (G && G->w ? G->w + 1 : 5));
    }
}

/* called from hud_blt_hook after the HUD has been drawn onto the race back buffer */
static void debugmenu_overlay(void *backbuf)
{
    BYTE desc[0x6C];
    struct lockinfo lk;
    void **vt;
    if (!g_dbg_open || !backbuf) return;
    if (!g_font_ready) {
        if (!g_race_font_logged) { log_msg("debug menu: font not captured yet (open the menu once on the title screen)"); g_race_font_logged = 1; }
        return;
    }
    vt = *(void ***)backbuf;
    memset(desc, 0, sizeof desc);
    *(DWORD *)desc = sizeof desc;
    if (((HRESULT (WINAPI *)(void *, RECT *, void *, DWORD, HANDLE))vt[25])(backbuf, NULL, desc, 1 /* WAIT */, NULL) != 0) return;
    lk.bits = *(BYTE **)(desc + 0x24); lk.pitch = *(int *)(desc + 0x10);
    lk.h = *(int *)(desc + 0x08); lk.w = *(int *)(desc + 0x0C); lk.bpp = *(int *)(desc + 0x54);
    g_txt_scale = max(1, lk.h / 240);
    g_txt_lock = &lk;
    darken(&lk, 10 * g_txt_scale, 20 * g_txt_scale, MENU_RIGHT * g_txt_scale, (40 + 13 * IT_COUNT + 22) * g_txt_scale);
    debugmenu_draw(buf_text);
    ((HRESULT (WINAPI *)(void *, void *))vt[32])(backbuf, NULL);
}

/* 0x404c10: front-end sprite-list draw. Capture the font, add our lines before the list
 * is consumed. */
static PFN_VOIDINT g_menudraw_orig;
static int __cdecl menudraw_hook(void)
{
    font_capture();
    if (g_dbg_open) debugmenu_draw(menu_text);
    return g_menudraw_orig();
}

/* in races the menu is painted onto the back buffer instead (debugmenu_overlay) */
static void debugmenu_race_tick(void) { }

/* ---- keyboard blocking --------------------------------------------------------------- */
static PFN_VOIDINT g_kbpoll_orig;
static int vr_inject_input(void);   /* vrexport.h: Quest controllers -> keys */
static int pad_inject(void);        /* pad.h */
static int __cdecl kbpoll_hook(void)
{
    int r = g_kbpoll_orig();
    if (g_dbg_open || g_cam_mode == 2) memset((void *)0x0054A0C0, 0, 256);
    else {
        if (vr_inject_input()) r = 1;
        if (pad_inject()) r = 1;           /* pad.h: SDL3 gamepad -> keys */
    }
    return r;
}

/* ---- track override ------------------------------------------------------------------- */
static PFN_VOIDINT g_raceload_orig;
static void (*g_on_raceload)(void);   /* sixcars.h: add extra cars before the track loads */
static int __cdecl raceload_hook(void)
{
    if (g_track_override >= 0) {
        char t[16];
        track_name(g_track_override, t, sizeof t);
        log_msg("debug menu: loading %s instead of track %d", t, TRACK_INDEX);
        TRACK_INDEX = g_track_override;
    }
    if (g_on_raceload) g_on_raceload();
    return g_raceload_orig();
}

/* ---- free-fly camera ------------------------------------------------------------------- */
/* position kept in world units (scene + origin), so the floating origin doesn't drag it */
static float g_fc_x, g_fc_y, g_fc_z, g_fc_yaw, g_fc_pitch;
static double g_fc_last;
static POINT g_fc_mouse;

static void freecam_apply(void *cam, void *scene)
{
    double now = now_ms(), dt = g_fc_last ? (now - g_fc_last) / 1000.0 : 0;
    float fx, fy, fz, rx, rz, speed;
    POINT p;
    if (dt > 0.1) dt = 0.1;
    g_fc_last = now;
    if (!g_fc_init) {                                    /* start where the game camera is */
        MAT4 m;
        g_fr_gettransform(cam, m);
        g_fc_x = m[3][0] + ORIGIN_X; g_fc_y = m[3][1]; g_fc_z = m[3][2] + ORIGIN_Z;
        g_fc_yaw = (float)atan2(m[2][0], m[2][2]);
        g_fc_pitch = (float)asin(max(-1.0f, min(1.0f, m[2][1])));
        g_fc_init = 1;
        GetCursorPos(&g_fc_mouse);
    }
    GetCursorPos(&p);
    if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) {
        g_fc_yaw += (p.x - g_fc_mouse.x) * 0.004f;
        g_fc_pitch -= (p.y - g_fc_mouse.y) * 0.004f;
        if (g_fc_pitch > 1.5f) g_fc_pitch = 1.5f;
        if (g_fc_pitch < -1.5f) g_fc_pitch = -1.5f;
    }
    g_fc_mouse = p;
    fx = (float)(sin(g_fc_yaw) * cos(g_fc_pitch)); fy = (float)sin(g_fc_pitch); fz = (float)(cos(g_fc_yaw) * cos(g_fc_pitch));
    rx = (float)cos(g_fc_yaw); rz = (float)-sin(g_fc_yaw);
    speed = (float)(dt * ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 1600.0 : 400.0));
    if (GetForegroundWindow() == GAME_HWND && !g_dbg_open) {
        if (GetAsyncKeyState('W') & 0x8000) { g_fc_x += fx * speed; g_fc_y += fy * speed; g_fc_z += fz * speed; }
        if (GetAsyncKeyState('S') & 0x8000) { g_fc_x -= fx * speed; g_fc_y -= fy * speed; g_fc_z -= fz * speed; }
        if (GetAsyncKeyState('D') & 0x8000) { g_fc_x += rx * speed; g_fc_z += rz * speed; }
        if (GetAsyncKeyState('A') & 0x8000) { g_fc_x -= rx * speed; g_fc_z -= rz * speed; }
        if (GetAsyncKeyState('E') & 0x8000) g_fc_y += speed;
        if (GetAsyncKeyState('Q') & 0x8000) g_fc_y -= speed;
    }
    g_fr_setposition(cam, (int)(INT_PTR)scene, g_fc_x - ORIGIN_X, g_fc_y, g_fc_z - ORIGIN_Z);
    g_fr_setorientation(cam, scene, fx, fy, fz, 0, 1, 0);
}

/* ---- install ------------------------------------------------------------------------------ */
static void install_debugmenu(void)
{
    static const BYTE md_expect[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C };
    static const BYTE kb_expect[5] = { 0xA1, 0xDC, 0xA1, 0x54, 0x00 };
    static const BYTE rl_expect[6] = { 0x8B, 0x0D, 0xC4, 0xAE, 0x54, 0x00 };
    int ok = 0;
    {   /* testing: MMCDA_TRACK=<index> pre-selects the track override (world*16 + track-1) */
        char v[8];
        if (GetEnvironmentVariableA("MMCDA_TRACK", v, sizeof v)) g_track_override = atoi(v);
    }
    if (memcmp((BYTE *)0x00404C10, md_expect, 6) == 0 &&
        (g_menudraw_orig = (PFN_VOIDINT)make_trampoline(md_expect, 6, (BYTE *)0x00404C16)) &&
        patch_jmp((BYTE *)0x00404C10, md_expect, 6, menudraw_hook)) ok |= 1;
    if (memcmp((BYTE *)0x00415360, kb_expect, 5) == 0 &&
        (g_kbpoll_orig = (PFN_VOIDINT)make_trampoline(kb_expect, 5, (BYTE *)0x00415365)) &&
        patch_jmp((BYTE *)0x00415360, kb_expect, 5, kbpoll_hook)) ok |= 2;
    if (memcmp((BYTE *)0x00401070, rl_expect, 6) == 0 &&
        (g_raceload_orig = (PFN_VOIDINT)make_trampoline(rl_expect, 6, (BYTE *)0x00401076)) &&
        patch_jmp((BYTE *)0x00401070, rl_expect, 6, raceload_hook)) ok |= 4;
    log_msg("debug menu hooks: menu draw %s, keyboard %s, track override %s",
            ok & 1 ? "ok" : "MISSING", ok & 2 ? "ok" : "MISSING", ok & 4 ? "ok" : "MISSING");
}
