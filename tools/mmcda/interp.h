/* ==== Frame interpolation (render at the monitor refresh rate) ======================
 * Included by mmcda.c (uses its helpers: log_msg, G_I32, make_trampoline, patch_jmp,
 * PFN_VOIDINT, g_transition).
 *
 * The game runs one fixed logic step per frame and caps frames at 33 ms by
 * busy-waiting in 0x406b70. We keep the logic at that rate but, instead of
 * busy-waiting, render extra frames in which every object the logic moved is
 * blended between its transform at the previous and the latest tick.
 *  - Objects that moved are found by hooking the D3DRM frame methods the game uses
 *    to place things (SetPosition/SetOrientation/AddRotation/AddScale/...).
 *  - At each viewport Render we apply lerp(prev, cur, alpha) to those frames, render,
 *    and restore cur. alpha = time since the tick began / tick length, so motion is
 *    shown one tick (~33 ms) behind, smoothly.
 *  - Extra frames are paced by vsync (dgVoodoo ForceVerticalSync = true).
 * MMCDA_NOINTERP=1 disables it. */
#include <math.h>

#define FR_ADDTRANSFORM   14
#define FR_ADDTRANSLATION 15
#define FR_ADDSCALE       16
#define FR_ADDROTATION    17
#define FR_GETTRANSFORM   29
#define FR_SETORIENTATION 60
#define FR_SETPOSITION    61
#define VP_CLEAR   12
#define VP_RENDER  13
#define DEV_UPDATE 14
/* Game speed (difficulty): the game logic runs one tick per frame, capped at ~33 ms (30 ticks/s).
   Easy and Medium stretch the tick during races to 20 and 25 ticks/s, so cars, CPU drivers, weapons
   and the race clock all slow down together; the extra time is filled with interpolated frames.
   25 is the PS1 PAL rate (VSync(2) at 50 Hz); 20 is about what period PCs managed.
   mmcda.ini [race] speed = easy | medium | hard (Controller Setup: Game speed). */
static volatile int g_game_speed = 2;              /* 1 easy (20 ticks/s), 2 medium (25, default), 3 hard (30, original) */
static const int k_speed_ticks[4] = { 30, 20, 25, 30 };
static volatile int g_unlock_all;                  /* car.h: every track selectable ([race] tracks=all) */
#define TICK_BASE  33.0
#define TICK_MS    ((g_race_active && g_game_speed >= 1 && g_game_speed < 3) ? TICK_BASE * 30.0 / k_speed_ticks[g_game_speed] : TICK_BASE)
#define MAX_TRACK  4096

typedef float MAT4[4][4];
typedef HRESULT (WINAPI *PFN_FR_ADDXFORM)(void *f, int type, MAT4 m);
typedef HRESULT (WINAPI *PFN_FR_GETXFORM)(void *f, MAT4 m);
typedef HRESULT (WINAPI *PFN_FR_V3)(void *f, int a, float x, float y, float z);
typedef HRESULT (WINAPI *PFN_FR_ROT)(void *f, int type, float x, float y, float z, float t);
typedef HRESULT (WINAPI *PFN_FR_ORIENT)(void *f, void *ref, float dx, float dy, float dz,
                                        float ux, float uy, float uz);
typedef HRESULT (WINAPI *PFN_VP_OBJ)(void *vp, void *obj);
typedef HRESULT (WINAPI *PFN_VP_NOARG)(void *vp);

static PFN_FR_ADDXFORM g_fr_addtransform;
static PFN_FR_GETXFORM g_fr_gettransform;
static PFN_FR_V3 g_fr_addtranslation, g_fr_addscale, g_fr_setposition;
static PFN_FR_ROT g_fr_addrotation;
static PFN_FR_ORIENT g_fr_setorientation;
static PFN_VP_OBJ g_vp_render;
/* overview camera (tiles.h): swaps the camera in around each Render */
static void overview_begin(void *vp);
static void overview_end(void *vp);

/* world_tick: tick in which the frame was placed with SetPosition relative to the scene
 * or the world frame, i.e. its translation is relative to the floating origin. */
