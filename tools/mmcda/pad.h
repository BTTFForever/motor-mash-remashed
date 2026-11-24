/* ==== Native gamepad support (SDL3) + controller setup screen ==========================
 * Included by mmcda.c after debugmenu.h.
 *
 * SDL3.dll is loaded at run time (game folder / MMCDA_HOME). A background thread keeps the
 * chosen gamepad's state current; SDL's HIDAPI Steam driver also takes the 2026 Steam
 * Controller (receiver 28de:1304) out of keyboard/mouse "lizard mode" without Steam.
 * The keyboard-poll hook (0x415360) turns the state into the game's player-1 keys in the
 * DirectInput buffer 0x54a0c0: arrows, Enter (fire / select), P, Esc.
 *
 * The front end's Calibrate option called 0x413430, which starts joy.cpl and waits for it
 * (it opens behind the full-screen game, so the game just looked frozen). That function
 * now runs our controller setup screen instead. Settings: mmcda.ini [pad].
 */

/* ---- SDL3, loaded dynamically -------------------------------------------------------- */
typedef unsigned int SDL_JoystickID;
typedef struct SDL_Gamepad SDL_Gamepad;
#define SDL_INIT_GAMEPAD 0x00002000u
#define PAD_NAXES 6                 /* LX LY RX RY LT RT (SDL_GamepadAxis order) */
#define PAD_NBUTTONS 26             /* SDL_GamepadButton count in SDL 3.4 */

static struct {
    HMODULE dll;
    unsigned char (__cdecl *SetHint)(const char *, const char *);
    unsigned char (__cdecl *Init)(unsigned);
    SDL_JoystickID *(__cdecl *GetGamepads)(int *);
    SDL_Gamepad *(__cdecl *OpenGamepad)(SDL_JoystickID);
    void (__cdecl *CloseGamepad)(SDL_Gamepad *);
    unsigned char (__cdecl *GamepadConnected)(SDL_Gamepad *);
    const char *(__cdecl *GetGamepadNameForID)(SDL_JoystickID);
    short (__cdecl *GetGamepadAxis)(SDL_Gamepad *, int);
    unsigned char (__cdecl *GetGamepadButton)(SDL_Gamepad *, int);
    void (__cdecl *UpdateGamepads)(void);
    void (__cdecl *PumpEvents)(void);
    void (__cdecl *FlushEvents)(unsigned, unsigned);
    void (__cdecl *Free)(void *);
    const char *(__cdecl *GetError)(void);
    char *(__cdecl *GetGamepadMapping)(SDL_Gamepad *);
} sdl;

static int pad_load_sdl(void)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%sSDL3.dll", g_home);
    sdl.dll = LoadLibraryA(path);
    if (!sdl.dll) sdl.dll = LoadLibraryA("SDL3.dll");
    if (!sdl.dll) { log_msg("pad: SDL3.dll not found, controller support off"); return 0; }
#define SDLF(f, n) if (!(*(FARPROC *)&sdl.f = GetProcAddress(sdl.dll, n))) { log_msg("pad: SDL3.dll lacks %s", n); return 0; }
    SDLF(SetHint, "SDL_SetHint") SDLF(Init, "SDL_Init") SDLF(GetGamepads, "SDL_GetGamepads")
    SDLF(OpenGamepad, "SDL_OpenGamepad") SDLF(CloseGamepad, "SDL_CloseGamepad")
    SDLF(GamepadConnected, "SDL_GamepadConnected") SDLF(GetGamepadNameForID, "SDL_GetGamepadNameForID")
    SDLF(GetGamepadAxis, "SDL_GetGamepadAxis") SDLF(GetGamepadButton, "SDL_GetGamepadButton")
    SDLF(UpdateGamepads, "SDL_UpdateGamepads") SDLF(PumpEvents, "SDL_PumpEvents")
    SDLF(FlushEvents, "SDL_FlushEvents") SDLF(Free, "SDL_free") SDLF(GetError, "SDL_GetError")
    SDLF(GetGamepadMapping, "SDL_GetGamepadMapping")
#undef SDLF
    return 1;
}

/* ---- shared state (pad thread writes, game thread reads) ----------------------------- */
#define PAD_MAXDEV 8
static struct {
    volatile LONG seq;                      /* bumps every update */
    int ndev;
    char names[PAD_MAXDEV][64];
    SDL_JoystickID ids[PAD_MAXDEV];
    volatile int active;                    /* index into ids, -1 none */
    float axis[PAD_NAXES];                  /* -1..1 (triggers 0..1) */
    unsigned char button[PAD_NBUTTONS];
} g_pad = { 0, 0, { { 0 } }, { 0 }, -1 };
static volatile int g_pad_want = -1;        /* setup screen asks the thread to switch */
static char g_pad_pref[64];                 /* preferred device name (ini) */

/* ---- bindings ------------------------------------------------------------------------ */
enum { ACT_ACCEL, ACT_BRAKE, ACT_LEFT, ACT_RIGHT, ACT_FIRE, ACT_ACTION, ACT_PAUSE, ACT_COUNT };
static const char *const k_act_names[ACT_COUNT] = { "Accelerate", "Brake", "Steer left", "Steer right", "Fire", "Dive (Atlantis)", "Pause" };
static const char *const k_act_keys[ACT_COUNT] = { "accelerate", "brake", "left", "right", "fire", "action", "pause" };
static const BYTE k_act_dik[ACT_COUNT] = { 0xC8, 0xD0, 0xCB, 0xCD, 0x1C, 0x19, 0x01 };
/* source codes: 0 none; 1 + button; 100 + axis * 2 (+1 for the negative direction) */
#define SRC_BTN(b)       (1 + (b))
#define SRC_AXIS(a, neg) (100 + (a) * 2 + ((neg) ? 1 : 0))
static int g_bind[ACT_COUNT];
static const int k_bind_default[ACT_COUNT] = {
    SRC_AXIS(5, 0), SRC_AXIS(4, 0), SRC_AXIS(0, 1), SRC_AXIS(0, 0),   /* RT, LT, left stick */
    SRC_BTN(0), SRC_BTN(2), SRC_BTN(1) };                             /* A, X, B */
