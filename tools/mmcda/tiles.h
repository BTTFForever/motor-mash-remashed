/* ==== Whole-level track rendering =====================================================
 * Included by mmcda.c after interp.h.
 *
 * The game draws the track through a sliding 8x8 window of tile frames (0x488898),
 * rebuilt every tick by the per-tick callback at 0x4016d0:
 *     0x4024b0 strip window visuals; 0x402620 floating origin;
 *     0x402e20 place window + attach meshes; 0x403230 cars/objects/camera
 * A map cell (row R, col C) ends up at  x = 127*C - origin_x + 63,
 *                                       z = -127*R - origin_z - 63   (world frame 0x48899c)
 * We build one frame per map cell once, as children of a container frame placed at
 * (63 - origin_x, 0, -63 - origin_z) each tick, so only the container moves. Cells
 * the game's window currently covers are hidden (no double draw). Special tiles
 * (definition flag bit 1: animated / object tiles) are left to the game.
 *
 * Map: ushort array at *(0x465638): [0]=width, [1]=height, [4]=edge mode, cells from
 * index 8, row-major. Cell value: bits 0-11 tile id, 12-13 height step (x 0x485f60),
 * 14-15 rotation. Tile defs: *(0x485bd0) + id*12, byte +4 bit 0 = special.
 * Meshes: 0x488578[id]. Window cell offsets: col 0x485f64, row 0x485f68.
 * mmcda.ini [track] radius = all | N (cells beyond the window to draw, 0 = original).
 */
#define TILE_SIZE 127
#define D3DRM_CREATEFRAME 4
#define FR_ADDVISUAL      18
#define FR_DELETEVISUAL   41
#define FR_RELEASE         2

typedef HRESULT (WINAPI *PFN_RM_CREATEFRAME)(void *rm, void *parent, void **out);
typedef HRESULT (WINAPI *PFN_FR_VISUAL)(void *f, void *visual);
typedef ULONG (WINAPI *PFN_RELEASE)(void *obj);

struct tcell { void *frame, *mesh; short r, c; char shown; };
static struct tcell *g_cells;
static int g_ncells, g_tiles_radius = -1;   /* -1 = whole map */
static void *g_tile_container, *g_tiles_map, *g_tiles_world;
static int g_tiles_wc = 0x7fffffff, g_tiles_wr = 0x7fffffff;

#define MAP_PTR   (*(unsigned short **)0x00465638)
#define TILE_DEFS (*(BYTE **)0x00485BD0)
#define TILE_MESH ((void **)0x00488578)
#define WIN_COL   (*(volatile int *)0x00485F64)
#define WIN_ROW   (*(volatile int *)0x00485F68)
#define HEIGHT_STEP (*(volatile int *)0x00485F60)

static void tiles_release(void)
{
    int i;
    for (i = 0; i < g_ncells; i++)
        if (g_cells[i].frame) ((PFN_RELEASE)(*(void ***)g_cells[i].frame)[FR_RELEASE])(g_cells[i].frame);
    free(g_cells); g_cells = NULL; g_ncells = 0;
    if (g_tile_container) ((PFN_RELEASE)(*(void ***)g_tile_container)[FR_RELEASE])(g_tile_container);
    g_tile_container = NULL; g_tiles_map = NULL; g_tiles_world = NULL;
    g_tiles_wc = g_tiles_wr = 0x7fffffff;
}