struct tracked { void *f; DWORD dirty_tick, valid_tick, world_tick; MAT4 prev, cur; };
/* Floating origin: 0x402620 sets origin = player position >> 3 (x at 0x44d3b8, z at
 * 0x44d3c0) and 0x403230 places everything at (world >> 3) - origin. When the origin
 * steps, every world-placed object jumps by the same amount in that tick; we shift
 * their previous transforms into the new origin so the step is never blended. */
#define ORIGIN_X (*(volatile int *)0x0044D3B8)
#define ORIGIN_Z (*(volatile int *)0x0044D3C0)
#define WORLD_FRAME (*(void **)0x0048899C)
static int g_last_ox, g_last_oz, g_have_origin;
static struct tracked g_trk[MAX_TRACK];
static int g_ntrk;
static void *g_dirty[MAX_TRACK];
static int g_ndirty;
static DWORD g_tick = 1;
static volatile int g_interp_on, g_internal, g_in_extra, g_race_seen, g_race_active;
static double g_tick_start;     /* ms, QPC */
/* Refresh lock: when the display refresh divides evenly into the ~33 ms tick (60, 90,
 * 120, 180, 240 Hz), run exactly one logic tick every N vsyncs and blend each frame at
 * exactly i/N. This removes the periodic hitch from the tick and the refresh drifting
 * apart. Other refresh rates keep the time-based blend. */
static volatile int g_lock_n;                /* 0 = time-based */
static volatile float g_alpha_fixed = -1.0f; /* >= 0: blend for the frame being drawn */
static int g_race_frames;                    /* extra frames since race start (warm-up) */

static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / f.QuadPart;
}

static struct tracked *trk_find(void *f, int add)
{
    int i;
    for (i = 0; i < g_ntrk; i++) if (g_trk[i].f == f) return &g_trk[i];
    if (!add) return NULL;
    if (g_ntrk >= MAX_TRACK) g_ntrk = 0;     /* table full: start over */
    memset(&g_trk[g_ntrk], 0, sizeof g_trk[0]);
    g_trk[g_ntrk].f = f;
    return &g_trk[g_ntrk++];
}

static void mark_dirty(void *f)
{
    struct tracked *t;
    if (g_internal || g_in_extra || !g_interp_on) return;
    t = trk_find(f, 1);
    if (t && t->dirty_tick != g_tick) {
        t->dirty_tick = g_tick;
        if (g_ndirty < MAX_TRACK) g_dirty[g_ndirty++] = f;
    }
}

static void interp_reset(void)
{
    g_ntrk = 0; g_ndirty = 0; g_race_active = 0; g_race_seen = 0; g_have_origin = 0;
}

static HRESULT WINAPI h_addtransform(void *f, int ty, MAT4 m) { mark_dirty(f); return g_fr_addtransform(f, ty, m); }
static HRESULT WINAPI h_addtranslation(void *f, int ty, float x, float y, float z) { mark_dirty(f); return g_fr_addtranslation(f, ty, x, y, z); }
static HRESULT WINAPI h_addscale(void *f, int ty, float x, float y, float z) { mark_dirty(f); return g_fr_addscale(f, ty, x, y, z); }
static HRESULT WINAPI h_setposition(void *f, int ref, float x, float y, float z)
{
    mark_dirty(f);
    if (!g_internal && !g_in_extra && g_interp_on &&
        ((void *)ref == *(void **)0x0054AF08 || ((void *)ref == WORLD_FRAME && ref))) {
        struct tracked *t = trk_find(f, 0);
        if (t) t->world_tick = g_tick;
    }
    return g_fr_setposition(f, ref, x, y, z);
}
static HRESULT WINAPI h_addrotation(void *f, int ty, float x, float y, float z, float t) { mark_dirty(f); return g_fr_addrotation(f, ty, x, y, z, t); }
static HRESULT WINAPI h_setorientation(void *f, void *ref, float dx, float dy, float dz, float ux, float uy, float uz)
{ mark_dirty(f); return g_fr_setorientation(f, ref, dx, dy, dz, ux, uy, uz); }

