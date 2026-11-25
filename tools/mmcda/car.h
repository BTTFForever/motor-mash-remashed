/* ==== Car handling tweaks: true analog control, body lean, flat shadows ===============
 * Included by mmcda.c after pad.h.
 *
 * Car records: 0x632da0 + i * 0x460. Driving update 0x4241f0(control word, ?, car):
 *   +0x2e0 turn rate per tick (heading +0x368 changes by it while Left/Right is held)
 *   +0x3d4 engine power (0x423a50(heading, power) pushes the car while Up/Down is held)
 *   +0x3a8 steering lean, turned into a tilt in +0x39c (a world-axis slope value) when
 *          the car is on flat ground; +0x5c is set to 2 then. Real slopes from the
 *          ground use +0x39c / +0x3a4 too, but never with +0x5c == 2.
 * True analog: for player 1's car the turn rate and power are scaled by the stick /
 * trigger for the duration of one update (the game still sees a digital key held).
 * Body lean: the lean tilt is scaled by mmcda.ini [car] lean (percent, 100 = original).
 * Shadows (0x485c18[i]) get the car's full tilt in 0x403230; while a car leans on flat
 * ground its shadow is laid flat again (heading, size and position kept).
 */
#define CAR_REC(i)   ((BYTE *)0x00632DA0 + (i) * 0x460)
#define CAR_COUNT    (*(volatile int *)0x00634C00)
#define CAR_SLOT(i)  (((volatile int *)0x00634C28)[i])
#define CAR_I(c, off) (*(int *)((BYTE *)(c) + (off)))


typedef void (__cdecl *PFN_DRIVE)(unsigned, unsigned, BYTE *);
static PFN_DRIVE g_drive_orig;
static float g_car_drop = 10.0f;

static void rumble_drive(BYTE *car);   /* rumble.h */
static int car_is_player1(BYTE *car)
{
    int i = (int)(car - CAR_REC(0)) / 0x460;
    if (i < 0 || i >= 8 || car != CAR_REC(i)) return 0;
    return CAR_I(car, 1000) != 1 && CAR_SLOT(i) == 0;     /* not computer driven, player slot 0 */
}

static void __cdecl drive_hook(unsigned in, unsigned p2, BYTE *car)
{
    static float turn_acc;
    int saved_turn = CAR_I(car, 0x2E0), saved_power = CAR_I(car, 0x3D4), scaled = 0;
    if (car_is_player1(car)) {
        if (g_an_steer_on && (in & 0xA000) && g_an_steer != 0.0f) {   /* from the stick, not keys */
            /* heading changes by +0x2e0 per tick at full lock; carry the fraction over */
            float want = saved_turn * min(1.0f, fabsf(g_an_steer)) + turn_acc;
            int t = (int)want;
            turn_acc = want - t;
            CAR_I(car, 0x2E0) = t;
            scaled = 1;
        } else turn_acc = 0;
        if (g_an_throttle_on && (in & 0xC0)) {
            /* the car loses +0x3d8 (drag) every tick, so power below that never moves it.
               Throttle: the trigger sets a target speed (fraction of top speed +0x3d0);
               below it the power above drag is scaled (never less than 35%), at or above
               it the push only matches drag. Brake/reverse: power above drag scaled. */
            int accel = (in & 0x40) != 0;
            float a = min(1.0f, accel ? g_an_accel : g_an_brake);
            if (a > 0.0f) {                                           /* from a trigger, not keys */
                int drag = CAR_I(car, 0x3D8), speed = CAR_I(car, 0x3C0), vmax = CAR_I(car, 0x3D0);
                int extra = saved_power > drag ? saved_power - drag : 0;
                float p;
                if (!accel) p = drag + extra * a;
                else if (speed < a * vmax) p = drag + extra * (0.35f + 0.65f * a);
                else p = (float)min(saved_power, drag);
                CAR_I(car, 0x3D4) = (int)(p + 0.5f);
                scaled = 1;
            }
        }
    }
    g_drive_orig(in, p2, car);
    if (car_is_player1(car)) rumble_drive(car);
    if (scaled) { CAR_I(car, 0x2E0) = saved_turn; CAR_I(car, 0x3D4) = saved_power; }
    if (g_car_lean != 100 && CAR_I(car, 0x5C) == 2)             /* leaning on flat ground */
        CAR_I(car, 0x39C) = CAR_I(car, 0x39C) * g_car_lean / 100;
}