static void tiles_build(void)
{
    void *rm = *(void **)0x0054AF00, *world = WORLD_FRAME;
    unsigned short *map = MAP_PTR;
    BYTE *defs = TILE_DEFS;
    int w, h, r, c, n = 0;
    void **fvt;
    if (!rm || !world || !map || !defs) return;
    w = map[0]; h = map[1];
    if (w <= 0 || h <= 0 || w > 512 || h > 512) return;
    if (((PFN_RM_CREATEFRAME)(*(void ***)rm)[D3DRM_CREATEFRAME])(rm, world, &g_tile_container) != 0) return;
    g_cells = (struct tcell *)calloc((size_t)w * h, sizeof *g_cells);
    if (!g_cells) return;
    g_internal = 1;                              /* keep these out of the interpolation tracker */
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++) {
            unsigned v = map[8 + r * w + c], id = v & 0xFFF, rot = v & 0xC000;
            void *f = NULL, *mesh;
            if (defs[id * 12 + 4] & 1) continue;  /* special tile: the game handles it */
            mesh = TILE_MESH[id];
            if (!mesh) continue;
            if (((PFN_RM_CREATEFRAME)(*(void ***)rm)[D3DRM_CREATEFRAME])(rm, g_tile_container, &f) != 0) continue;
            fvt = *(void ***)f;
            ((PFN_FR_V3)fvt[FR_SETPOSITION])(f, (int)(INT_PTR)g_tile_container, (float)(TILE_SIZE * c),
                                             (float)(((v >> 12) & 3) * HEIGHT_STEP), (float)(-TILE_SIZE * r));
            /* same orientations as 0x402e20 */
            if (rot == 0x4000)      ((PFN_FR_ORIENT)fvt[FR_SETORIENTATION])(f, g_tile_container, 1, 0, 0, 0, 1, 0);
            else if (rot == 0x8000) ((PFN_FR_ORIENT)fvt[FR_SETORIENTATION])(f, g_tile_container, 0, 0, -1, 0, 1, 0);
            else if (rot == 0xC000) ((PFN_FR_ORIENT)fvt[FR_SETORIENTATION])(f, g_tile_container, -1, 0, 0, 0, 1, 0);
            else                    ((PFN_FR_ORIENT)fvt[FR_SETORIENTATION])(f, g_tile_container, 0, 0, 1, 0, 1, 0);
            g_cells[n].frame = f; g_cells[n].mesh = mesh;
            g_cells[n].r = (short)r; g_cells[n].c = (short)c; g_cells[n].shown = 0;
            n++;
        }
    g_internal = 0;
    g_ncells = n;
    g_tiles_map = map; g_tiles_world = world;
    log_msg("track: %dx%d map, %d tile frames built (radius %s)", w, h, n,
            g_tiles_radius < 0 ? "all" : "limited");
}

/* show cells outside the game's 8x8 window (and within the radius); hide the rest */
static void tiles_update_visibility(void)
{
    int i, wc = WIN_COL, wr = WIN_ROW;
    if (wc == g_tiles_wc && wr == g_tiles_wr) return;
    g_tiles_wc = wc; g_tiles_wr = wr;
    g_internal = 1;
    for (i = 0; i < g_ncells; i++) {
        struct tcell *t = &g_cells[i];
        int lc = t->c - wc, lr = t->r - wr;      /* cell position in window coordinates */
        int in_window = lc >= 0 && lc < 8 && lr >= 0 && lr < 8;
        int dist = max(max(-lc, lc - 7), max(-lr, lr - 7));
        int want = !in_window && (g_tiles_radius < 0 || dist <= g_tiles_radius);
        if (want != t->shown) {
            void **fvt = *(void ***)t->frame;
            if (want) ((PFN_FR_VISUAL)fvt[FR_ADDVISUAL])(t->frame, t->mesh);
            else      ((PFN_FR_VISUAL)fvt[FR_DELETEVISUAL])(t->frame, t->mesh);
            t->shown = (char)want;
        }
    }
    g_internal = 0;
}

/* ---- animated / special map tiles outside the window --------------------------------
 * Replica of the special-tile branch of 0x402e20 for cells the window doesn't cover.
 * Each special tile type has one pool record per map occurrence (created in 0x402380):
 * records of 9 ints at 0x486070 ([0] = frame, [8] = in-use flag), first record of a
 * type at 0x488248[id]. 0x4024b0 releases all records every tick, so we attach ours
 * each tick exactly like the game does. Slot search is bounded to the type's own range. */
#define POOL_REC(i)   ((int *)(0x00486070 + (i) * 0x24))
#define POOL_START    ((int *)0x00488248)
#define POOL_TOTAL    60
#define FR_ADDCHILD   11
typedef HRESULT (WINAPI *PFN_FR_OBJ)(void *f, void *child);

#define POOL_TYPES    200          /* 0x488248 .. 0x488568, indexed by tile id */

static int pool_type_end(int start)  /* smallest other type's start index above ours */
{
    int i, end = POOL_TOTAL;
    for (i = 0; i < POOL_TYPES; i++) {
        int s = POOL_START[i];
        if (s > start && s < end) end = s;
    }
    return end;
}

/* Animated objects (windmills, snowmen ...) are D3DRM AnimationSets (0x429fd0): table
   0x54d310 holds {animation set, time} per object, record[5] is the index, and a frame
   move callback (0x42a170) calls SetTime(time) and adds the frame delta each Move.
   That callback only reaches objects inside the game's own 8x8 window, so the ones we
   place outside it stood still. If an object's clock didn't move since last tick, run
   the callback's work here: one tick = 1.0, like scene->Move(1.0). */
