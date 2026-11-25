/* ==== Car sounds that share one sound buffer =========================================
 * Included by mmcda.c after sixcars.h.
 *
 * The game has one DirectSound buffer per sample: playing a sample (0x4130d0) rewinds and
 * plays that buffer and returns its handle (sample + 1). Looping car sounds are started
 * once and then updated every tick by 0x4221a0 through 0x41c660(handle, freq, volume,
 * car), which sets the buffer's pitch and a volume from that car's distance to the camera,
 * and are stopped by 0x413190(handle) when the car no longer wants them.
 *
 * 1. Engines: 0x4221a0 plays sample table entry [character] (0x5076c0, via the identity
 *    map 0x45a3d0). The race sound loader 0x4017e0 first fills the table with the
 *    identity, then overwrites entries 0..cars-1 with the characters of cars 0..cars-1,
 *    so two cars often point at the same engine sample and fight over its buffer: the
 *    engine stutters. Six cars made it far more likely. Keep the identity.
 * 2. Boost and the other per-car loops use one sample for every car. Two cars at once
 *    fought over its volume, and when one car's loop ended it stopped the buffer under
 *    the other. Now the car nearest the camera sets a shared buffer each tick, and a
 *    stop from the race tick is skipped while another car still plays that loop.
 */
static BYTE *snd_tab(int rt, DWORD orig) { return g_rnew[rt] ? g_rnew[rt] : (BYTE *)orig; }

static void snd_fix_engines(void)
{
    int i;
    for (i = 0; i < 8; i++) ((volatile int *)0x005076C0)[i] = i;
}

/* the race tick's own calls, so menus and other code keep the original behaviour */
static int snd_from_tick(void *ret) { return (DWORD)ret >= 0x004221A0 && (DWORD)ret < 0x00423000; }

typedef void (__cdecl *PFN_SNDSTOP)(int);
static PFN_SNDSTOP g_sndstop_orig;
static void __cdecl sndstop_hook(int handle)
{
    if (snd_from_tick(_ReturnAddress())) {
        static const int bits[4] = { 1, 2, 4, 8 };
        static const int rts[4] = { RT_SND_1A40, RT_SND_25D0, RT_SND_0E00, RT_SND_19E0 };
        static const DWORD origs[4] = { 0x631A40, 0x6325D0, 0x630E00, 0x6319E0 };
        int *req = (int *)snd_tab(RT_SNDREQ, 0x630A60), *play = (int *)snd_tab(RT_SNDPLAY, 0x637E40);
        int j, b, n = CAR_COUNT;
        for (j = 0; j < n && j < 8; j++)
            for (b = 0; b < 4; b++)
                if ((req[j] & bits[b]) && (play[j] & bits[b]) && ((int *)snd_tab(rts[b], origs[b]))[j] == handle)
                    return;                     /* another car still plays this loop */
    }
    g_sndstop_orig(handle);
}

typedef void (__cdecl *PFN_SNDUPD)(int, int, unsigned, BYTE *);
static PFN_SNDUPD g_sndupd_orig;
static void __cdecl sndupd_hook(int handle, int freq, unsigned vol, BYTE *car)
{
    if (handle > 0 && handle < 256 && car && snd_from_tick(_ReturnAddress())) {
        static struct { DWORD t; float d; BYTE *car; } last[256];
        float dx = (float)(CAR_I(car, 900) - *(volatile int *)0x00630B00);
        float dz = (float)(CAR_I(car, 0x38C) - *(volatile int *)0x00630B10);
        float d = dx * dx + dz * dz;
        DWORD now = GetTickCount();
        /* a nearer car set this buffer within the last few ticks: leave it */
        if (last[handle].car != car && now - last[handle].t < 100 && d > last[handle].d) return;
        last[handle].t = now; last[handle].d = d; last[handle].car = car;
    }
    g_sndupd_orig(handle, freq, vol, car);
}

typedef int (__cdecl *PFN_SNDLOAD)(int);
static PFN_SNDLOAD g_sndload4_orig;
static int __cdecl sndload4_hook(int keep)
{
    int r = g_sndload4_orig(keep);
    if (keep == 0) snd_fix_engines();
    return r;
}

static void install_sndfix(void)
{
    static const BYTE upd_expect[9] = { 0x55, 0x8B, 0xEC, 0x8B, 0x15, 0x00, 0x0B, 0x63, 0x00 };
    static const BYTE stop_expect[6] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08 };
    int ok = 0;
    if (memcmp((BYTE *)0x0041C660, upd_expect, 9) == 0 &&
        (g_sndupd_orig = (PFN_SNDUPD)make_trampoline(upd_expect, 9, (BYTE *)0x0041C669)) &&
        patch_jmp((BYTE *)0x0041C660, upd_expect, 9, sndupd_hook)) ok++;
    if (memcmp((BYTE *)0x00413190, stop_expect, 6) == 0 &&
        (g_sndstop_orig = (PFN_SNDSTOP)make_trampoline(stop_expect, 6, (BYTE *)0x00413196)) &&
        patch_jmp((BYTE *)0x00413190, stop_expect, 6, sndstop_hook)) ok++;
    if (!g_sndload_orig) {          /* six cars off: sixcars.h has not hooked the loader */
        static const BYTE sl[8] = { 0x55, 0x8B, 0xEC, 0xA1, 0xC0, 0x89, 0x48, 0x00 };
        if (memcmp((BYTE *)0x004017E0, sl, 8) == 0 &&
            (g_sndload4_orig = (PFN_SNDLOAD)make_trampoline(sl, 8, (BYTE *)0x004017E8)) &&
            patch_jmp((BYTE *)0x004017E0, sl, 8, sndload4_hook)) ok++;
    } else ok++;
    log_msg("sound: shared-buffer fixes installed (%d/3)", ok);
}