static int g_pad_deadzone = 15;             /* percent */
static int g_pad_steer_analog = 1, g_pad_throttle_analog = 0;
/* analog amounts for the car hook (car.h): the game still sees a held key, and the
   hook scales the car's turn rate / engine power by these */
static float g_an_steer, g_an_accel, g_an_brake;
static volatile int g_an_steer_on, g_an_throttle_on;
static int g_car_lean = 100;                 /* percent of the original body lean (car.h) */
static int g_car_lowered = 1;                /* ride height: lowered onto the road (car.h) */

static const char *const k_btn_names[PAD_NBUTTONS] = {
    "A", "B", "X", "Y", "Back", "Guide", "Start", "L stick", "R stick", "LB", "RB",
    "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Misc", "R paddle 1", "L paddle 1",
    "R paddle 2", "L paddle 2", "Touchpad", "Misc 2", "Misc 3", "Misc 4", "Misc 5", "Misc 6" };
static const char *const k_axis_names[PAD_NAXES * 2] = {
    "L stick right", "L stick left", "L stick down", "L stick up", "R stick right", "R stick left",
    "R stick down", "R stick up", "L trigger", "L trigger", "R trigger", "R trigger" };

static const char *src_name(int s)
{
    if (s >= 1 && s <= PAD_NBUTTONS) return k_btn_names[s - 1];
    if (s >= 100 && s < 100 + PAD_NAXES * 2) return k_axis_names[s - 100];
    return "-";
}

/* 0..1 strength of a source, dead zone applied to sticks and triggers */
static float src_value(int s)
{
    float v, dz = g_pad_deadzone / 100.0f;
    if (g_pad.active < 0) { g_an_steer_on = g_an_throttle_on = 0; return 0; }
    if (s >= 1 && s <= PAD_NBUTTONS) return g_pad.button[s - 1] ? 1.0f : 0.0f;
    if (s < 100 || s >= 100 + PAD_NAXES * 2) return 0;
    v = g_pad.axis[(s - 100) / 2];
    if ((s - 100) & 1) v = -v;
    if (v <= dz) return 0;
    return min(1.0f, (v - dz) / (1.0f - dz));
}

static void pad_load_settings(void)
{
    char path[MAX_PATH], v[64];
    int a;
    snprintf(path, sizeof path, "%smmcda.ini", g_home);
    GetPrivateProfileStringA("pad", "device", "", g_pad_pref, sizeof g_pad_pref, path);
    g_pad_deadzone = GetPrivateProfileIntA("pad", "deadzone", 15, path);
    if (g_pad_deadzone < 0 || g_pad_deadzone > 60) g_pad_deadzone = 15;
    GetPrivateProfileStringA("pad", "steering", "analog", v, sizeof v, path);
    g_pad_steer_analog = lstrcmpiA(v, "digital") != 0;
    GetPrivateProfileStringA("pad", "throttle", "analog", v, sizeof v, path);
    g_pad_throttle_analog = lstrcmpiA(v, "analog") == 0;
    for (a = 0; a < ACT_COUNT; a++) g_bind[a] = GetPrivateProfileIntA("pad", k_act_keys[a], k_bind_default[a], path);
}

static void car_save_settings(void);   /* car.h */
static void pad_save_settings(void)
{
    car_save_settings();
    char path[MAX_PATH], v[16];
    int a;
    snprintf(path, sizeof path, "%smmcda.ini", g_home);
    if (g_pad.active >= 0) WritePrivateProfileStringA("pad", "device", g_pad.names[g_pad.active], path);
    snprintf(v, sizeof v, "%d", g_pad_deadzone); WritePrivateProfileStringA("pad", "deadzone", v, path);
    WritePrivateProfileStringA("pad", "steering", g_pad_steer_analog ? "analog" : "digital", path);
    WritePrivateProfileStringA("pad", "throttle", g_pad_throttle_analog ? "analog" : "digital", path);
    for (a = 0; a < ACT_COUNT; a++) { snprintf(v, sizeof v, "%d", g_bind[a]); WritePrivateProfileStringA("pad", k_act_keys[a], v, path); }
}

