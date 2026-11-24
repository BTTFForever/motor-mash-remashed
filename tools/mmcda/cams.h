/* ==== Extra race cameras (controller Select / F7) ========================================
 * Included by mmcda.c after car.h. Applied per rendered frame from overview_begin
 * (tiles.h): the game's camera frame is posed for the render and restored afterwards,
 * like the F8 overview, so the game logic never sees it.
 *
 *   0 Original  the game's camera; right stick up/down = camera height (PageUp/PageDown)
 *   1 Chase     behind the car, loosely following; right stick looks left/right/behind
 *   2 Bumper    on the front of the car; right stick looks left/right/behind
 *   3 Top-down  straight down over the car; right stick up/down zooms
 *   4 TV        fixed trackside cameras that track and zoom; cut to the next one ahead
 *
 * Positions are kept in absolute game units (scene + floating origin). The car pose is
 * the player's body frame (0x488568[i]) as interpolated for this frame. Which model axis
 * points forward (+X or -X) is learned from the car's motion.
 */
#define VIEW_COUNT 5
static const char *const k_view_names[VIEW_COUNT] = { "Original", "Chase", "Bumper", "Top-down", "TV" };
static volatile int g_view;
static float g_cv_fsign = 1.0f;                 /* model x axis * this = forward */
static struct {
    int init;
    double last_ms;
    float px, py, pz;                           /* smoothed camera position (absolute) */
    float yaw;                                  /* smoothed follow yaw (chase) */
    float look;                                 /* smoothed look-around offset */
    float lastcx, lastcz; int havelast;         /* car position, for velocity */
    float vx, vz;                               /* smoothed car velocity (units/s) */
    float tvx, tvy, tvz; int tv_ok, tv_side;    /* TV camera spot */
    float topdown_h;
} g_cv = { 0 };

static float ang_wrap(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }
static float smooth_k(float dt, float tau) { return tau <= 0 ? 1.0f : 1.0f - (float)exp(-dt / tau); }

/* right-stick look direction: up = ahead (0), right = +90, down = behind; 0 when centred */
static float stick_look(void)
{
    float sx = g_pad.active >= 0 ? g_pad.axis[2] : 0, sy = g_pad.active >= 0 ? g_pad.axis[3] : 0;
    if (sx * sx + sy * sy < 0.35f * 0.35f) return 0;
    return (float)atan2(sx, -sy);
}

/* the player car: absolute position and forward yaw (atan2(x, z) of forward) */
static int player_pose(float *x, float *y, float *z, float *yaw)
{
    int i, n = CAR_COUNT;
    for (i = 0; i < n && i < 8; i++) {
        BYTE *car = CAR_REC(i);
        void *body = g_t_bodyframe[i];
        MAT4 m;
        if (!car_is_player1(car) || !body || g_fr_gettransform(body, m) != 0) continue;
        *x = m[3][0] + ORIGIN_X; *y = m[3][1]; *z = m[3][2] + ORIGIN_Z;
        if (g_render_vulkan && GetEnvironmentVariableA("MMCDA_VKDEBUG", NULL, 0)) {
            static int n;
            if (n++ % 200 == 0) {
                vframe *f = (vframe *)body;
                int d;
                for (d = 0; f && d < 5; f = f->parent, d++)
                    log_msg("cams: body chain %d: %p '%s' local t(%.1f %.1f %.1f) kids %d vis %d", d, f, f->o.name,
                            f->m[3][0], f->m[3][1], f->m[3][2], f->kids.n, f->visuals.n);
            }
        }
        if (g_cc[i].ok && g_cc[i].frame == body) {
            /* the model was shifted so its middle sits on the pivot: the frame origin moved,
               so aim at where the mesh centre (model cx, 0, cz) ends up */
            *x += g_cc[i].cx * m[0][0] + g_cc[i].cz * m[2][0];
            *y += g_cc[i].cx * m[0][1] + g_cc[i].cz * m[2][1];
            *z += g_cc[i].cx * m[0][2] + g_cc[i].cz * m[2][2];
        }
        *yaw = (float)atan2(m[0][0] * g_cv_fsign, m[0][2] * g_cv_fsign);
        if (m[0][0] * m[0][0] + m[0][2] * m[0][2] < 1e-8f) return 0;
        /* learn which way the model faces from the motion (fast and straight enough) */
        {
            float sp = (float)sqrt(g_cv.vx * g_cv.vx + g_cv.vz * g_cv.vz);
            float ax = m[0][0], az = m[0][2], al = (float)sqrt(ax * ax + az * az);
            if (sp > 150.0f) {
                float d = (g_cv.vx * ax + g_cv.vz * az) / (sp * al);
                if (d > 0.8f && g_cv_fsign < 0) { g_cv_fsign = 1; log_msg("cams: car models face +x"); }
                if (d < -0.8f && g_cv_fsign > 0) { g_cv_fsign = -1; log_msg("cams: car models face -x"); }
            }
        }
        return 1;
    }
    return 0;
}