/* Car models have their origin (the pivot the game turns and places them around) a few
   units off the middle of the body, so cars swing around a point near one end. Find the
   middle of each car's mesh (MeshBuilder GetBox, slot 16) once, and shift the model and
   its shadow so the middle sits on the pivot. */
struct carcentre { void *frame; float cx, cz; int ok; };
static struct carcentre g_cc[8];
static int car_centre(int i, void *body, float *cx, float *cz)
{
    struct carcentre *c = &g_cc[i];
    if (c->frame != body) {
        void *arr = NULL, *vis = NULL;
        c->frame = body; c->ok = 0;
        if (((HRESULT (WINAPI *)(void *, void **))(*(void ***)body)[32])(body, &arr) == 0 && arr) {   /* GetVisuals */
            if (((DWORD (WINAPI *)(void *))(*(void ***)arr)[3])(arr) > 0 &&
                ((HRESULT (WINAPI *)(void *, DWORD, void **))(*(void ***)arr)[4])(arr, 0, &vis) == 0 && vis) {
                char cls[32]; DWORD sz = sizeof cls;
                float box[6];
                cls[0] = 0;
                ((HRESULT (WINAPI *)(void *, DWORD *, char *))(*(void ***)vis)[10])(vis, &sz, cls);
                if (!lstrcmpA(cls, "Builder") &&
                    ((HRESULT (WINAPI *)(void *, float *))(*(void ***)vis)[16])(vis, box) == 0) {
                    c->cx = (box[0] + box[3]) / 2; c->cz = (box[2] + box[5]) / 2; c->ok = 1;
                    log_msg("car %d: model centre offset %.1f, %.1f", i, c->cx, c->cz);
                }
                ((ULONG (WINAPI *)(void *))(*(void ***)vis)[2])(vis);
            }
            ((ULONG (WINAPI *)(void *))(*(void ***)arr)[2])(arr);
        }
    }
    *cx = c->cx; *cz = c->cz;
    return c->ok;
}

/* Spinning objects (pickups etc.): records of 9 ints at 0x488008, count 0x485bf0,
   [0] = root frame, [8] = placed this tick. 0x403230 sets the root to a fresh spin about
   the vertical axis, 1/8 scale and the position each tick; the model's origin is off its
   middle, so it wobbles round a small circle. Measure the middle once (bounding boxes of
   all parts, in root space) and shift the model onto the spin axis after placement. */
static void mat_mul4(MAT4 r, MAT4 a, MAT4 b)     /* r = a * b (row vectors) */
{
    int i, j; MAT4 t;
    for (i = 0; i < 4; i++) for (j = 0; j < 4; j++)
        t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
    memcpy(r, t, sizeof t);
}
struct objcentre { void *frame; float cx, cz; int ok; };
static struct objcentre g_oc[32];