/* ---- pad thread ---------------------------------------------------------------------- */
static DWORD WINAPI pad_thread(LPVOID p)
{
    SDL_Gamepad *gp = NULL;
    DWORD last_scan = 0;
    int i;
    (void)p;
    if (!pad_load_sdl()) return 0;
    sdl.SetHint("SDL_JOYSTICK_HIDAPI_STEAM", "1");                 /* Steam Controller w/o Steam */
    sdl.SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    if (!sdl.Init(SDL_INIT_GAMEPAD)) { log_msg("pad: SDL_Init failed: %s", sdl.GetError()); return 0; }
    log_msg("pad: SDL3 ready");
    for (;;) {
        DWORD now = GetTickCount();
        sdl.PumpEvents();
        sdl.FlushEvents(0, 0xFFFFFFFFu);
        sdl.UpdateGamepads();
        if (now - last_scan > 1000 || g_pad_want >= 0 || (gp && !sdl.GamepadConnected(gp))) {
            int n = 0, pick = -1;
            SDL_JoystickID *ids = sdl.GetGamepads(&n), cur = gp && g_pad.active >= 0 ? g_pad.ids[g_pad.active] : 0;
            last_scan = now;
            if (n > PAD_MAXDEV) n = PAD_MAXDEV;
            for (i = 0; i < n; i++) {
                const char *nm = sdl.GetGamepadNameForID(ids[i]);
                g_pad.ids[i] = ids[i];
                lstrcpynA(g_pad.names[i], nm ? nm : "Gamepad", sizeof g_pad.names[i]);
            }
            if (n != g_pad.ndev) log_msg("pad: %d gamepad(s)%s%s", n, n ? ", first: " : "", n ? g_pad.names[0] : "");
            g_pad.ndev = n;
            if (ids) sdl.Free(ids);
            if (g_pad_want >= 0 && g_pad_want < n) pick = g_pad_want;
            else {
                for (i = 0; i < n && pick < 0; i++) if (gp && g_pad.ids[i] == cur && sdl.GamepadConnected(gp)) pick = i;
                for (i = 0; i < n && pick < 0; i++) if (g_pad_pref[0] && !lstrcmpA(g_pad.names[i], g_pad_pref)) pick = i;
                if (pick < 0 && n > 0) pick = 0;
            }
            g_pad_want = -1;
            if (pick < 0 || !gp || g_pad.ids[pick] != cur || !sdl.GamepadConnected(gp)) {
                if (gp) { sdl.CloseGamepad(gp); gp = NULL; }
                if (pick >= 0) gp = sdl.OpenGamepad(g_pad.ids[pick]);
                g_pad.active = gp ? pick : -1;
                if (gp) {
                    char *mp = sdl.GetGamepadMapping(gp);
                    log_msg("pad: using %s", g_pad.names[pick]);
                    if (mp) { log_msg("pad: mapping %s", mp); sdl.Free(mp); }
                }
            }
        }
        if (gp) {
            for (i = 0; i < PAD_NAXES; i++) {
                float v = sdl.GetGamepadAxis(gp, i) / 32767.0f;
                g_pad.axis[i] = v < -1 ? -1 : v;
            }
            for (i = 0; i < PAD_NBUTTONS; i++) {
                /* Misc 3-6 are the 2026 Steam Controller's stick/grip touch sensors: ignored */
                unsigned char b = (i >= 22 && i <= 25) ? 0 : (sdl.GetGamepadButton(gp, i) ? 1 : 0);
                static int nlog;
                if (b != g_pad.button[i] && nlog < 400) { nlog++; log_msg("pad: button %d (%s) %s", i, k_btn_names[i], b ? "down" : "up"); }
                g_pad.button[i] = b;
            }
        } else {
            memset(g_pad.axis, 0, sizeof g_pad.axis); memset(g_pad.button, 0, sizeof g_pad.button);
        }
        InterlockedIncrement(&g_pad.seq);
        Sleep(4);
    }
}

/* ---- gamepad -> game keys (keyboard-poll hook, once per game loop) ------------------ */
static volatile int g_pad_setup_open;
static volatile int g_pad_esc_pulse;
static int g_cal_inpage;                 /* opened from the Calibrate page, which stays up underneath */
static int g_cal_leave;                  /* closing: leave the Calibrate page when no key is held */
static volatile DWORD g_joy_race_poll;       /* last time a race read Joystick 1 */
static int pad_inject(void)
{
    static float acc[ACT_COUNT];
    BYTE *k = (BYTE *)0x0054A0C0;
    int a, any = 0;
    /* DirectInput sees no joystick once lizard mode is off, so the start-up control choice
       (count + 2 options, 0x40d3c0) would only offer the two keyboards: report one joystick
       while an SDL controller is connected. Joystick 1 reads it (joyread_hook); the device
       slots stay empty, which the game's own joystick code skips. */
    if (g_pad.active >= 0 && *(volatile int *)0x0054A1C8 == 0) {
        *(volatile int *)0x0054A1C8 = 1;
        log_msg("pad: offering the SDL controller as the game's Joystick 1");
    }
    if (g_pad_setup_open) { memset(k, 0, 256); return 1; }      /* setup screen owns the input */
    if (g_cal_leave) {
        /* the key that closed the setup screen must not reach the options page as well */
        int i, held = g_pad.button[0] || g_pad.button[1] || g_pad.button[6];
        for (i = 0; i < 256; i++) if (k[i] & 0x80) held = 1;
        memset(k, 0, 256);
        if (!held) { *(volatile int *)0x005072F4 = 2; g_cal_leave = 0; }   /* as the page did after joy.cpl */
        return 1;
    }
    if (g_pad_esc_pulse > 0) {
        /* first wait for the key that closed the setup screen to be let go, so it does not
           reach the page as well; then one Esc of our own */
        int i, held = 0;
        for (i = 0; i < 256; i++) if (k[i] & 0x80) held = 1;
        if (g_pad.button[0] || g_pad.button[1] || g_pad.button[6]) held = 1;   /* A / B / Start */
        memset(k, 0, 256);
        if (held && g_pad_esc_pulse == 3) return 1;
        g_pad_esc_pulse--;
        if (g_pad_esc_pulse == 1) k[0x01] = 0x80;
        return 1;
    }
    if (g_pad.active < 0) return 0;
    /* The controller drives only through the game's Joystick 1 (joyread_hook): the purple
       and pink keyboard choices stay keyboard-only, as in the original. The menus read
       Joystick 1 too, whatever was chosen. Only Esc comes from the keyboard in this game
       (pause in races, back in menus), so ☰ / the Pause binding, and B in menus, send Esc. */
    (void)acc; (void)a;
    if (src_value(g_bind[ACT_PAUSE]) > 0.5f || g_pad.button[6] || (!g_race_active && g_pad.button[1])) {
        k[0x01] = 0x80; any = 1;
    }
    /* analog amounts for car.h; only used while a race reads Joystick 1 (recent poll) */
    g_an_steer = src_value(g_bind[ACT_RIGHT]) - src_value(g_bind[ACT_LEFT]);
    g_an_accel = src_value(g_bind[ACT_ACCEL]);
    g_an_brake = src_value(g_bind[ACT_BRAKE]);
    {
        int joy = g_race_active && GetTickCount() - g_joy_race_poll < 250;
        g_an_steer_on = joy && g_pad_steer_analog;
        g_an_throttle_on = joy && g_pad_throttle_analog;
    }
    return any;
}

