/* ==== Controller rumble from game events ==============================================
 * Included by mmcda.c after car.h.
 *
 * Every car sound goes through 0x41c5d0(channel, sample, 0x396, volume, car): it turns
 * the car's distance from the camera into a volume and plays the sample. The samples are
 * handles the game loads into a table at 0x5076f0, one dword per sound, so the sound's
 * slot in that table tells which event happened and the car argument tells to whom.
 * When it is player 1's car (or, for crashes and blasts, a car right next to it) we ask
 * the pad thread for a rumble (pad.h pad_rumble).
 *
 * Slots, from the calls in the decompilation:
 *   0x507788-94  car hits car / wall (4 variants)       0x423120, 0x4241f0
 *   0x507774     car explodes (state 7) / falls apart (0xc)  0x41fff0, 0x420090
 *   0x50772c     hit by a shot                          0x4286b0
 *   0x507728     caught in a blast                      0x4287d0
 *   0x507740     fires a shot                           0x428490
 *   0x507730/34  pickup used (types 3 / 4)              0x4241f0
 *   0x507758     knocked by a roaming creature (state 9) 0x423fd0
 *   0x507738/3c/48/50/54  track features (traps)        0x427480
 *   0x507724/ac  per-car race events                    0x4221a0
 *   (boost, pickup types 1 / 8, is felt from the car's state in rumble_drive)
 *   0x5077a8     state change                           0x420120
 *   0x50775c     a pickup collected (sets +0x428)       0x426bd0
 * Set MMCDA_RUMBLELOG=1 to log every player-car sound slot (for tuning).
 */
typedef unsigned (__cdecl *PFN_CARSND)(int, unsigned, int, unsigned, BYTE *);
static PFN_CARSND g_carsnd_orig;
static int g_rumble_log;

struct rumfx { DWORD slot; float lo, hi; DWORD ms; int reach; const char *name; DWORD flash, flash_ms; };
/* reach: 0 player's car only, 1 also a car touching it (crashes), 2 also nearby (blasts) */
static const struct rumfx k_rumfx[] = {
    { 0x507788, 0.45f, 0.55f, 140, 1, "crash" }, { 0x50778C, 0.45f, 0.55f, 140, 1, "crash" },
    { 0x507790, 0.45f, 0.55f, 140, 1, "crash" }, { 0x507794, 0.45f, 0.55f, 140, 1, "crash" },
    { 0x507774, 1.00f, 1.00f, 700, 2, "explode", 0xFF0000, 0 },   /* light bar: red strobe, below */
    { 0x50772C, 0.85f, 0.75f, 320, 0, "shot hit", 0xFF0000, 450 },
    { 0x507728, 0.90f, 0.70f, 380, 2, "blast", 0xFF2000, 500 },
    { 0x507740, 0.10f, 0.55f,  70, 0, "fire" },
    { 0x507730, 0.20f, 0.50f, 150, 0, "pickup" }, { 0x507734, 0.20f, 0.50f, 150, 0, "pickup" },
    { 0x507758, 0.80f, 0.60f, 400, 1, "knocked", 0xFF0000, 450 },
    { 0x507738, 0.60f, 0.50f, 250, 1, "trap" }, { 0x50773C, 0.60f, 0.50f, 250, 1, "trap" },
    { 0x507748, 0.60f, 0.50f, 250, 1, "trap" }, { 0x507750, 0.60f, 0.50f, 250, 1, "trap" },
    { 0x507754, 0.60f, 0.50f, 250, 1, "trap" },
    { 0x507724, 0.40f, 0.40f, 180, 0, "event" },
    { 0x5077AC, 0.40f, 0.40f, 180, 0, "event" },
    { 0x5077A8, 0.50f, 0.30f, 300, 0, "event" },
    { 0x50775C, 0.00f, 0.35f,  90, 0, "pickup got", 0x00FFFF, 300 },
};

/* the file name of a loaded sample (what 0x4017e0 loaded into slot `idx`): in menus the
   front-end list (0x440848); in races the shared race list (0x440440, 59 entries) and then
   the world's own list, by world type (map header +8) */