static void obj_box_walk(void *f, MAT4 acc, float *mn, float *mx, int depth, int is_root)
{
    void *arr = NULL, *o;
    MAT4 local, m;
    DWORD i, n;
    if (depth > 8) return;
    if (is_root) memcpy(m, acc, sizeof m);
    else { if (g_fr_gettransform(f, local) != 0) return; mat_mul4(m, local, acc); }
    if (((HRESULT (WINAPI *)(void *, void **))(*(void ***)f)[32])(f, &arr) == 0 && arr) {      /* visuals */
        n = ((DWORD (WINAPI *)(void *))(*(void ***)arr)[3])(arr);
        for (i = 0; i < n && i < 16; i++)
            if (((HRESULT (WINAPI *)(void *, DWORD, void **))(*(void ***)arr)[4])(arr, i, &o) == 0 && o) {
                char cls[32]; DWORD sz = sizeof cls; float b[6]; int c;
                cls[0] = 0;
                ((HRESULT (WINAPI *)(void *, DWORD *, char *))(*(void ***)o)[10])(o, &sz, cls);
                if (!lstrcmpA(cls, "Builder") && ((HRESULT (WINAPI *)(void *, float *))(*(void ***)o)[16])(o, b) == 0)
                    for (c = 0; c < 8; c++) {   /* box corners into root space */
                        float x = b[(c & 1) ? 3 : 0], y = b[(c & 2) ? 4 : 1], z = b[(c & 4) ? 5 : 2];
                        float wx = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0];
                        float wz = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2];
                        if (wx < mn[0]) mn[0] = wx; if (wx > mx[0]) mx[0] = wx;
                        if (wz < mn[1]) mn[1] = wz; if (wz > mx[1]) mx[1] = wz;
                    }
                ((ULONG (WINAPI *)(void *))(*(void ***)o)[2])(o);
            }
        ((ULONG (WINAPI *)(void *))(*(void ***)arr)[2])(arr);
    }
    arr = NULL;
    if (((HRESULT (WINAPI *)(void *, void **))(*(void ***)f)[19])(f, &arr) == 0 && arr) {      /* children */
        n = ((DWORD (WINAPI *)(void *))(*(void ***)arr)[3])(arr);
        for (i = 0; i < n && i < 32; i++)
            if (((HRESULT (WINAPI *)(void *, DWORD, void **))(*(void ***)arr)[4])(arr, i, &o) == 0 && o) {
                obj_box_walk(o, m, mn, mx, depth + 1, 0);
                ((ULONG (WINAPI *)(void *))(*(void ***)o)[2])(o);
            }
        ((ULONG (WINAPI *)(void *))(*(void ***)arr)[2])(arr);
    }
}

static void centre_objects(void)
{
    int k, n = *(volatile int *)0x00485BF0;
    typedef HRESULT (WINAPI *PFN_TR)(void *, int, float, float, float);
    for (k = 0; k < n && k < 32; k++) {
        int *rec = (int *)(0x00488008 + k * 0x24);
        void *f = (void *)rec[0];
        struct objcentre *c = &g_oc[k];
        if (!f || !rec[8]) continue;
        if (c->frame != f) {
            MAT4 id = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
            float mn[2] = { 1e30f, 1e30f }, mx[2] = { -1e30f, -1e30f };
            c->frame = f; c->ok = 0;
            obj_box_walk(f, id, mn, mx, 0, 1);
            if (mn[0] <= mx[0]) {
                c->cx = (mn[0] + mx[0]) / 2; c->cz = (mn[1] + mx[1]) / 2; c->ok = 1;
                log_msg("object %d: centre offset %.1f, %.1f (model units)", k, c->cx, c->cz);
            }
        }
        {   /* only objects that spin: their yaw (state 0x634924 + k*44 - 8) changes each tick */
            static int last_yaw[32], spin_ticks[32];
            int yaw = *(volatile int *)(0x00634924 + k * 44 - 8);
            if (yaw != last_yaw[k]) spin_ticks[k] = min(spin_ticks[k] + 1, 1000); else spin_ticks[k] = 0;
            last_yaw[k] = yaw;
            if (c->ok && spin_ticks[k] >= 2) ((PFN_TR)(*(void ***)f)[15])(f, 1 /* before */, -c->cx, 0, -c->cz);
        }
    }
}