/* ---- controller setup screen (replaces 0x413430, the joy.cpl launcher) --------------- */
enum { ROW_DEVICE, ROW_BIND0, ROW_DEADZONE = ROW_BIND0 + ACT_COUNT, ROW_STEER, ROW_THROTTLE, ROW_LEAN, ROW_RIDE, ROW_SPEED, ROW_TRACKS, ROW_DEFAULTS, ROW_DONE, ROW_COUNT };

static void fill_rect(struct lockinfo *lk, int x0, int y0, int x1, int y1, DWORD rgb)
{
    DWORD v = lk->bpp == 16 ? conv_px(rgb, 32, 16) : rgb;
    int x, y;
    for (y = y0; y < y1; y++) for (x = x0; x < x1; x++) put_px(lk, x, y, v);
}

/* input for the screen: gamepad (any button) or keyboard (window focused) */
struct uiin { int up, down, left, right, ok, back; };
static void ui_read(struct uiin *in)
{
    HWND fg = GetForegroundWindow();
    int kb = fg == *(HWND *)0x006172A8;
    float lx = g_pad.active >= 0 ? g_pad.axis[0] : 0, ly = g_pad.active >= 0 ? g_pad.axis[1] : 0;
    in->up    = g_pad.button[11] || ly < -0.6f || (kb && (GetAsyncKeyState(VK_UP) & 0x8000));
    in->down  = g_pad.button[12] || ly > 0.6f || (kb && (GetAsyncKeyState(VK_DOWN) & 0x8000));
    in->left  = g_pad.button[13] || lx < -0.6f || (kb && (GetAsyncKeyState(VK_LEFT) & 0x8000));
    in->right = g_pad.button[14] || lx > 0.6f || (kb && (GetAsyncKeyState(VK_RIGHT) & 0x8000));
    in->ok    = g_pad.button[0] || (kb && (GetAsyncKeyState(VK_RETURN) & 0x8000));
    in->back  = g_pad.button[1] || g_pad.button[6] || (kb && (GetAsyncKeyState(VK_ESCAPE) & 0x8000));
}

/* the first source that is newly active (for binding); 0 if none */
static int pad_capture(const unsigned char *btn0, const float *axis0)
{
    int i;
    /* skip Misc 3-6: on the 2026 Steam Controller these are the stick and grip touch
       sensors, which fire as soon as a thumb rests on a stick (before the real press) */
    for (i = 0; i < PAD_NBUTTONS; i++) if (g_pad.button[i] && !btn0[i] && (i < 22 || i > 25)) return SRC_BTN(i);
    for (i = 0; i < PAD_NAXES; i++) {
        float v = g_pad.axis[i];
        if (v > 0.6f && axis0[i] < 0.3f) return SRC_AXIS(i, 0);
        if (v < -0.6f && axis0[i] > -0.3f) return SRC_AXIS(i, 1);
    }
    return 0;
}

/* A copy of a loaded sprite in 32-bit RGB (0 = transparent): the page background (slot 1,
   320x256, reloaded by the game for each page) and the logo, taken when Calibrate is chosen,
   while the options page is still loaded. */
struct spcopy { int w, h; DWORD *px; };
static struct spcopy g_page_bg, g_page_logo;
static void sprite_copy(struct spcopy *c, int slot)
{
    BYTE *rec, *hdr, *src;
    int w, h, x, y, packed;
    if (slot < 0) return;
    rec = SPR_REC(slot); hdr = *(BYTE **)rec; src = *(BYTE **)(rec + 4);
    w = *(int *)(rec + 0x408); h = *(int *)(rec + 0x40C);
    if (!hdr || !src || w <= 0 || h <= 0 || w > 640 || h > 512) return;
    free(c->px);
    if (!(c->px = (DWORD *)malloc((size_t)w * h * 4))) { c->w = c->h = 0; return; }
    packed = hdr[0xE] <= 0x10;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            int i = y * w + x, k = packed ? ((x & 1) ? src[i / 2] & 0xF : (src[i / 2] >> 4) & 0xF) : src[i];
            DWORD v = k ? conv_px(((DWORD *)(rec + 8))[k], *(volatile int *)0x00504DA0, 32) & 0xFFFFFF : 0;
            c->px[i] = k ? (v | 0xFF000000u) : 0;
        }
    c->w = w; c->h = h;
}

static void sprite_draw(struct lockinfo *lk, const struct spcopy *c, int x0, int y0)
{
    int x, y;
    for (y = 0; y < c->h; y++)
        for (x = 0; x < c->w; x++) {
            DWORD v = c->px[y * c->w + x];
            if (v) put_px(lk, x0 + x, y0 + y, conv_px(v & 0xFFFFFF, 32, lk->bpp));
        }
}

/* text at any size: x, y in 320x240 menu units, sc = menu units per font pixel */
static float g_ui_k;   /* back-buffer pixels per menu unit */
static int ui_text_w(const char *s, float sc)
{
    float w = 0;
    for (; *s; s++) {
        struct fglyph *G = (BYTE)*s < 128 ? &g_font[(BYTE)*s] : NULL;
        w += sc * (G && G->adv > 0 ? G->adv : (G && G->w ? G->w + 1 : 5));
    }
    return (int)(w + 0.5f);
}