/* ground level of the map cell under an absolute position (tile height steps, tiles.h):
   cell (row R, col C) spans x 127C..127C+127, z -127R-127..-127R */
static float cell_floor(float x, float z)
{
    unsigned short *map = MAP_PTR;
    int c, r;
    if (!map) return -1e9f;
    c = (int)floor(x / 127.0f); r = (int)floor(-z / 127.0f);
    if (c < 0 || r < 0 || c >= map[0] || r >= map[1]) return -1e9f;
    return (float)(((map[8 + r * map[0] + c] >> 12) & 3) * HEIGHT_STEP);
}

/* keep a camera above the ground it is over and the ground between it and the car */
static float clear_ground(float x, float y, float z, float tx, float tz)
{
    int k;
    for (k = 0; k <= 4; k++) {
        float f = k / 4.0f, g = cell_floor(x + (tx - x) * f * 0.75f, z + (tz - z) * f * 0.75f) + 30.0f;
        if (y < g) y = g;
    }
    return y;
}

static void cam_look(void *cam, void *scene, float x, float y, float z, float tx, float ty, float tz)
{
    float dx = tx - x, dy = ty - y, dz = tz - z;
    g_fr_setposition(cam, (int)(INT_PTR)scene, x - ORIGIN_X, y, z - ORIGIN_Z);
    g_fr_setorientation(cam, scene, dx, dy, dz, 0, 1, 0);
}