/* after cars are placed: lay the shadows of leaning cars flat on the (flat) ground */
typedef int (__cdecl *PFN_PLACE)(void);
static PFN_PLACE g_place_orig;
static int __cdecl place_hook(void)
{
    int r = g_place_orig(), i, n = CAR_COUNT;
    if (!g_fr_gettransform || !g_fr_addtransform) return r;
    centre_objects();
    /* Cars in state +0x440 = 1..4 (e.g. firing the machine gun, hit by a weapon) are drawn
       as an animated model from the object pool, and 0x403230 leaves their body frame
       where it was. The race cameras (cams.h) follow the body frame, so give it the pose
       the game would have used (no visual is attached, so nothing extra is drawn). */
    for (i = 0; i < n && i < 8; i++) {
        BYTE *car = CAR_REC(i);
        void *body = g_t_bodyframe[i];
        typedef HRESULT (WINAPI *PFN_ROT)(void *, int, float, float, float, float);
        typedef HRESULT (WINAPI *PFN_POS)(void *, void *, float, float, float);
        void **vt;
        int y;
        if (!body || (g_t_placed[i] & 1) || !(g_t_placed[i] & 2)) continue;
        vt = *(void ***)body;
        ((PFN_ROT)vt[17])(body, 0 /* replace */, 0, 1, 0, (float)((CAR_I(car, 0x3B8) & 0x7F) << 5) * 0.0015340011f);
        ((PFN_ROT)vt[17])(body, 2 /* after */, 0, 0, -1, (float)CAR_I(car, 0x3A4) * 0.0015340011f);
        ((PFN_ROT)vt[17])(body, 2, -1, 0, 0, (float)CAR_I(car, 0x39C) * 0.0015340011f);
        y = CAR_I(car, 0x398) + CAR_I(car, 0x388);
        ((PFN_POS)vt[61])(body, *(void **)0x0048899C,
                          (float)(CAR_I(car, 0x384) / 8 - *(volatile int *)0x0044D3B8),
                          (float)(0xC - y / 8),
                          (float)(CAR_I(car, 0x38C) / 8 - *(volatile int *)0x0044D3C0));
        /* ...and lower the animated model like the body ("Lowered"): it is the pooled
           frame (records of 9 ints at 0x486070, +0x20 = in use) placed at the car's spot */
        if (g_car_lowered) {
            float px = (float)(CAR_I(car, 0x384) / 8 - *(volatile int *)0x0044D3B8);
            float py = (float)(0xC - y / 8);
            float pz = (float)(CAR_I(car, 0x38C) / 8 - *(volatile int *)0x0044D3C0);
            int k;
            for (k = 0; k < 60; k++) {
                BYTE *rec = (BYTE *)0x00486070 + k * 0x24;
                void *pf = *(void **)rec;
                MAT4 pm;
                if (!pf || *(int *)(rec + 0x20) != 1 || g_fr_gettransform(pf, pm) != 0) continue;
                if (fabsf(pm[3][0] - px) < 0.5f && fabsf(pm[3][2] - pz) < 0.5f && fabsf(pm[3][1] - py) < 0.5f) {
                    typedef HRESULT (WINAPI *PFN_TR)(void *, int, float, float, float);
                    ((PFN_TR)(*(void ***)pf)[15])(pf, 2 /* after */, 0, -g_car_drop, 0);
                    break;
                }
            }
        }
    }
    for (i = 0; i < n && i < 8; i++) {
        BYTE *car = CAR_REC(i);
        void *sh = g_t_shadframe[i], *body = g_t_bodyframe[i];
        MAT4 m;
        float sx, sy, sz, fx, fz, fl;
        /* 0x403230 orients the car with its heading cut to 128 steps (+0x3a0 = 2.8 degree
           steps) while the physics heading +0x368 has 1024: add the missing fraction as a
           rotation applied before the game's (same axis and units), car and shadow */
        {
            int h = CAR_I(car, 0x368) & 0x3FF, q = CAR_I(car, 0x3A0);
            float d = (float)(h * 4 - q);
            if (d > 2048) d -= 4096; else if (d < -2048) d += 4096;
            if (d > -64 && d < 64) {
                typedef HRESULT (WINAPI *PFN_ROT)(void *, int, float, float, float, float);
                int placed = g_t_placed[i];   /* 1 body, 4 shadow set this tick */
                if (body && (placed & 1) && CAR_I(car, 0x5C) == 2) {
                    /* Leaning on flat ground: the game tilts the body by the lean (+0x39c)
                       about the WORLD x axis after the heading, so it only looks right facing
                       one way (heading 0) and is reversed / becomes pitch otherwise. Rebuild:
                       lean about the car's own axis (as at heading 0), then the precise
                       heading, then the game's scale and position. */
                    typedef HRESULT (WINAPI *PFN_V3C)(void *, int, float, float, float);
                    MAT4 bm;
                    if (g_fr_gettransform(body, bm) == 0) {
                        void **bvt = *(void ***)body;
                        float bsx = sqrtf(bm[0][0] * bm[0][0] + bm[0][1] * bm[0][1] + bm[0][2] * bm[0][2]);
                        float bsy = sqrtf(bm[1][0] * bm[1][0] + bm[1][1] * bm[1][1] + bm[1][2] * bm[1][2]);
                        float bsz = sqrtf(bm[2][0] * bm[2][0] + bm[2][1] * bm[2][1] + bm[2][2] * bm[2][2]);
                        ((PFN_ROT)bvt[17])(body, 0 /* replace */, 0, 1, 0, (float)(h * 4) * 0.0015340011f);
                        ((PFN_ROT)bvt[17])(body, 1 /* before */, -1, 0, 0, (float)CAR_I(car, 0x39C) * 0.0015340011f);
                        ((PFN_V3C)bvt[16])(body, 2 /* after */, bsx, bsy, bsz);              /* AddScale */
                        ((PFN_V3C)bvt[15])(body, 2, bm[3][0], bm[3][1], bm[3][2]);         /* AddTranslation */
                    }
                } else if (body && (placed & 1)) ((PFN_ROT)(*(void ***)body)[17])(body, 1 /* before */, 0, 1, 0, d * 0.0015340011f);
                {   /* centre the model (and its shadow, same offset) on the pivot */
                    typedef HRESULT (WINAPI *PFN_TR)(void *, int, float, float, float);
                    float cx, cz;
                    static int nocentre = -1;
                    if (nocentre < 0) nocentre = GetEnvironmentVariableA("MMCDA_NOCENTRE", NULL, 0) != 0;
                    if (!nocentre && body && (placed & 1) && car_centre(i, body, &cx, &cz)) {
                        ((PFN_TR)(*(void ***)body)[15])(body, 1 /* before */, -cx, 0, -cz);
                        if (sh && (placed & 4)) ((PFN_TR)(*(void ***)sh)[15])(sh, 1, -cx, 0, -cz);
                    }
                }
                /* ride height: the game sets cars ~10 units (wheels) and their shadows ~5
                   units above the road; "Lowered" removes most of that (jumps unchanged) */
                if (g_car_lowered) {
                    typedef HRESULT (WINAPI *PFN_TR)(void *, int, float, float, float);
                    if (body && (placed & 1)) ((PFN_TR)(*(void ***)body)[15])(body, 2 /* after */, 0, -g_car_drop, 0);
                    if (sh && (placed & 4)) ((PFN_TR)(*(void ***)sh)[15])(sh, 2, 0, -2.0f, 0);   /* 4 above the ground: lower and some City road surfaces cover it */
                }
                if (sh && (placed & 4)) ((PFN_ROT)(*(void ***)sh)[17])(sh, 1, 0, 1, 0, d * 0.0015340011f);
            }
        }
        if (!sh || CAR_I(car, 0x5C) != 2 || !(g_t_placed[i] & 4)) continue;
        if (g_fr_gettransform(sh, m) != 0) continue;
        sx = sqrtf(m[0][0] * m[0][0] + m[0][1] * m[0][1] + m[0][2] * m[0][2]);
        sy = sqrtf(m[1][0] * m[1][0] + m[1][1] * m[1][1] + m[1][2] * m[1][2]);
        sz = sqrtf(m[2][0] * m[2][0] + m[2][1] * m[2][1] + m[2][2] * m[2][2]);
        fx = m[2][0]; fz = m[2][2]; fl = sqrtf(fx * fx + fz * fz);
        if (fl < 1e-6f || sz < 1e-6f) continue;
        fx /= fl; fz /= fl;
        /* forward kept on the ground plane, up = world up, right = up x forward */
        m[2][0] = fx * sz; m[2][1] = 0; m[2][2] = fz * sz;
        m[1][0] = 0; m[1][1] = sy; m[1][2] = 0;
        m[0][0] = fz * sx; m[0][1] = 0; m[0][2] = -fx * sx;
        g_fr_addtransform(sh, 0 /* replace */, m);
    }
    return r;
}