static void ui_text(struct lockinfo *lk, float x, float y, const char *s, float sc)
{
    float k = g_ui_k * sc;                       /* back-buffer pixels per font pixel */
    for (; *s; s++) {
        struct fglyph *G = (BYTE)*s < 128 ? &g_font[(BYTE)*s] : NULL;
        if (G && G->px) {
            int x0 = (int)(x * g_ui_k), y0 = (int)(y * g_ui_k);
            int dw = (int)(G->w * k + 0.5f), dh = (int)(G->h * k + 0.5f), dx, dy;
            for (dy = 0; dy < dh; dy++)
                for (dx = 0; dx < dw; dx++) {
                    int gx = (int)(dx / k), gy = (int)(dy / k);
                    BYTE i;
                    if (gx >= G->w || gy >= G->h || !(i = G->px[gy * G->w + gx])) continue;
                    put_px(lk, x0 + dx, y0 + dy, conv_px(G->pal[i], g_font_bpp, lk->bpp));
                }
        }
        x += sc * (G && G->adv > 0 ? G->adv : (G && G->w ? G->w + 1 : 5));
    }
}

/* the menus' red ball marker: a shaded sphere, r in menu units */
static void ui_ball(struct lockinfo *lk, float cx, float cy, float r)
{
    int bx0 = (int)(cx * g_ui_k), by0 = (int)(cy * g_ui_k), rad = (int)(r * g_ui_k + 0.5f), x, y;
    for (y = -rad; y <= rad; y++)
        for (x = -rad; x <= rad; x++) {
            float d = (float)(x * x + y * y) / (rad * rad), hl;
            int rr, gg, bb;
            if (d > 1) continue;
            hl = (float)((x + rad / 3) * (x + rad / 3) + (y + rad / 3) * (y + rad / 3)) / (rad * rad);   /* highlight, top left */
            rr = (int)(230 - 120 * d); gg = bb = (int)(30 - 20 * d);
            if (hl < 0.12f) { rr = 255; gg = bb = (int)(255 - hl * 1500); if (gg < 60) gg = bb = 60; }
            put_px(lk, bx0 + x, by0 + y, conv_px(((DWORD)rr << 16) | (gg << 8) | bb, 32, lk->bpp));
        }
}

/* Laid out like the game's option pages (e.g. Display): the logo at the top, rows with the
   red ball on the selected one and values on the right, the page's name in a box below. */
static void setup_draw(struct lockinfo *lk, const BYTE *bg, int sel, int capturing)
{
    /* the menus run at 320x240 (one font pixel per pixel): the rows scroll, 8 at a time */
    /* over the Calibrate page the rows start below its joystick icon: 7 at a time */
    const int VIS = bg ? 8 : 7;
    const float y0 = bg ? 62 : 82, step = 16, sc = 1, vx = 300;
    static int top;
    int r, i, bpp8 = lk->bpp / 8;
    char v[64];
    if (sel < top) top = sel;
    if (sel >= top + VIS) top = sel - VIS + 1;
    if (top > ROW_COUNT - VIS) top = ROW_COUNT - VIS;
    if (top < 0) top = 0;
    g_ui_k = lk->h / 240.0f;
    if (!bg) ;   /* over the game's Calibrate page as it is */
    else if (g_page_bg.px && g_page_bg.w == 320 && lk->w == 320) {   /* the options page's own background and logo */
        int y;
        for (y = 0; y < lk->h && y < g_page_bg.h; y++) {
            int x;
            for (x = 0; x < 320; x++) put_px(lk, x, y, conv_px(g_page_bg.px[y * 320 + x] & 0xFFFFFF, 32, lk->bpp));
        }
        if (g_page_logo.px) sprite_draw(lk, &g_page_logo, 95, -2);
    } else {   /* the menu behind, dimmed; the logo kept as it is */
        int lx0 = (int)(92 * g_ui_k), lx1 = (int)(228 * g_ui_k), ly1 = (int)(60 * g_ui_k);
        for (i = 0; i < lk->h; i++) memcpy(lk->bits + i * lk->pitch, bg + (size_t)i * lk->pitch, (size_t)lk->w * bpp8);
        darken(lk, 0, 0, lk->w, ly1);
        darken(lk, 0, ly1, lk->w, lk->h);
        for (i = 0; i < ly1 && i < lk->h; i++)
            memcpy(lk->bits + i * lk->pitch + lx0 * bpp8, bg + (size_t)i * lk->pitch + lx0 * bpp8, (size_t)(lx1 - lx0) * bpp8);
    }
    for (r = top; r < top + VIS && r < ROW_COUNT; r++) {
        float y = y0 + (r - top) * step;
        const char *label = "", *val = "";
        if (r == sel) ui_ball(lk, 14, y + 7, 4);
        if (r == ROW_DEVICE) {
            label = "Device";
            val = g_pad.active >= 0 ? g_pad.names[g_pad.active] : (g_pad.ndev ? "-" : "None found");
        } else if (r >= ROW_BIND0 && r < ROW_BIND0 + ACT_COUNT) {
            label = k_act_names[r - ROW_BIND0];
            val = capturing && r == sel ? "Press a button..." : src_name(g_bind[r - ROW_BIND0]);
        } else if (r == ROW_DEADZONE) { label = "Dead zone"; snprintf(v, sizeof v, "%d%%", g_pad_deadzone); val = v; }
        else if (r == ROW_STEER) { label = "Steering"; val = g_pad_steer_analog ? "Analog" : "Digital"; }
        else if (r == ROW_THROTTLE) { label = "Throttle"; val = g_pad_throttle_analog ? "Analog" : "Digital"; }
        else if (r == ROW_LEAN) { label = "Body lean"; snprintf(v, sizeof v, "%d%%", g_car_lean); val = v; }
        else if (r == ROW_RIDE) { label = "Ride height"; val = g_car_lowered ? "Lowered" : "Original"; }
        else if (r == ROW_TRACKS) { label = "Tracks"; val = g_unlock_all ? "All unlocked" : "Normal (earn them)"; }
        else if (r == ROW_SPEED) { label = "Game speed"; val = g_game_speed == 1 ? "Easy (20)" : g_game_speed == 2 ? "Medium (25)" : "Hard (30)"; }
        else if (r == ROW_DEFAULTS) label = "Reset to defaults";
        else if (r == ROW_DONE) label = "Save and exit";
        ui_text(lk, 24, y, label, sc);
        if (*val) ui_text(lk, vx - ui_text_w(val, sc), y, val, sc);
    }
    /* more rows above / below: small blue arrows at the left, as the game's arrows */
    for (i = 0; i < 2; i++) {
        int show = i == 0 ? top > 0 : top + VIS < ROW_COUNT, ty = i == 0 ? 55 : (int)(y0 + VIS * step + 1), d;
        if (!show) continue;
        for (d = 0; d < 5; d++)
            fill_rect(lk, (int)((14 - d) * g_ui_k), (int)((ty + (i == 0 ? d : 4 - d)) * g_ui_k),
                      (int)((15 + d) * g_ui_k), (int)((ty + (i == 0 ? d : 4 - d) + 1) * g_ui_k), 0x2040F0);
    }
    if (bg) {   /* the page's name in a box, as the game's pages have */
        const char *t = "Controller Setup";
        float bx0 = 86, bx1 = 234, by0 = 199, by1 = 234;
        int x0 = (int)(bx0 * g_ui_k), x1 = (int)(bx1 * g_ui_k), y0b = (int)(by0 * g_ui_k), y1b = (int)(by1 * g_ui_k);
        int t1 = max(1, (int)(g_ui_k + 0.5f));
        if (!g_page_bg.px) {   /* the page background has its own box */
            darken(lk, x0, y0b, x1, y1b);
            fill_rect(lk, x0, y0b, x1, y0b + t1, 0x000000);
            fill_rect(lk, x0, y0b, x0 + t1, y1b, 0x000000);
            fill_rect(lk, x0, y1b - t1, x1, y1b, 0x2A5A20);
            fill_rect(lk, x1 - t1, y0b, x1, y1b, 0x2A5A20);
        }
        ui_text(lk, (bx0 + bx1) / 2 - ui_text_w(t, 1) / 2.0f, by0 + 9, t, 1);
    }
    /* live view: steering and throttle/brake after dead zone, at the top right */
    {
        int S = (int)(g_ui_k + 0.5f);
        float st = src_value(g_bind[ACT_RIGHT]) - src_value(g_bind[ACT_LEFT]);
        float ac = src_value(g_bind[ACT_ACCEL]), br = src_value(g_bind[ACT_BRAKE]);
        int bx = (int)(244 * g_ui_k), by = (int)(10 * g_ui_k), bw = 56 * S, bh = 5 * S, c = bx + bw / 2;
        fill_rect(lk, bx, by, bx + bw, by + bh, 0x303030);
        if (st >= 0) fill_rect(lk, c, by, c + (int)(st * bw / 2), by + bh, 0xF0C020);
        else fill_rect(lk, c + (int)(st * bw / 2), by, c, by + bh, 0xF0C020);
        fill_rect(lk, c, by - S, c + S, by + bh + S, 0xFFFFFF);
        fill_rect(lk, bx, by + 7 * S, bx + bw, by + 9 * S, 0x303030);
        fill_rect(lk, bx, by + 7 * S, bx + (int)(ac * bw), by + 9 * S, 0x40D040);
        fill_rect(lk, bx, by + 11 * S, bx + bw, by + 13 * S, 0x303030);
        fill_rect(lk, bx, by + 11 * S, bx + (int)(br * bw), by + 13 * S, 0xD04040);
    }
}