/* returns 1 if it posed the camera (projection args filled: front, back, zoom) */
static int views_pose(void *cam, void *scene, float *front, float *back, float *zoom)
{
    double now = now_ms();
    float dt = g_cv.last_ms ? (float)((now - g_cv.last_ms) / 1000.0) : 0.016f;
    float cx, cy, cz, yaw, la;
    if (dt > 0.1f) dt = 0.1f;
    if (dt < 0.0005f) dt = 0.0005f;
    g_cv.last_ms = now;
    if (!player_pose(&cx, &cy, &cz, &yaw)) return 0;
    /* car velocity (smoothed) */
    if (g_cv.havelast) {
        float vx = (cx - g_cv.lastcx) / dt, vz = (cz - g_cv.lastcz) / dt, k = smooth_k(dt, 0.3f);
        if (vx * vx + vz * vz > 4000.0f * 4000.0f) { vx = vz = 0; g_cv.init = 0; }   /* respawn / new race */
        g_cv.vx += (vx - g_cv.vx) * k; g_cv.vz += (vz - g_cv.vz) * k;
    }
    g_cv.lastcx = cx; g_cv.lastcz = cz; g_cv.havelast = 1;
    la = stick_look();
    g_cv.look += ang_wrap(la - g_cv.look) * smooth_k(dt, 0.12f);
    *front = 1.0f; *back = 4096.0f; *zoom = 1.0f;
    switch (g_view) {
    case 1: {   /* chase: loose yaw follow, orbit by the right stick */
        float y2, tx, tz, px, pz, py, k;
        if (!g_cv.init) { g_cv.yaw = yaw; }
        g_cv.yaw += ang_wrap(yaw - g_cv.yaw) * smooth_k(dt, 0.35f);
        y2 = g_cv.yaw + g_cv.look;
        px = cx - (float)sin(y2) * 95.0f; pz = cz - (float)cos(y2) * 95.0f; py = cy + 42.0f;
        py = clear_ground(px, py, pz, cx, cz);
        k = smooth_k(dt, 0.08f);
        if (!g_cv.init) { g_cv.px = px; g_cv.py = py; g_cv.pz = pz; g_cv.init = 1; }
        g_cv.px += (px - g_cv.px) * k; g_cv.py += (py - g_cv.py) * k; g_cv.pz += (pz - g_cv.pz) * k;
        tx = cx + (float)sin(y2) * 25.0f; tz = cz + (float)cos(y2) * 25.0f;
        cam_look(cam, scene, g_cv.px, g_cv.py, g_cv.pz, tx, cy + 10.0f, tz);
        *front = 2.0f;
        return 1;
    }
    case 2: {   /* bumper: rigid on the car front, look around with the stick */
        float px = cx + (float)sin(yaw) * 24.0f, pz = cz + (float)cos(yaw) * 24.0f, py = cy + 16.0f;
        float y2 = yaw + g_cv.look;
        g_cv.init = 1;
        cam_look(cam, scene, px, py, pz, px + (float)sin(y2) * 100.0f, py - 2.0f, pz + (float)cos(y2) * 100.0f);
        *front = 1.0f; *zoom = 1.25f;
        return 1;
    }
    case 3: {   /* top-down: north up, zoom with the stick */
        float sy = g_pad.active >= 0 ? g_pad.axis[3] : 0, k;
        if (g_cv.topdown_h < 150.0f) g_cv.topdown_h = 700.0f;
        if (fabsf(sy) > 0.2f) g_cv.topdown_h *= (float)exp(sy * dt * 1.5f);
        g_cv.topdown_h = max(150.0f, min(6000.0f, g_cv.topdown_h));
        k = smooth_k(dt, 0.1f);
        if (!g_cv.init) { g_cv.px = cx; g_cv.pz = cz; g_cv.init = 1; }
        g_cv.px += (cx - g_cv.px) * k; g_cv.pz += (cz - g_cv.pz) * k;
        g_fr_setposition(cam, (int)(INT_PTR)scene, g_cv.px - ORIGIN_X, cy + g_cv.topdown_h, g_cv.pz - ORIGIN_Z);
        g_fr_setorientation(cam, scene, 0, -1, 0, 0, 0, 1);
        *front = 4.0f; *back = g_cv.topdown_h + 1500.0f;
        return 1;
    }
    case 4: {   /* TV: fixed spot ahead and to the side, track and zoom, cut when passed */
        float sp = (float)sqrt(g_cv.vx * g_cv.vx + g_cv.vz * g_cv.vz), dx, dz, dist, fx, fz;
        fx = sp > 30.0f ? g_cv.vx / sp : (float)sin(yaw);
        fz = sp > 30.0f ? g_cv.vz / sp : (float)cos(yaw);
        dx = cx - g_cv.tvx; dz = cz - g_cv.tvz; dist = (float)sqrt(dx * dx + dz * dz);
        if (!g_cv.tv_ok || !g_cv.init || dist > 700.0f || (dx * fx + dz * fz > 160.0f && dist > 260.0f)) {
            float side = (g_cv.tv_side ^= 1) ? 1.0f : -1.0f, ahead = 380.0f + sp * 0.4f;
            if (ahead > 650.0f) ahead = 650.0f;
            g_cv.tvx = cx + fx * ahead + fz * side * 140.0f;
            g_cv.tvz = cz + fz * ahead - fx * side * 140.0f;
            g_cv.tvy = clear_ground(g_cv.tvx, cy + 70.0f, g_cv.tvz, cx, cz);
            g_cv.tv_ok = 1; g_cv.init = 1;
            dx = cx - g_cv.tvx; dz = cz - g_cv.tvz; dist = (float)sqrt(dx * dx + dz * dz);
        }
        cam_look(cam, scene, g_cv.tvx, g_cv.tvy, g_cv.tvz, cx, cy + 8.0f, cz);
        *front = 2.0f;
        *zoom = max(0.28f, min(1.0f, 220.0f / (dist + 1.0f)));       /* zoom in as it moves away */
        return 1;
    }
    }
    return 0;
}