static const char *snd_name(unsigned idx)
{
    static const DWORD world_list[6] = { 0x440618, 0x440678, 0x4406D0, 0x440708, 0x440778, 0x4407F0 };
    static const int world_n[6] = { 12, 11, 7, 14, 15, 11 };
    if (!g_race_active) return idx < 26 ? *(const char **)(0x440848 + 8 * idx) : NULL;
    if (idx < 59) return *(const char **)(0x440440 + 8 * idx);
    if (*(BYTE **)0x00465638) {
        int t = *(short *)(*(BYTE **)0x00465638 + 8);
        if (t >= 0 && t < 6 && idx - 59 < (unsigned)world_n[t]) return *(const char **)(world_list[t] + 8 * (idx - 59));
    }
    return NULL;
}

/* the Steam Controller as a speaker (steamhap.h): play the sample the game is playing */
static void speaker_play(unsigned idx, int freq, float gain)
{
    if (g_pad.active < 0 || !strstr(g_pad.names[g_pad.active], "Steam Controller")) return;
    /* the game's loop flag for the sample (0x548e90, set by the loader from its name list) */
    sc_sound(snd_name(idx), freq * 12, gain, idx < 256 && ((volatile int *)0x00548E90)[idx] != 0, (int)idx);
}

static BYTE *rumble_player_car(void)
{
    int i, n = CAR_COUNT;
    for (i = 0; i < n && i < 8; i++) if (car_is_player1(CAR_REC(i))) return CAR_REC(i);
    return NULL;
}

/* returns the channel handle: callers keep it for looping sounds (0x4221a0) */
static unsigned __cdecl carsnd_hook(int ch, unsigned sample, int p3, unsigned vol, BYTE *car)
{
    unsigned r = g_carsnd_orig(ch, sample, p3, vol, car);
    if (g_pad.active >= 0 && g_race_active && car) {
        BYTE *me = rumble_player_car();
        const struct rumfx *fx = NULL;
        DWORD slot = 0;
        unsigned i;
        if (!me) return r;
        for (i = 0; i < sizeof k_rumfx / sizeof k_rumfx[0]; i++)
            if (*(volatile unsigned *)k_rumfx[i].slot == sample) { fx = &k_rumfx[i]; break; }
        if (g_rumble_log && (car == me || fx)) {
            for (slot = 0x5076F0; slot < 0x5077B0; slot += 4) if (*(volatile unsigned *)slot == sample) break;
            log_msg("rumble: sound 0x%06lX (%s) car %d%s", slot, fx ? fx->name : "?",
                    (int)(car - CAR_REC(0)) / 0x460, car == me ? " (you)" : "");
        }
        if (car == me) speaker_play(sample, p3, 1.6f * vol / 127.0f);   /* every sound of your car */
        if (!fx) return r;
        if (car == me && fx->flash && fx->flash_ms) pad_flash(fx->flash, fx->flash_ms);
        if (car == me && fx->flash && !fx->flash_ms) {    /* exploding: state 7 runs 60 ticks (2 s) */
            g_led_strobe = fx->flash; g_led_strobe_end = GetTickCount() + 2000;
        }
        if (g_pad_rumble <= 0) return r;
        if (car == me) pad_rumble(fx->lo, fx->hi, fx->ms);
        else if (fx->reach) {
            float dx = (float)(CAR_I(car, 900) - CAR_I(me, 900)), dz = (float)(CAR_I(car, 0x38C) - CAR_I(me, 0x38C));
            /* crashes: a car touching yours. Blasts: only right beside you (about one
               tile), at most 30% and shorter, so your own explosion stays unmistakable */
            float d = sqrtf(dx * dx + dz * dz), rad = fx->reach == 1 ? 400.0f : 1016.0f, k;
            if (d >= rad) return r;
            if (fx->reach == 1) pad_rumble(fx->lo, fx->hi, fx->ms);
            else {
                k = 0.3f * (1.0f - d / rad);
                pad_rumble(fx->lo * k, fx->hi * k, fx->ms * 2 / 5);
            }
        }
    }
    return r;
}

/* Ground under player 1's car, once per driving tick (car.h drive_hook). 0x41f760 gives
   the surface of the tile spot under the car: 0 off the road (grass, dirt: the game cuts
   speed to 5/6), 2 heavy ground (speed capped at 1/4), 4 slippery (starts a skid), 1
   cobbles (Amazon: at speed the car keeps bouncing a little), 13 the main road, 9 ramps. +0x2c0 is 5000 while the car is on the ground, anything else in the air. */