/* The setup screen runs inside the game's own front-end frames: Calibrate (0x413430)
   only switches it on and returns; the present hook calls pad_setup_frame() just before
   every frame goes out, which reads input and draws over the back buffer. While it is
   open the game's keyboard is blanked; on exit one Esc is sent so the game's (empty)
   Calibrate page closes too. */
static struct {
    int sel, capturing, started;
    struct uiin prev;
    unsigned char btn0[PAD_NBUTTONS];
    float axis0[PAD_NAXES];
    BYTE *bg;
    int bg_pitch, bg_h;
    DWORD next_rep;
    int repeat;
} g_ps;

static void pad_setup_close(void)
{
    free(g_ps.bg); g_ps.bg = NULL;
    pad_save_settings();
    g_pad_setup_open = 0;
    if (g_cal_inpage) g_cal_leave = 1;   /* back to the options page once the keys are let go */
    else g_pad_esc_pulse = 3;            /* leave the game's Calibrate page as well */
    g_cal_inpage = 0;
    log_msg("pad: setup saved (device %s)", g_pad.active >= 0 ? g_pad.names[g_pad.active] : "none");
}

static void pad_setup_input(void)
{
    struct uiin in;
    int sel = g_ps.sel;
    ui_read(&in);
    if (g_ps.capturing) {
        int s = pad_capture(g_ps.btn0, g_ps.axis0);
        if (in.back && !g_pad.button[1] && !g_ps.prev.back) g_ps.capturing = 0;     /* Esc cancels */
        else if (s) { g_bind[sel - ROW_BIND0] = s; g_ps.capturing = 0; memset(&in, 1, sizeof in); }
        if (g_ps.capturing) { int i; for (i = 0; i < PAD_NBUTTONS; i++) g_ps.btn0[i] &= g_pad.button[i]; }
    } else {
        int up = in.up && !g_ps.prev.up, down = in.down && !g_ps.prev.down;
        int left = in.left && !g_ps.prev.left, right = in.right && !g_ps.prev.right;
        DWORD now = GetTickCount();
        if ((in.up || in.down) && (g_ps.prev.up || g_ps.prev.down)) {          /* auto-repeat */
            if (!g_ps.repeat) { g_ps.repeat = 1; g_ps.next_rep = now + 400; }
            else if (now >= g_ps.next_rep) { g_ps.next_rep = now + 120; up = in.up; down = in.down; }
        } else g_ps.repeat = 0;
        if (up) sel = (sel + ROW_COUNT - 1) % ROW_COUNT;
        if (down) sel = (sel + 1) % ROW_COUNT;
        if (left || right) {
            int d = right ? 1 : -1;
            if (sel == ROW_DEVICE && g_pad.ndev > 0) g_pad_want = ((g_pad.active < 0 ? 0 : g_pad.active) + d + g_pad.ndev) % g_pad.ndev;
            if (sel == ROW_DEADZONE) g_pad_deadzone = max(0, min(60, g_pad_deadzone + 5 * d));
            if (sel == ROW_STEER) g_pad_steer_analog = !g_pad_steer_analog;
            if (sel == ROW_THROTTLE) g_pad_throttle_analog = !g_pad_throttle_analog;
            if (sel == ROW_LEAN) g_car_lean = max(0, min(100, g_car_lean + 25 * d));
            if (sel == ROW_RIDE) g_car_lowered = !g_car_lowered;
            if (sel == ROW_SPEED) g_game_speed = max(1, min(3, g_game_speed + d));
            if (sel == ROW_TRACKS) g_unlock_all = !g_unlock_all;
        }
        if (in.ok && !g_ps.prev.ok) {
            if (sel >= ROW_BIND0 && sel < ROW_BIND0 + ACT_COUNT) {
                g_ps.capturing = 1;
                memcpy(g_ps.btn0, g_pad.button, sizeof g_ps.btn0); memcpy(g_ps.axis0, g_pad.axis, sizeof g_ps.axis0);
                g_ps.btn0[0] = 1;                                      /* the A press that started it */
            } else if (sel == ROW_STEER) g_pad_steer_analog = !g_pad_steer_analog;
            else if (sel == ROW_THROTTLE) g_pad_throttle_analog = !g_pad_throttle_analog;
            else if (sel == ROW_RIDE) g_car_lowered = !g_car_lowered;
            else if (sel == ROW_SPEED) g_game_speed = g_game_speed == 1 ? 3 : g_game_speed - 1;
            else if (sel == ROW_TRACKS) g_unlock_all = !g_unlock_all;
            else if (sel == ROW_DEFAULTS) { memcpy(g_bind, k_bind_default, sizeof g_bind); g_pad_deadzone = 15; g_pad_steer_analog = 1; g_pad_throttle_analog = 1; g_car_lean = 100; g_car_lowered = 1; g_game_speed = 2; }
            else if (sel == ROW_DONE) { g_ps.prev = in; g_ps.sel = sel; pad_setup_close(); return; }
            else if (sel == ROW_DEVICE && g_pad.ndev > 0) g_pad_want = ((g_pad.active < 0 ? 0 : g_pad.active) + 1) % g_pad.ndev;
        }
        if (in.back && !g_ps.prev.back) { g_ps.prev = in; pad_setup_close(); return; }
    }
    g_ps.sel = sel;
    g_ps.prev = in;
}