static float g_anim_last[256];
static void tiles_animate(int *rec)
{
    int ai = rec[5];
    float *t;
    void *set;
    {
        static int nlog;
        if (nlog++ < 12) log_msg("anim: rec %p ai %d set %p t %.1f", rec, ai, ai >= 0 && ai < 256 ? *(void **)(0x0054D310 + ai * 8) : 0,
                                 ai >= 0 && ai < 256 ? *(float *)(0x0054D314 + ai * 8) : -1.0f);
    }
    if (ai < 0 || ai >= 256) return;
    set = *(void **)(0x0054D310 + ai * 8);
    t = (float *)(0x0054D314 + ai * 8);
    if (!set) return;
    if (*t == g_anim_last[ai]) {
        ((HRESULT (WINAPI *)(void *, float))(*(void ***)set)[14])(set, *t);   /* SetTime */
        *t += 1.0f;
    }
    g_anim_last[ai] = *t;
}

static void tiles_place_specials(void)
{
    unsigned short *map = MAP_PTR;
    BYTE *defs = TILE_DEFS;
    void *world = WORLD_FRAME;
    int w, h, r, c, wc = WIN_COL, wr = WIN_ROW, ox = ORIGIN_X, oz = ORIGIN_Z;
    if (!map || !defs || !world) return;
    w = map[0]; h = map[1];
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++) {
            int idx = r * w + c + 8, lc = c - wc, lr = r - wr, slot, end, *rec;
            unsigned v = map[idx], id = v & 0xFFF, rot = v & 0xC000;
            BYTE *def = defs + id * 12;
            void *f, **fvt;
            float ang;
            if (!(def[4] & 1)) continue;                                  /* not special */
            if (lc >= 0 && lc < 8 && lr >= 0 && lr < 8) continue;          /* game draws it */
            if (g_tiles_radius >= 0 && max(max(-lc, lc - 7), max(-lr, lr - 7)) > g_tiles_radius) continue;
            if (id >= POOL_TYPES) continue;
            slot = POOL_START[id];
            if (slot < 0 || slot >= POOL_TOTAL) continue;
            end = pool_type_end(slot);
            while (slot < end && POOL_REC(slot)[8] != 0) slot++;
            if (slot >= end) continue;                                     /* type exhausted */
            rec = POOL_REC(slot);
            if (!rec[0]) continue;
            if (*(int *)(def + 8) == 0xC) {                                /* per-cell animation table */
                int *tab = (int *)0x00630A80, k;
                for (k = 0; tab[k * 2] != -1; k++)
                    if (tab[k * 2] == idx - 8) { ((void (__cdecl *)(int *, int))0x0042A210)(rec, tab[k * 2 + 1]); break; }
            } else ((void (__cdecl *)(int *, int))0x0042A210)(rec, -1);
            ((PFN_FR_OBJ)(*(void ***)world)[FR_ADDCHILD])(world, (void *)rec[0]);
            rec[8] = 1;
            tiles_animate(rec);
            f = (void *)rec[0]; fvt = *(void ***)f;
            ang = rot == 0x4000 ? 1.5707964f : rot == 0x8000 ? 3.1415927f : rot == 0xC000 ? 4.712389f : 0.0f;
            ((PFN_FR_ROT)fvt[FR_ADDROTATION])(f, 0, 0, 1, 0, ang);
            ((PFN_FR_V3)fvt[FR_ADDSCALE])(f, 2, 0.125f, 0.125f, 0.125f);
            ((PFN_FR_V3)fvt[FR_SETPOSITION])(f, (int)(INT_PTR)world,
                (float)(TILE_SIZE * c - ox + 63), (float)(((v >> 12) & 3) * HEIGHT_STEP),
                (float)(-TILE_SIZE * r - oz - 63));
        }
}

/* ---- moving objects: 0x402dd0 visibility test (orig: within +-0x200 of the origin) -- */
static int __cdecl object_visible_hook(int x, int z)
{
    int lim = g_tiles_radius < 0 ? 0x7FFFFFFF : 0x200 + g_tiles_radius * TILE_SIZE;
    int dx = x - ORIGIN_X, dz = z - ORIGIN_Z;
    return dx < lim && -lim < dx && dz < lim && -lim < dz;
}