static void rumble_drive(BYTE *car)
{
    static DWORD next;
    static int air, last_surf = -1;
    int surf, speed, vmax;
    float f, lo = 0, hi = 0;
    DWORD now;
    if (g_rumble_log) {                          /* character -> car model names, once */
        static int done;
        BYTE *t = *(BYTE **)0x00485BD0;
        int c;
        if (!done && t) {
            done = 1;
            for (c = 0; c < 12; c++) {
                const char *a = *(const char **)(t + (c + 0x96) * 12), *b = *(const char **)(t + (c + 0xA2) * 12);
                log_msg("rumble: character %d models '%s' '%s'", c, a ? a : "-", b ? b : "-");
            }
            log_msg("rumble: you are character %d", ((int *)0x00634C04)[(int)(car - CAR_REC(0)) / 0x460]);
        }
    }
    {   /* light bar: your character's car colour, pulsing while a boost runs */
        int ch = ((volatile int *)0x00634C04)[(int)(car - CAR_REC(0)) / 0x460], t = CAR_I(car, 0x428);
        if (ch >= 0 && ch < 12) g_led_base = k_char_led[ch];
        g_led_pulse = (t == 1 || t == 8) && CAR_I(car, 0x430);
    }
    {   /* controller speaker: your car's looping sounds (boost and the like) go on while the
           game still wants them for your car: request bits 1, 2, 4 of 0x630a60 (moved by
           sixcars.h) for the sounds in slots 0x507720, 0x5077ac, 0x507724 (0x4221a0) */
        int me = (int)(car - CAR_REC(0)) / 0x460, req;
        req = (g_rnew[RT_SNDREQ] ? ((volatile int *)g_rnew[RT_SNDREQ]) : (volatile int *)0x00630A60)[me];
        if (req & 1) sc_keep(*(volatile int *)0x00507720);
        if (req & 2) sc_keep(*(volatile int *)0x005077AC);
        if (req & 4) sc_keep(*(volatile int *)0x00507724);
    }
    {   /* new lap: +0x418 counts the laps done (the ranking code caps progress by it) */
        static int last_lap = -1;
        static BYTE *last_car;
        static DWORD second;                     /* a double tick, so it differs from a pickup */
        int lap = CAR_I(car, 0x418);
        if (car != last_car || lap < last_lap) { last_car = car; last_lap = lap; }
        if (lap > last_lap) {
            last_lap = lap;
            pad_flash(0xFFFFFF, 600);
            if (g_pad_rumble > 0) { pad_rumble(0.5f, 0.8f, 90); second = GetTickCount() + 180; }
            if (g_rumble_log) log_msg("rumble: lap %d", lap);
        }
        if (second && (int)(GetTickCount() - second) >= 0) { second = 0; if (g_pad_rumble > 0) pad_rumble(0.5f, 0.8f, 90); }
    }
    if (g_pad_rumble <= 0 || g_pad.active < 0 || !g_race_active) return;
    surf = ((int (__cdecl *)(BYTE *))0x0041F760)(car);
    {   /* worlds by 0x634c44 / 16: 0 West, 1 Arctic, 2 Atlantis, 3 Nightmare, 4 Amazon, 5 City.
           Arctic's ice track and snow are both surface 0 (grass elsewhere): feel it as ice.
           The City road is surface 0 as well: feel it as road (13). */
        int world = *(volatile int *)0x00634C44 / 16;
        if (surf == 0 && world == 1) surf = 4;
        if (surf == 0 && world == 5) surf = 13;
    }
    speed = abs(CAR_I(car, 0x3C0)); vmax = CAR_I(car, 0x3D0);
    f = vmax > 0 ? min(1.0f, (float)speed / vmax) : 0;
    if (g_rumble_log && surf != last_surf) log_msg("rumble: surface %d (world %d, at %d,%d)", surf, *(volatile int *)0x00634C44 / 16, CAR_I(car, 900), CAR_I(car, 0x38C));
    last_surf = surf;
    {   /* DualSense adaptive triggers (pad.h sends them only to a DualSense) */
        int tr[2] = { trig_of(g_bind[ACT_ACCEL]), trig_of(g_bind[ACT_BRAKE]) }, k, t = CAR_I(car, 0x428);
        int boosting = (t == 1 || t == 8) && CAR_I(car, 0x430), airborne = CAR_I(car, 0x2BC) - CAR_I(car, 0x388) > 20;
        for (k = 0; k < 2; k++) {
            if (tr[k] < 0) continue;
            if (airborne) pad_trigger(tr[k], 0x05, 0, 0, 0);                         /* slack in the air */
            else if (boosting) pad_trigger(tr[k], 0x06, 30, 90, 0);                  /* fast buzz */
            else if (surf == 2) pad_trigger(tr[k], 0x01, 0, 170, 0);                 /* mud, sand: heavy */
            else if (surf == 0) pad_trigger(tr[k], 0x01, 0, 80, 0);                  /* grass, dirt */
            else if (surf == 1) pad_trigger(tr[k], 0x06, 20, 60, 0);                 /* cobbles: low buzz */
            else if (surf == 4) pad_trigger(tr[k], 0x05, 0, 0, 0);                   /* ice: no grip */
            else pad_trigger(tr[k], 0x01, 0, 10, 0);                                 /* road: just enough to feel */
        }
    }
    /* airborne while the car's height (+0x388) is above (less than) the ground (+0x2bc), the
       test the drive update itself uses. Cobblestones (surface 1) give short hops; ramps
       launch the car for longer: only a longer flight gets a landing thump, bigger the longer
       it was */
    if (CAR_I(car, 0x388) < CAR_I(car, 0x2BC)) { air++; return; }   /* up is negative (ramps launch at -10 / -20) */
    if (air) {
        if (g_rumble_log) log_msg("rumble: landed after %d ticks (surface %d)", air, surf);
        if (air >= 10) { float k = min(1.0f, air / 40.0f); pad_rumble(0.35f + 0.5f * k, 0.25f + 0.35f * k, 130 + (DWORD)(150 * k)); }
        air = 0;
    }
    now = GetTickCount();
    {   /* boost (pickup type 1 or 8 at +0x428, running while +0x430 is set, +0x42c counts
           down): a surge as it fires, then a steady push on the heavy motor */
        static int boosting;
        static DWORD bnext;
        int t = CAR_I(car, 0x428), on = (t == 1 || t == 8) && CAR_I(car, 0x430);
        if (on && !boosting) { pad_rumble(0.55f, 0.20f, 260); bnext = now + 200; }
        else if (on && (int)(now - bnext) >= 0) { pad_rumble(0.22f, 0.04f, 110); bnext = now + 80; }
        boosting = on;
        if (on) return;
    }
    if ((int)(now - next) < 0 || f < 0.1f) return;
    next = now + 80;                             /* a stream of short overlapping pulses */
    if (surf == 1) {                             /* cobbles: short sharp taps with gaps */
        next = now + 70;
        pad_rumble(0.02f, 0.10f + 0.10f * f, 35);
        return;
    }
    if (surf == 0) {                             /* grass, dirt: faint ticks, heavy and light motor in turn, closer together with speed */
        next = now + 160 - (DWORD)(110 * f);       /* 160 ms slow, 50 ms at top speed */
        static int side;                         /* alternate heavy / light: a left-right patter */
        side ^= 1;
        if (side) pad_rumble(0.025f + 0.015f * f, 0, 25);
        else pad_rumble(0, 0.02f + 0.015f * f, 25);
        return;
    }
    if (surf == 2) { lo = 0.13f + 0.09f * f; hi = 0.03f; }                 /* mud, sand */
    else if (surf == 4) { lo = 0; hi = 0.012f + 0.016f * f; }                   /* ice: a faint hiss */
    else return;
    pad_rumble(lo, hi, 110);
}