/* Sky: the original camera never shows the horizon, so the scene background is black.
   While an extra view is on, use a colour per world (scene SetSceneBackgroundRGB, frame
   slot 49) and give the hard clear the same colour; restore on leaving (slot 48). */
static const float k_sky[6][3] = {
    { 0.55f, 0.74f, 0.95f },   /* West */
    { 0.78f, 0.86f, 0.96f },   /* Arctic */
    { 0.04f, 0.18f, 0.40f },   /* Atlantis (underwater) */
    { 0.06f, 0.03f, 0.14f },   /* Nightmare */
    { 0.50f, 0.72f, 0.86f },   /* Amazon */
    { 0.18f, 0.22f, 0.42f } }; /* City */
static void views_sky(void)
{
    static void *sky_scene;
    static DWORD saved;
    void *scene = *(void **)0x0054AF08;
    int want = g_view != 0 && g_race_active && scene;
    if (sky_scene && (sky_scene != scene || !g_race_active)) { sky_scene = NULL; g_clear_rgb = 0; }   /* race over / new race */
    if (want && !sky_scene) {
        int w = (*(volatile int *)0x00634C44 / 16) % 6;
        const float *c;
        if (w < 0) w = 0;
        c = k_sky[w];
        saved = ((DWORD (WINAPI *)(void *))(*(void ***)scene)[42])(scene);
        ((HRESULT (WINAPI *)(void *, float, float, float))(*(void ***)scene)[49])(scene, c[0], c[1], c[2]);
        g_clear_rgb = ((DWORD)(c[0] * 255) << 16) | ((DWORD)(c[1] * 255) << 8) | (DWORD)(c[2] * 255);
        sky_scene = scene;
    } else if (!want && sky_scene) {
        /* only touch the scene while it is still the game's current one: when a race ends
           it is released during the teardown, and calling into it then crashed the game */
        if (sky_scene == scene && g_race_active)
            ((HRESULT (WINAPI *)(void *, DWORD))(*(void ***)sky_scene)[48])(sky_scene, saved);
        g_clear_rgb = 0;
        sky_scene = NULL;
    }
}

/* per rendered frame, before overview handling: inputs + original-camera height */
static void views_input(void)
{
    views_sky();
    static int sel_was, f7_was, env_done;
    if (!env_done) {                            /* test switch: start in a given view */
        char v[8];
        env_done = 1;
        if (GetEnvironmentVariableA("MMCDA_VIEW", v, sizeof v)) g_view = atoi(v) % VIEW_COUNT;
    }
    int sel = g_pad.active >= 0 && g_pad.button[4];
    int f7 = GetForegroundWindow() == *(HWND *)0x006172A8 && (GetAsyncKeyState(VK_F7) & 0x8000);
    if ((sel && !sel_was) || (f7 && !f7_was)) {
        g_view = (g_view + 1) % VIEW_COUNT;
        g_cv.init = 0; g_cv.tv_ok = 0;
        log_msg("camera view: %s", k_view_names[g_view]);
    }
    sel_was = sel; f7_was = f7;
    if (g_view == 0 && g_pad.active >= 0 && !g_pad_setup_open) {     /* original: stick = height */
        static double last;
        double now = now_ms();
        float dt = last ? (float)((now - last) / 1000.0) : 0, sy = g_pad.axis[3];
        last = now;
        if (dt > 0.1f) dt = 0.1f;
        if (fabsf(sy) > 0.2f) {
            g_zoom *= (float)exp(sy * dt * 1.2f);
            if (g_zoom < 0.6f) g_zoom = 0.6f;
            if (g_zoom > 3.0f) g_zoom = 3.0f;
            CAM_HEIGHT = 56.0f * g_zoom;
            apply_far_clip();
        }
    }
}