/* Track unlocking: a track is selectable once it has a lap record (record byte at
   0x463A30 + track * 0x34 below 0x0D; a new game starts with 2 per world). The course
   picker 0x40A620 tests that in two loops ("cmp byte [edi],0Dh / jae skip" at 0x40A6BE and
   0x40A8D3). "All tracks" turns the two jumps into no-ops; the saved records (Clock.ini)
   are never touched, so switching back locks them again. */
static void tracks_apply(void)
{
    static const struct { DWORD at; BYTE jae[2]; } k_sites[2] = { { 0x40A6C1, { 0x73, 0x1D } }, { 0x40A8D6, { 0x73, 0x0D } } };
    int k;
    DWORD old;
    for (k = 0; k < 2; k++) {
        BYTE *c = (BYTE *)k_sites[k].at;
        if (!(memcmp(c, k_sites[k].jae, 2) == 0 || (c[0] == 0x90 && c[1] == 0x90))) continue;
        VirtualProtect(c, 2, PAGE_EXECUTE_READWRITE, &old);
        if (g_unlock_all) { c[0] = 0x90; c[1] = 0x90; } else memcpy(c, k_sites[k].jae, 2);
        VirtualProtect(c, 2, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), (void *)0x40A6C1, 0x220);
}

static void car_load_settings(void)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%smmcda.ini", g_home);
    g_car_lean = GetPrivateProfileIntA("car", "lean", 100, path);
    {
        char v[16];
        GetPrivateProfileStringA("race", "speed", "medium", v, sizeof v, path);
        g_game_speed = _stricmp(v, "easy") == 0 ? 1 : _stricmp(v, "hard") == 0 ? 3 : 2;
        GetPrivateProfileStringA("race", "tracks", "normal", v, sizeof v, path);
        g_unlock_all = _stricmp(v, "all") == 0;
        tracks_apply();
    }
    g_car_lowered = GetPrivateProfileIntA("car", "lowered", 1, path) != 0;
    {   /* how far "Lowered" drops the body, game units (default 10) */
        char v[16];
        GetPrivateProfileStringA("car", "drop", "10", v, sizeof v, path);
        g_car_drop = (float)atof(v);
        if (g_car_drop < 0 || g_car_drop > 20) g_car_drop = 10;
    }
    if (g_car_lean < 0 || g_car_lean > 100) g_car_lean = 100;
}