/* called by the present hook before each frame is shown */
static void pad_setup_frame(void)
{
    void *bb = *(void **)0x004891E4, **vt;
    BYTE desc[0x6C];
    struct lockinfo lk;
    int h;
    {   /* debug: MMCDA_OPENSETUP opens the setup screen once the menus are up */
        static int auto_done;
        if (!auto_done && g_font_ready && GetEnvironmentVariableA("MMCDA_OPENSETUP", NULL, 0)) {
            auto_done = 1; g_ps.started = 0; g_pad_setup_open = 1;
        }
    }
    g_vr_2d_only = g_pad_setup_open && !g_cal_inpage;
    if (!g_pad_setup_open) return;
    if (!g_ps.started) {
        memset(&g_ps, 0, sizeof g_ps);
        memset(&g_ps.prev, 1, sizeof g_ps.prev);                 /* wait for the opening press to end */
        g_ps.started = 1;
    }
    pad_setup_input();
    g_vr_2d_only = g_pad_setup_open && !g_cal_inpage;
    if (!g_pad_setup_open || !bb || !g_font_ready) return;
    vt = *(void ***)bb;
    memset(desc, 0, sizeof desc); *(DWORD *)desc = sizeof desc;
    if (((HRESULT (WINAPI *)(void *, RECT *, void *, DWORD, HANDLE))vt[25])(bb, NULL, desc, 1, NULL) != 0) return;
    lk.bits = *(BYTE **)(desc + 0x24); lk.pitch = *(int *)(desc + 0x10);
    lk.h = *(int *)(desc + 0x08); lk.w = *(int *)(desc + 0x0C); lk.bpp = *(int *)(desc + 0x54);
    g_txt_scale = max(1, lk.h / 240);
    if (!g_ps.bg || g_ps.bg_pitch != lk.pitch || g_ps.bg_h != lk.h) {   /* the menu as it was, as background */
        free(g_ps.bg);
        g_ps.bg = (BYTE *)malloc((size_t)lk.pitch * lk.h);
        g_ps.bg_pitch = lk.pitch; g_ps.bg_h = lk.h;
        if (g_ps.bg) for (h = 0; h < lk.h; h++) memcpy(g_ps.bg + (size_t)h * lk.pitch, lk.bits + h * lk.pitch, lk.pitch);
    }
    if (g_cal_inpage) setup_draw(&lk, NULL, g_ps.sel, g_ps.capturing);
    else if (g_ps.bg) setup_draw(&lk, g_ps.bg, g_ps.sel, g_ps.capturing);
    ((HRESULT (WINAPI *)(void *, void *))vt[32])(bb, NULL);
}