static void lerp_mat(MAT4 out, MAT4 a, MAT4 b, float t)
{
    int r, c;
    for (r = 0; r < 4; r++) {
        float la = 0, lb = 0, lo = 0;
        for (c = 0; c < 4; c++) out[r][c] = a[r][c] + (b[r][c] - a[r][c]) * t;
        if (r < 3) {   /* keep each basis row's length (lerping rotations shrinks them) */
            for (c = 0; c < 3; c++) { la += a[r][c] * a[r][c]; lb += b[r][c] * b[r][c]; lo += out[r][c] * out[r][c]; }
            if (lo > 1e-12f) {
                float want = (float)(sqrt(la) + (sqrt(lb) - sqrt(la)) * t), k = want / (float)sqrt(lo);
                for (c = 0; c < 3; c++) out[r][c] *= k;
            }
        }
    }
}

/* debug (MMCDA_INTERPDIAG, Vulkan): after each real tick, find frames whose local transform
   changed although none of the hooked calls touched them (so they are not blended) */
static void interp_diag_walk(vframe *f, int depth, int *moved, int *untracked)
{
    static struct { vframe *f; MAT m; DWORD tick; int logged; } seen[4096];
    static int nseen;
    int i;
    if (!f || depth > 32) return;
    for (i = 0; i < nseen && seen[i].f != f; i++) ;
    if (i == nseen && nseen < 4096) { seen[nseen].f = f; memcpy(seen[nseen].m, f->m, sizeof(MAT)); seen[nseen].tick = g_tick; seen[nseen].logged = 0; nseen++; }
    else if (i < nseen) {
        if (memcmp(seen[i].m, f->m, sizeof(MAT))) {
            struct tracked *t = trk_find(f, 0);
            (*moved)++;
            if (!t || t->dirty_tick != g_tick) {
                (*untracked)++;
                if (seen[i].logged < 2) {
                    seen[i].logged++;
                    log_msg("interpdiag: frame %p untracked move (depth %d, %d visuals, %d kids, rot %.3f, pos %.1f %.1f %.1f -> %.1f %.1f %.1f)",
                            (void *)f, depth, f->visuals.n, f->kids.n, f->rot_theta, seen[i].m[3][0], seen[i].m[3][1], seen[i].m[3][2],
                            f->m[3][0], f->m[3][1], f->m[3][2]);
                }
            }
            memcpy(seen[i].m, f->m, sizeof(MAT));
        }
    }
    for (i = 0; i < f->kids.n; i++) interp_diag_walk((vframe *)f->kids.p[i], depth + 1, moved, untracked);
}