/* 0x4130d0(channel, sample, freq, volume) plays a sample. In the main menu the gunman's
   "pop pop" is icecrak2 (0x5076f0) at a low pitch, played twice per loop by the menu
   animation 0x410cb0 (frames 0x19 and 0x27): flash white. */
typedef unsigned (__cdecl *PFN_PLAY)(int, unsigned, int, int);
static PFN_PLAY g_play_orig;
static unsigned __cdecl play_hook(int ch, unsigned sample, int freq, int vol)
{
    if (!g_race_active) speaker_play(sample, freq, 1.4f);                 /* menu sounds */
    else {                                                                /* race-wide ones */
        const char *nm = snd_name(sample);
        if (nm && (!lstrcmpiA(nm, "grnstart") || !lstrcmpiA(nm, "redstart") || !lstrcmpiA(nm, "applause")))
            speaker_play(sample, freq, 1.4f);
    }
    if (g_rumble_log && !g_race_active)
        log_msg("rumble: menu sound %u freq %d page %d", sample, freq, *(volatile int *)0x005072F4);
    if (!g_race_active && g_pad.active >= 0) {
        if (sample == *(volatile unsigned *)0x005076F0) pad_flash(0xFFFFFF, 160);
        /* menu haptics from the front-end sounds (slots 0x5076c0 + 4n), as logged in the
           menus: bridge3 moves in a menu, boost01 changes a setting (left / right), hit14
           confirms or leaves, hit15 opens a page. (The character carousel makes no sound:
           pad.h ticks it from the highlighted character.) */
        else if (g_pad_rumble > 0 && g_pad_menuhap) {
            if (sample == *(volatile unsigned *)0x005076CC || sample == *(volatile unsigned *)0x005076C8)
                pad_rumble(0.12f, 0.30f, 70);
            else if (sample == *(volatile unsigned *)0x005076E4) pad_rumble(0.35f, 0.35f, 70);
            else if (sample == *(volatile unsigned *)0x005076E8) pad_rumble(0.25f, 0.25f, 60);
        }
    }
    return g_play_orig(ch, sample, freq, vol);
}