/* ---- overview camera (F8) ---------------------------------------------------------------
 * Applied only around viewport Render: the game's camera frame (0x54af0c, child of the
 * scene 0x54af08) is saved, replaced by a straight-down view from above the map centre
 * high enough for the whole map to fit, rendered, and restored, so game logic never
 * sees it. Height comes from the per-frame projection the race loop sets
 * (SetPlane +-0.52*hscale horizontally, +-0.684 or +-0.57 vertically at the front
 * plane distance, which is 1 by default). Far/near clip are adjusted for this frame
 * and restored. */
#define VP_SETFRONT 14
#define VP_GETFRONT 36
#define VP_GETBACK  35
typedef float (WINAPI *PFN_VP_GETVAL)(void *vp);
static MAT4 g_ov_saved;
static void freecam_apply(void *cam, void *scene);       /* debugmenu.h */
static void debugmenu_race_tick(void);                   /* debugmenu.h */
static float g_ov_front, g_ov_back;
static int g_ov_active;
static void views_input(void);                                    /* cams.h */
static int views_pose(void *cam, void *scene, float *front, float *back, float *zoom);
static volatile int g_view;

static void overview_begin(void *vp)
{
    void *cam = *(void **)0x0054AF0C, *scene = *(void **)0x0054AF08, **cvt, **vvt;
    unsigned short *map = MAP_PTR;
    float hp, vpl, mw, md, cx, cz, y, ywide, ytall;
    int rotate;
    g_ov_active = 0;
    views_input();
    if (!g_overview && g_cam_mode == 0 && g_view != 0 && g_race_active && cam && scene && g_fr_gettransform) {
        /* extra views (cams.h): pose the camera for this frame, set clips and projection */
        float fr, bk, zm;
        vvt = *(void ***)vp;
        g_internal = 1;
        g_fr_gettransform(cam, g_ov_saved);
        if (views_pose(cam, scene, &fr, &bk, &zm)) {
            hp = 0.52f * g_hscale;
            vpl = (*(volatile short *)0x006302BC) ? 0.57f : 0.684f;
            g_ov_front = ((PFN_VP_GETVAL)vvt[VP_GETFRONT])(vp);
            g_ov_back = ((PFN_VP_GETVAL)vvt[VP_GETBACK])(vp);
            ((PFN_SETVAL)vvt[VP_SETBACK])(vp, bk);
            ((PFN_SETVAL)vvt[VP_SETFRONT])(vp, fr);
            g_setplane_orig(vp, -hp * fr * zm, hp * fr * zm, -vpl * fr * zm, vpl * fr * zm);
            g_ov_active = 1;                                      /* overview_end restores */
        }
        g_internal = 0;
        return;
    }
    if ((!g_overview && g_cam_mode != 2) || !g_race_active || !cam || !scene || !map || !g_fr_gettransform) return;
    cvt = *(void ***)cam; vvt = *(void ***)vp;
    if (g_cam_mode == 2) {                       /* free-fly: own pose, longer far clip */
        g_internal = 1;
        g_fr_gettransform(cam, g_ov_saved);
        freecam_apply(cam, scene);
        g_ov_back = ((PFN_VP_GETVAL)vvt[VP_GETBACK])(vp);
        ((PFN_SETVAL)vvt[VP_SETBACK])(vp, 12000.0f);
        g_internal = 0;
        g_ov_active = 2;
        return;
    }
    hp = 0.52f * g_hscale;
    vpl = (*(volatile short *)0x006302BC) ? 0.57f : 0.684f;
    mw = (float)(TILE_SIZE * map[0]);            /* world X extent */
    md = (float)(TILE_SIZE * map[1]);            /* world Z extent */
    /* screen-horizontal = X (north up), or rotated 90 degrees if that fits better */
    ywide = max((mw / 2) / hp, (md / 2) / vpl);
    ytall = max((md / 2) / hp, (mw / 2) / vpl);
    rotate = ytall < ywide;
    y = (rotate ? ytall : ywide) * 1.06f;
    cx = mw / 2 - (float)ORIGIN_X + 63.0f - TILE_SIZE / 2.0f;
    cz = -md / 2 - (float)ORIGIN_Z - 63.0f + TILE_SIZE / 2.0f;

    g_internal = 1;
    g_fr_gettransform(cam, g_ov_saved);
    g_fr_setposition(cam, (int)(INT_PTR)scene, cx, y, cz);
    if (rotate) g_fr_setorientation(cam, scene, 0, -1, 0, 1, 0, 0);
    else        g_fr_setorientation(cam, scene, 0, -1, 0, 0, 0, 1);
    g_ov_front = ((PFN_VP_GETVAL)vvt[VP_GETFRONT])(vp);
    g_ov_back = ((PFN_VP_GETVAL)vvt[VP_GETBACK])(vp);
    ((PFN_SETVAL)vvt[VP_SETBACK])(vp, y + 1000.0f);
    ((PFN_SETVAL)vvt[VP_SETFRONT])(vp, y * 0.25f);
    /* SetPlane is relative to the front plane, so rescale it to keep the same view */
    g_setplane_orig(vp, -hp * y * 0.25f, hp * y * 0.25f, -vpl * y * 0.25f, vpl * y * 0.25f);
    g_internal = 0;
    g_ov_active = 1;
}