/* viewport->Render(scene): capture after a logic tick, blend, render, restore */
static void vr_capture(void);                            /* vrexport.h */
static void (*g_census_frame)(void);                     /* census.h, game thread */
static HRESULT WINAPI h_vp_render(void *vp, void *scene)
{
    HRESULT hr;
    int i, n = 0;
    float alpha;
    static struct tracked *list[MAX_TRACK];
    if (g_census_frame) g_census_frame();
    if (g_race_active && !g_in_extra) vr_capture();       /* logic done, nothing blended yet */
    if (!g_interp_on || !g_race_active) {
        overview_begin(vp);
        hr = g_vp_render(vp, scene);
        overview_end(vp);
        return hr;
    }

    g_internal = 1;
    if (!g_in_extra) {
        /* real tick: record the new transforms of everything the logic moved */
        int ox = ORIGIN_X, oz = ORIGIN_Z;
        float dox = g_have_origin ? (float)(ox - g_last_ox) : 0.0f;
        float doz = g_have_origin ? (float)(oz - g_last_oz) : 0.0f;
        if (dox != 0.0f || doz != 0.0f) log_msg("origin step %.0f,%.0f", dox, doz);
        g_last_ox = ox; g_last_oz = oz; g_have_origin = 1;
        /* Pooled object frames (records of 9 ints at 0x486070, 60 of them) are released
           and handed out again every tick (0x4024b0), so one frame can be a different
           object from one tick to the next. The game hands them out in a fixed order per
           object type (tile objects by grid cell, cars hit by a weapon by car index), so a
           frame keeps its object while that set is unchanged. Blend a pooled frame only if
           it was used last tick and moved less than 40 units (a car moves ~19 per tick; a
           different object of the same type is at least a tile, 127 units, away). */
        void *pool[60];
        int np = 0, k;
        for (k = 0; k < 60; k++) { void *pf = *(void **)(0x00486070 + k * 0x24); if (pf) pool[np++] = pf; }
        for (i = 0; i < g_ndirty; i++) {
            struct tracked *t = trk_find(g_dirty[i], 0);
            int had = t && t->valid_tick == g_tick - 1;
            if (!t) continue;
            for (k = 0; k < np && pool[k] != t->f; k++) ;
            if (k < np) {
                MAT4 old;
                memcpy(old, t->cur, sizeof(MAT4));
                g_fr_gettransform(t->f, t->cur);
                if (had) {
                    float dx, dy, dz;
                    if (t->world_tick == g_tick) { old[3][0] -= dox; old[3][2] -= doz; }
                    dx = t->cur[3][0] - old[3][0]; dy = t->cur[3][1] - old[3][1]; dz = t->cur[3][2] - old[3][2];
                    if (dx * dx + dy * dy + dz * dz < 40.0f * 40.0f) memcpy(t->prev, old, sizeof(MAT4));
                    else memcpy(t->prev, t->cur, sizeof(MAT4));
                } else memcpy(t->prev, t->cur, sizeof(MAT4));
                t->valid_tick = g_tick;
                continue;
            }
            if (had) {
                memcpy(t->prev, t->cur, sizeof(MAT4));
                if (t->world_tick == g_tick) {   /* express last position in the new origin */
                    t->prev[3][0] -= dox;
                    t->prev[3][2] -= doz;
                }
            }
            g_fr_gettransform(t->f, t->cur);
            if (!had) memcpy(t->prev, t->cur, sizeof(MAT4));
            else {   /* teleport (respawn, camera cut): don't blend */
                float dx = t->cur[3][0] - t->prev[3][0], dy = t->cur[3][1] - t->prev[3][1],
                      dz = t->cur[3][2] - t->prev[3][2];
                if (dx * dx + dy * dy + dz * dz > 60.0f * 60.0f) memcpy(t->prev, t->cur, sizeof(MAT4));
            }
            t->valid_tick = g_tick;
        }
    }
    if (!g_in_extra && g_render_vulkan && GetEnvironmentVariableA("MMCDA_INTERPDIAG", NULL, 0)) {
        static DWORD last; static int tm, tu, ticks;
        int moved = 0, untracked = 0;
        interp_diag_walk((vframe *)scene, 0, &moved, &untracked);
        tm += moved; tu += untracked; ticks++;
        if (GetTickCount() - last > 5000) {
            last = GetTickCount();
            log_msg("interpdiag: %d ticks, %.1f frames moved per tick, %.1f of them untracked", ticks, (double)tm / ticks, (double)tu / ticks);
            tm = tu = ticks = 0;
        }
    }
    if (g_alpha_fixed >= 0.0f) {
        alpha = g_alpha_fixed;                   /* refresh-locked: exact i/N */
    } else {   /* blend for when this frame will be on screen: the next vsync */
        double now = now_ms(), shown = g_last_present_ms + g_present_dt;
        while (shown < now) shown += g_present_dt;
        if (shown - now > g_present_dt * 1.5) shown = now;
        alpha = (float)((shown - g_tick_start) / TICK_MS);
    }
    if (alpha < 0) alpha = 0;
    if (alpha > 1) alpha = 1;
    for (i = 0; i < g_ntrk; i++) {
        struct tracked *t = &g_trk[i];
        MAT4 m;
        /* only frames the logic touched in the latest tick (so they are still alive) */
        if (t->valid_tick != g_tick || t->dirty_tick != g_tick) continue;
        if (!memcmp(t->prev, t->cur, sizeof(MAT4))) continue;
        lerp_mat(m, t->prev, t->cur, alpha);
        g_fr_addtransform(t->f, 0 /* D3DRMCOMBINE_REPLACE */, m);
        list[n++] = t;
    }
    overview_begin(vp);
    hr = g_vp_render(vp, scene);
    overview_end(vp);
    for (i = 0; i < n; i++) g_fr_addtransform(list[i]->f, 0, list[i]->cur);
    g_internal = 0;
    return hr;
}