static void __cdecl calibrate_hook(void);
static void __cdecl calibrate_page_hook(void)
{
    calibrate_hook();
    g_cal_inpage = g_pad_setup_open;
}

static void __cdecl calibrate_hook(void)
{
    if (!g_font_ready) { log_msg("pad: setup screen needs the front-end font"); return; }
    g_ps.started = 0;
    sprite_copy(&g_page_bg, 1);
    if (g_page_bg.w != 320 || g_page_bg.h < 240) { free(g_page_bg.px); g_page_bg.px = NULL; }
    sprite_copy(&g_page_logo, LOADED_SPRITE[225]);
    g_pad_setup_open = 1;
    log_msg("pad: setup screen opened (page background %dx%d)", g_page_bg.w, g_page_bg.h);
}


/* 0x4155f0: the game's own DirectInput joystick read (digital, 90% dead zone). With lizard
   mode off, the Steam Controller's raw HID interface also appears to DirectInput, where
   its stick/grip touch sensors land on the first buttons; the game turned those into
   fire and P (pause). While SDL drives a controller, report "nothing pressed" here so all
   controller input goes through our mapping only. */
typedef unsigned (__cdecl *PFN_JOYREAD)(int);
static PFN_JOYREAD g_joyread_orig;
/* SDL controller in the game's joystick format (what 0x4155f0 returns): 1 = button 1
   (accelerate), 0x1000 = button 2 (fire), 0x40 stick up (dive), 0x80 stick down (brake),
   0x8000 left, 0x2000 right. Used when the player picked the joystick icon. */
static unsigned pad_joybits(void)
{
    unsigned b = 0;
    float st = src_value(g_bind[ACT_RIGHT]) - src_value(g_bind[ACT_LEFT]);
    float th = g_pad_steer_analog ? 0.0f : 0.5f, tt = g_pad_throttle_analog ? 0.0f : 0.5f;
    if (st > th || g_pad.button[14]) b |= 0x2000;
    if (-st > th || g_pad.button[13]) b |= 0x8000;
    if (src_value(g_bind[ACT_ACCEL]) > tt) b |= 1;
    if (src_value(g_bind[ACT_BRAKE]) > tt || g_pad.button[12]) b |= 0x80;
    if (src_value(g_bind[ACT_FIRE]) > 0.5f) b |= 0x1000;
    if (src_value(g_bind[ACT_ACTION]) > 0.5f || g_pad.button[11]) b |= 0x40;
    if (!g_race_active) {                  /* menus: the left stick moves up/down as well */
        if (g_pad.axis[1] < -0.5f) b |= 0x40;
        if (g_pad.axis[1] > 0.5f) b |= 0x80;
    }
    return b;
}

static unsigned __cdecl joyread_hook(int n)
{
    if (g_pad.active >= 0) {
        if (g_pad_setup_open) return 0;
        if (n == 0 && g_race_active) g_joy_race_poll = GetTickCount();
        return n == 0 ? pad_joybits() : 0;      /* joystick 1 = the SDL controller */
    }
    /* the controller was unplugged: the game still counts the Joystick 1 we offered, but
       its DirectInput slot (0x54A1D0) is empty, and the original read would crash on it */
    if (n < 0 || n >= 2 || !((void *volatile *)0x0054A1D0)[n]) return 0;
    return g_joyread_orig(n);
}

static void install_pad(void)
{
    static const BYTE cal_expect[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x58 };
    char v[8];
    pad_load_settings();
    if (GetEnvironmentVariableA("MMCDA_NOPAD", v, sizeof v)) return;
    CreateThread(NULL, 0, pad_thread, NULL, 0, NULL);
    {
        static const BYTE jr_expect[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x50 };
        if (memcmp((BYTE *)0x004155F0, jr_expect, 6) == 0 &&
            (g_joyread_orig = (PFN_JOYREAD)make_trampoline(jr_expect, 6, (BYTE *)0x004155F6)) &&
            patch_jmp((BYTE *)0x004155F0, jr_expect, 6, joyread_hook))
            log_msg("pad: the game's joystick 1 reads the SDL controller");
    }
    {   /* The Calibrate page (frame counter 0x5050d8) switched to a window on its first frame,
           ran joy.cpl on its 4th, switched back (rebuilding the whole front end) and left
           (0x5072f4 = 2). Now: no window switch, the 4th frame opens the setup screen over the
           page as it is (icon, logo, background), and closing it leaves the page. */
        static const BYTE f0[2] = { 0x6A, 0x01 }, f3[10] = { 0xE8, 0x66, 0x4B, 0x00, 0x00, 0xA1, 0x18, 0x5E, 0x50, 0x00 };
        BYTE *a = (BYTE *)0x0040E8A3, *b = (BYTE *)0x0040E8C5;
        DWORD old;
        if (memcmp(a, f0, 2) == 0 && memcmp(b, f3, 10) == 0) {
            VirtualProtect(a, 0x30, PAGE_EXECUTE_READWRITE, &old);
            a[0] = 0xEB; a[1] = (BYTE)(0x40E8EA - 0x40E8A5);                      /* frame 0: nothing */
            b[0] = 0xE8; *(INT32 *)(b + 1) = (INT32)((BYTE *)calibrate_page_hook - (b + 5));
            b[5] = 0xEB; b[6] = (BYTE)(0x40E8EA - 0x40E8CC);                      /* skip the switch back and the exit */
            VirtualProtect(a, 0x30, old, &old);
            FlushInstructionCache(GetCurrentProcess(), a, 0x30);
            log_msg("pad: Calibrate page keeps its screen");
        }
    }
    if (memcmp((BYTE *)0x00413430, cal_expect, 6) == 0 && patch_jmp((BYTE *)0x00413430, cal_expect, 6, calibrate_hook))
        log_msg("pad: Calibrate opens the controller setup screen");
    else log_msg("pad: calibrate site mismatch");
}