/* 0x413110: the same as 0x4130d0 but the sample loops. The menu carousels (0x411b30) play
   gears01 (0x5076dc) through it on every turn, at pitch (player * 1000 + 2500) one way and
   ((player + 3) * 1000) the other. gears01 is two parts: a rising wind-up at 190-560 ms and a
   long grind at 790-1640 ms as recorded (22050 Hz); it plays at freq * 12 Hz. */
static PFN_PLAY g_play2_orig;
static unsigned __cdecl play2_hook(int ch, unsigned sample, int freq, int vol)
{
    if (!g_race_active && g_pad.active >= 0 && g_pad_rumble > 0 && g_pad_menuhap && freq > 0 &&
        sample == *(volatile unsigned *)0x005076DC) {
        float k = 22050.0f / (freq * 12.0f);
        pad_rumble_at((DWORD)(190 * k), 0.0f, 0.13f, (DWORD)(370 * k));    /* wind-up: a light-motor buzz, */
        pad_rumble_at2((DWORD)(790 * k), 0.0f, 0.11f, (DWORD)(850 * k));   /* grind: a bit over the ice hiss */
    }
    if (!g_race_active) speaker_play(sample, freq, 1.0f);
    return g_play2_orig(ch, sample, freq, vol);
}

static void install_rumble(void)
{
    /* push ebp; mov ebp,esp; mov edx,[0x630b00] */
    static const BYTE snd_expect[9] = { 0x55, 0x8B, 0xEC, 0x8B, 0x15, 0x00, 0x0B, 0x63, 0x00 };
    g_rumble_log = GetEnvironmentVariableA("MMCDA_RUMBLELOG", NULL, 0) != 0;
    if (memcmp((BYTE *)0x0041C5D0, snd_expect, 9) == 0 &&
        (g_carsnd_orig = (PFN_CARSND)make_trampoline(snd_expect, 9, (BYTE *)0x0041C5D9)) &&
        patch_jmp((BYTE *)0x0041C5D0, snd_expect, 9, carsnd_hook))
        log_msg("rumble: car event hook installed (strength %d%%)", g_pad_rumble);
    {
        static const BYTE pl_expect[6] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x0C };
        if (memcmp((BYTE *)0x004130D0, pl_expect, 6) == 0 &&
            (g_play_orig = (PFN_PLAY)make_trampoline(pl_expect, 6, (BYTE *)0x004130D6)) &&
            patch_jmp((BYTE *)0x004130D0, pl_expect, 6, play_hook))
            log_msg("rumble: menu gunshot light hook installed");
        if (memcmp((BYTE *)0x00413110, pl_expect, 6) == 0 &&
            (g_play2_orig = (PFN_PLAY)make_trampoline(pl_expect, 6, (BYTE *)0x00413116)) &&
            patch_jmp((BYTE *)0x00413110, pl_expect, 6, play2_hook))
            log_msg("rumble: menu carousel hook installed");
    }
}