/* One frame of 0x42a880 without the limiter, scene->Move(1.0) or game logic. */
typedef int (__cdecl *PFN_GAME0)(void);
typedef int (__cdecl *PFN_GAME1)(int);
static int render_extra_frame(void)
{
    void *vp = *(void **)0x0054AF1C, *scene = *(void **)0x0054AF08, *dev = *(void **)0x0054AF18;
    int ok = 0;
    if (!vp || !scene || !dev || !g_vp_render || !G_I32(0x0061764C)) return 0;
    if (!((PFN_GAME0)0x004060F0)() || !((PFN_GAME0)0x00406240)()) return 0;
    g_in_extra = 1;
    if (((PFN_VP_NOARG)(*(void ***)vp)[VP_CLEAR])(vp) == 0) {
        if (G_I32(0x0062FAFA) && G_I32(0x00504FB4)) {   /* same HUD block as 0x42a880 */
            int s1 = G_I32(0x00503828), s2 = G_I32(0x0050382C), s3 = G_I32(0x00503830), s4 = G_I32(0x00504FA4);
            G_I32(0x00503828) = G_I32(0x0061D3E8); G_I32(0x0050382C) = 0; G_I32(0x00503830) = 0; G_I32(0x00504FA4) = 1;
            ((PFN_GAME1)0x004075B0)(1);
            ((PFN_GAME0)0x004074C0)();
            G_I32(0x00503828) = s1; G_I32(0x0050382C) = s2; G_I32(0x00503830) = s3; G_I32(0x00504FA4) = s4;
        }
        if (((PFN_VP_OBJ)(*(void ***)vp)[VP_RENDER])(vp, scene) == 0 &&
            ((PFN_GAME0)0x00406CA0)() &&
            ((PFN_VP_NOARG)(*(void ***)dev)[DEV_UPDATE])(dev) == 0) {
            ((PFN_GAME1)0x004058A0)(0);
            ok = 1;
        }
    }
    g_in_extra = 0;
    return ok;
}

/* 0x406b70: the 33 ms frame limiter. Fill the wait with interpolated frames. */
static PFN_VOIDINT g_limiter_orig;
static int __cdecl limiter_hook(void)
{
    double now;
    /* race is active if the race camera ran during the last tick (kept up to date even
       with smoothing off: the controller and car code use it) */
    g_race_active = g_race_seen;
    g_race_seen = 0;
    if (!g_interp_on) {
        /* no smoothing: still honour a slower game speed by waiting out the longer tick */
        static double last;
        double t = TICK_MS;
        if (t > TICK_BASE + 1) { while (now_ms() - last < t - TICK_BASE) Sleep(1); }
        last = now_ms();
        return g_limiter_orig();
    }
    if (!g_race_active) { g_race_frames = 0; g_lock_n = 0; }

    /* after ~1 s of racing, decide whether the refresh rate can be locked to the tick */
    if (g_race_active && g_race_frames > 120 && g_lock_n == 0 && g_race_frames < 1000000) {
        double dt = g_present_dt;
        int n = (int)(TICK_MS / dt + 0.5);
        /* Hard: the tick must match n vsyncs (0.7 ms). Easy / Medium: they are already a
           chosen slow-down, so snap to the nearest whole number of vsyncs if within 10%
           (120 Hz: 25 ticks -> 5 vsyncs = 24/s, 20 ticks -> 6 vsyncs) - even frame pacing
           matters more than the exact rate. */
        double tol = TICK_MS > TICK_BASE + 1 ? TICK_MS * 0.10 : 0.7;
        if (n >= 2 && n <= 12 && fabs(n * dt - TICK_MS) < tol) {
            g_lock_n = n;
            log_msg("refresh %.2f Hz: locking one tick to %d vsyncs (%.2f ms tick)", 1000.0 / dt, n, n * dt);
        } else {
            g_race_frames = 1000000;             /* checked once; stay time-based */
            log_msg("refresh %.2f Hz: time-based blending", 1000.0 / dt);
        }
    }

    if (g_lock_n >= 2 && g_race_active && !g_transition) {
        int i, ok = 1;
        double t0 = now_ms();
        for (i = 1; i < g_lock_n && ok; i++) {
            g_alpha_fixed = (float)i / (float)g_lock_n;
            ok = render_extra_frame();
            g_race_frames++;
        }
        /* safety: if vsync is not actually blocking, never run logic faster than ~30 Hz */
        if (now_ms() - t0 < (g_lock_n - 1) * g_present_dt * 0.5) {
            g_lock_n = 0; g_race_frames = 1000000;
            log_msg("vsync not blocking: back to time-based blending");
        }
        /* the tick's own frame presents on the next vsync: wait to just before it */
        while (now_ms() - g_tick_start < g_lock_n * g_present_dt - 1.5) Sleep(0);
        g_alpha_fixed = 0.0f;                    /* the tick's own frame */
        g_tick_start = now_ms();
        g_tick++;
        g_ndirty = 0;
        G_I32(0x00504FC4) = (int)(timeGetTime() - 0x21);
        return g_limiter_orig();
    }
    g_alpha_fixed = -1.0f;
    for (;;) {
        now = now_ms();
        if (now - g_tick_start >= TICK_MS) break;
        if (!g_race_active || g_transition || !render_extra_frame()) {
            if (TICK_MS - (now - g_tick_start) > 2.0) Sleep(1);
        } else if (g_race_frames++, now_ms() - now < 1.0) {
            Sleep(1);   /* present didn't block (no vsync): don't spin the GPU */
        }
    }
    g_tick_start = (now - g_tick_start > TICK_MS * 3) ? now : g_tick_start + TICK_MS;
    g_tick++;
    g_ndirty = 0;
    G_I32(0x00504FC4) = (int)(timeGetTime() - 0x21);   /* original limiter: no further wait */
    return g_limiter_orig();
}