static void car_save_settings(void)
{
    char path[MAX_PATH], v[16];
    snprintf(path, sizeof path, "%smmcda.ini", g_home);
    snprintf(v, sizeof v, "%d", g_car_lean);
    WritePrivateProfileStringA("car", "lean", v, path);
    WritePrivateProfileStringA("car", "lowered", g_car_lowered ? "1" : "0", path);
    WritePrivateProfileStringA("race", "speed", g_game_speed == 1 ? "easy" : g_game_speed == 2 ? "medium" : "hard", path);
    WritePrivateProfileStringA("race", "tracks", g_unlock_all ? "all" : "normal", path);
    tracks_apply();
}

static void install_car(void)
{
    static const BYTE drv_expect[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18 };
    static const BYTE plc_expect[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x2C };
    car_load_settings();
    if (memcmp((BYTE *)0x004241F0, drv_expect, 6) == 0 &&
        (g_drive_orig = (PFN_DRIVE)make_trampoline(drv_expect, 6, (BYTE *)0x004241F6)) &&
        patch_jmp((BYTE *)0x004241F0, drv_expect, 6, drive_hook))
        log_msg("car: analog control + lean hook installed (lean %d%%)", g_car_lean);
    if (memcmp((BYTE *)0x00403230, plc_expect, 6) == 0 &&
        (g_place_orig = (PFN_PLACE)make_trampoline(plc_expect, 6, (BYTE *)0x00403236)) &&
        patch_jmp((BYTE *)0x00403230, plc_expect, 6, place_hook))
        log_msg("car: flat shadows hook installed");
}