static void overview_end(void *vp)
{
    void *cam = *(void **)0x0054AF0C, **vvt;
    float hp, vpl;
    if (!g_ov_active || !cam) return;
    vvt = *(void ***)vp;
    if (g_ov_active == 2) {
        g_internal = 1;
        g_fr_addtransform(cam, 0, g_ov_saved);
        ((PFN_SETVAL)vvt[VP_SETBACK])(vp, g_ov_back);
        g_internal = 0;
        g_ov_active = 0;
        return;
    }
    hp = 0.52f * g_hscale;
    vpl = (*(volatile short *)0x006302BC) ? 0.57f : 0.684f;
    g_internal = 1;
    g_fr_addtransform(cam, 0, g_ov_saved);
    ((PFN_SETVAL)vvt[VP_SETFRONT])(vp, g_ov_front);
    ((PFN_SETVAL)vvt[VP_SETBACK])(vp, g_ov_back);
    g_setplane_orig(vp, -hp, hp, -vpl, vpl);      /* exactly what the race loop sets */
    g_internal = 0;
    g_ov_active = 0;
}

/* replaces the `call 0x402e20` at 0x4016da inside the per-tick track callback */
static void __cdecl track_window_hook(void)
{
    ((void (__cdecl *)(void))0x00402E20)();
    debugmenu_race_tick();
    if (g_tiles_radius == 0) {                   /* original window only: hide any extra tiles */
        if (g_tile_container) tiles_update_visibility();
        return;
    }
    tiles_place_specials();
    if (g_tile_container && (MAP_PTR != g_tiles_map || WORLD_FRAME != g_tiles_world)) tiles_release();
    if (!g_tile_container) tiles_build();
    if (!g_tile_container) return;
    tiles_update_visibility();
    /* one move per tick; goes through the hooked SetPosition so the interpolation's
       floating-origin compensation applies to it like to every other world frame */
    ((PFN_FR_V3)(*(void ***)g_tile_container)[FR_SETPOSITION])(g_tile_container, (int)(INT_PTR)WORLD_FRAME,
        (float)(63 - ORIGIN_X), 0.0f, (float)(-63 - ORIGIN_Z));
}

static void install_tiles(void)
{
    static const BYTE expect[5] = { 0xE8, 0x41, 0x17, 0x00, 0x00 };   /* call 0x402e20 */
    BYTE *site = (BYTE *)0x004016DA;
    char path[MAX_PATH], v[16];
    DWORD old;
    snprintf(path, sizeof path, "%smmcda.ini", g_home);
    GetPrivateProfileStringA("track", "radius", "all", v, sizeof v, path);
    g_tiles_radius = lstrcmpiA(v, "all") == 0 ? -1 : atoi(v);
    if (g_tiles_radius == 0) { log_msg("track: original 8x8 window"); return; }
    if (memcmp(site, expect, 5) != 0) { log_msg("track window site mismatch, skipped"); return; }
    VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old);
    *(INT32 *)(site + 1) = (INT32)((BYTE *)track_window_hook - (site + 5));
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    log_msg("whole-level track hook installed");
    {   /* 0x402dd0 object visibility test -> radius-aware version */
        static const BYTE vis_expect[6] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08 };
        if (memcmp((BYTE *)0x00402DD0, vis_expect, 6) == 0 &&
            patch_jmp((BYTE *)0x00402DD0, vis_expect, 6, object_visible_hook))
            log_msg("object visibility hook installed");
        else log_msg("object visibility site mismatch: %02x %02x %02x %02x",
                     ((BYTE *)0x402DD0)[0], ((BYTE *)0x402DD0)[1], ((BYTE *)0x402DD0)[2], ((BYTE *)0x402DD0)[3]);
    }
}