static void hook_vtable_slot(void **vt, int slot, void *hook, void **orig)
{
    DWORD old;
    if (vt[slot] == hook) return;
    if (!VirtualProtect(&vt[slot], sizeof(void *), PAGE_READWRITE, &old)) return;
    *orig = vt[slot];
    vt[slot] = hook;
    VirtualProtect(&vt[slot], sizeof(void *), old, &old);
}

/* called when the race viewport exists (after scene/camera frames are created) */
/* Viewport Clear only clears (colour and z) where D3DRM tracked drawing in earlier
   frames. Anything it missed keeps stale depth, so geometry there fails the depth test
   and shows black or old pixels in 32-pixel blocks (flicker). Mark the whole viewport
   dirty first so every clear is complete. */
#define VP_FORCEUPDATE 23
#define VP_GETWIDTH    32
#define VP_GETHEIGHT   33
typedef HRESULT (WINAPI *PFN_VP_FORCE)(void *vp, DWORD x1, DWORD y1, DWORD x2, DWORD y2);
typedef DWORD (WINAPI *PFN_VP_GETDIM)(void *vp);
static PFN_VP_NOARG g_vp_clear;
static DWORD g_clear_rgb;          /* 0x00RRGGBB the hard clear fills with (sky colour, cams.h) */
static HRESULT WINAPI h_vp_clear(void *vp)
{
    void **vvt = *(void ***)vp;
    DWORD w = ((PFN_VP_GETDIM)vvt[VP_GETWIDTH])(vp), h = ((PFN_VP_GETDIM)vvt[VP_GETHEIGHT])(vp);
    void *bb = *(void **)0x004891E4;
    if (w && h) ((PFN_VP_FORCE)vvt[VP_FORCEUPDATE])(vp, 0, 0, w - 1, h - 1);
    if (bb) {   /* hard clear: colour fill the back buffer, depth fill its z-buffer */
        typedef HRESULT (WINAPI *PFN_BLT)(void *, RECT *, void *, RECT *, DWORD, void *);
        typedef HRESULT (WINAPI *PFN_GETATT)(void *, DWORD *, void **);
        BYTE fx[100];
        DWORD caps[1] = { 0x00020000 };           /* DDSCAPS_ZBUFFER */
        void *z = NULL, **bvt = *(void ***)bb;
        static int logged;
        HRESULT r1, r2 = -1;
        memset(fx, 0, sizeof fx); *(DWORD *)fx = sizeof fx;
        if (g_clear_rgb) {   /* fill colour in the back buffer's pixel format */
            static void *fmt_bb; static int fmt_bpp;
            if (fmt_bb != bb) {
                BYTE d[0x6C]; memset(d, 0, sizeof d); *(DWORD *)d = sizeof d;
                fmt_bpp = ((HRESULT (WINAPI *)(void *, void *))bvt[22])(bb, d) == 0 ? *(int *)(d + 0x54) : 16;
                fmt_bb = bb;
            }
            *(DWORD *)(fx + 0x50) = fmt_bpp == 16
                ? (((g_clear_rgb >> 8) & 0xF800) | ((g_clear_rgb >> 5) & 0x07E0) | ((g_clear_rgb >> 3) & 0x001F))
                : g_clear_rgb;
        }
        r1 = ((PFN_BLT)bvt[5])(bb, NULL, NULL, NULL, 0x01000400 /* WAIT|COLORFILL */, fx);
        if (((PFN_GETATT)bvt[12])(bb, caps, &z) == 0 && z) {
            *(DWORD *)(fx + 0x50) = 0xFFFFFFFF;
            r2 = ((PFN_BLT)(*(void ***)z)[5])(z, NULL, NULL, NULL, 0x01000000 | 0x02000000 /* WAIT|DEPTHFILL */, fx);
            ((ULONG (WINAPI *)(void *))(*(void ***)z)[2])(z);
        }
        if (!logged) { log_msg("hard clear: colour fill %08lx, depth fill %08lx (z %p)", r1, r2, z); logged = 1; }
    }
    return g_vp_clear(vp);
}

static void install_interp_vtables(void *vp)
{
    static int done;
    void **fvt, *scene = *(void **)0x0054AF08;
    if (done || !g_interp_on || !scene) return;
    fvt = *(void ***)scene;
    g_fr_gettransform = (PFN_FR_GETXFORM)fvt[FR_GETTRANSFORM];
    hook_vtable_slot(fvt, FR_ADDTRANSFORM, (void *)h_addtransform, (void **)&g_fr_addtransform);
    hook_vtable_slot(fvt, FR_ADDTRANSLATION, (void *)h_addtranslation, (void **)&g_fr_addtranslation);
    hook_vtable_slot(fvt, FR_ADDSCALE, (void *)h_addscale, (void **)&g_fr_addscale);
    hook_vtable_slot(fvt, FR_ADDROTATION, (void *)h_addrotation, (void **)&g_fr_addrotation);
    hook_vtable_slot(fvt, FR_SETORIENTATION, (void *)h_setorientation, (void **)&g_fr_setorientation);
    hook_vtable_slot(fvt, FR_SETPOSITION, (void *)h_setposition, (void **)&g_fr_setposition);
    hook_vtable_slot(*(void ***)vp, VP_RENDER, (void *)h_vp_render, (void **)&g_vp_render);
    if (!GetEnvironmentVariableA("MMCDA_NOFULLCLEAR", NULL, 0))
        hook_vtable_slot(*(void ***)vp, VP_CLEAR, (void *)h_vp_clear, (void **)&g_vp_clear);
    done = 1;
    log_msg("interpolation vtable hooks installed");
}

static void install_interp(void)
{
    static const BYTE lim_expect[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x28 };
    char v[2];
    if (GetEnvironmentVariableA("MMCDA_NOINTERP", v, sizeof v)) return;
    if (memcmp((BYTE *)0x00406B70, lim_expect, 6) != 0) { log_msg("limiter site mismatch, interpolation off"); return; }
    timeBeginPeriod(1);
    g_limiter_orig = (PFN_VOIDINT)make_trampoline(lim_expect, 6, (BYTE *)0x00406B76);
    if (g_limiter_orig && patch_jmp((BYTE *)0x00406B70, lim_expect, 6, limiter_hook)) {
        g_interp_on = 1;
        g_tick_start = now_ms();
        log_msg("frame interpolation installed");
    }
}
