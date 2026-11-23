/* ==== VR export: share the live D3DRM scene with the tabletop VR viewer ===============
 * Included by mmcda.c after tiles.h. Off unless mmcda.ini [vr] export=1 or MMCDA_VR=1.
 *
 * Every logic tick (top of the viewport Render hook, before interpolation blends
 * anything) the scene frame tree is walked; each frame with a MeshBuilder visual becomes
 * one instance {mesh id, 3x4 world matrix}. Positions are made absolute by adding the
 * floating origin (0x44d3b8/0x44d3c0) back, so they stay continuous across origin steps.
 *
 * Shared memory "Local\MotorMashVR" (layout in vrshared.h, shared with the viewer):
 * a header plus 3 rotating instance slots; `latest` names the newest complete slot.
 * Mesh and texture data are written once per race ("generation") as files under
 * <home>vrcache\<generation>\ m<id>.bin / t<id>.bin, then `meshes_written` is bumped.
 *
 * D3DRM v1 slot numbers (verified on the disc's DX5 d3drm.dll with a run-time probe):
 *   Object GetClassName 10; Frame GetChildren 19, GetTransform 29, GetVisuals 32;
 *   Array GetSize 3, GetElement 4; MeshBuilder CreateMesh 48;
 *   Mesh GetGroupCount 22, GetGroup 23, GetVertices 24, GetGroupColor 25,
 *   GetGroupTexture 29. Texture GetImage is probed (24, then 21) and validated.
 */
#include "vrshared.h"

#define OBJ_GETCLASSNAME   10
#define FR_GETCHILDREN     19
#define FR_GETVISUALS      32
#define ARR_GETSIZE         3
#define ARR_GETELEMENT      4
#define MB_CREATEMESH      48
#define MESH_GETGROUPCOUNT 22
#define MESH_GETGROUP      23
#define MESH_GETVERTICES   24
#define MESH_GETGROUPCOLOR 25
#define MESH_GETGROUPTEX   29

typedef HRESULT (WINAPI *PFN_GETCLASS)(void *o, DWORD *size, char *name);
typedef HRESULT (WINAPI *PFN_GETARRAY)(void *o, void **arr);
typedef DWORD   (WINAPI *PFN_ARR_SIZE)(void *a);
typedef HRESULT (WINAPI *PFN_ARR_ELEM)(void *a, DWORD i, void **out);
typedef HRESULT (WINAPI *PFN_MB_CREATEMESH)(void *mb, void **mesh);
typedef unsigned (WINAPI *PFN_MESH_GCOUNT)(void *m);
typedef HRESULT (WINAPI *PFN_MESH_GETGROUP)(void *m, DWORD id, unsigned *vc, unsigned *fc,
                                            unsigned *vpf, DWORD *dsize, unsigned *data);
typedef HRESULT (WINAPI *PFN_MESH_GETVERTS)(void *m, DWORD id, DWORD index, DWORD count, void *v);
typedef DWORD   (WINAPI *PFN_MESH_GCOLOR)(void *m, DWORD id);
typedef HRESULT (WINAPI *PFN_MESH_GTEX)(void *m, DWORD id, void **tex);
typedef void *  (WINAPI *PFN_TEX_GETIMAGE)(void *t);
#define VT(o) (*(void ***)(o))

typedef struct { float px, py, pz, nx, ny, nz, tu, tv; DWORD color; } RMVERTEX;   /* D3DRMVERTEX */
typedef struct { BYTE r, g, b, flags; } RMPALENTRY;
typedef struct {
    int width, height, aspectx, aspecty, depth, rgb, bytes_per_line;
    void *buffer1, *buffer2;
    ULONG red_mask, green_mask, blue_mask, alpha_mask;
    int palette_size;
    RMPALENTRY *palette;
} RMIMAGE;                                                                          /* D3DRMIMAGE */

static int g_vr_on;
static int g_vr_hud_pending;          /* set per captured tick: copy the HUD once */
static HANDLE g_vr_map;
static MMVR_SHARED *g_vr;
static char g_vr_dir[MAX_PATH];

/* pointer -> id maps (open addressing); id < 0 = not exportable */
#define VR_HASH 8192
struct vrmap { void *key; int id; };
static struct vrmap g_vr_mesh[VR_HASH], g_vr_tex[VR_HASH];
static int g_vr_nmesh, g_vr_ntex;

static struct vrmap *vr_slot(struct vrmap *m, void *key)
{
    unsigned h = ((unsigned)(UINT_PTR)key >> 4) * 2654435761u, i;
    for (i = 0; i < VR_HASH; i++) {
        struct vrmap *e = &m[(h + i) & (VR_HASH - 1)];
        if (e->key == key || !e->key) return e;
    }
    return NULL;
}

static void vr_release(void *o) { ((ULONG (WINAPI *)(void *))VT(o)[2])(o); }

static void obj_classname(void *o, char *buf, DWORD n)
{
    DWORD sz = n;
    buf[0] = 0;
    if (((PFN_GETCLASS)VT(o)[OBJ_GETCLASSNAME])(o, &sz, buf) != 0) lstrcpynA(buf, "?", n);
}

static int vr_write_file(const char *name, const void *a, DWORD na, const void *b, DWORD nb)
{
    char tmp[MAX_PATH], dst[MAX_PATH];
    HANDLE h;
    DWORD w;
    snprintf(dst, sizeof dst, "%s\\%s", g_vr_dir, name);
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    WriteFile(h, a, na, &w, NULL);
    if (b && nb) WriteFile(h, b, nb, &w, NULL);
    CloseHandle(h);
    return MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING);
}

/* ---- textures -> RGBA8 ------------------------------------------------------------ */
static BYTE mask_chan(DWORD v, ULONG mask)
{
    int shift = 0, bits = 0;
    if (!mask) return 0;
    while (!(mask & 1)) { mask >>= 1; shift++; }
    while (mask & 1) { mask >>= 1; bits++; }
    v = (v >> shift) & ((1u << bits) - 1);
    return (BYTE)(bits >= 8 ? v >> (bits - 8) : (v * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1));
}

/* The game's texture callback 0x42b540 keeps a table at 0x54def8 (0x454 bytes per entry,
   count at 0x54a0b4): +0x44c the DirectDraw surface holding the pixels (often 8-bit
   palettised, with a DDCKEY_SRCBLT colour key for transparency), +0x450 the D3DRM
   texture built from it with InitFromSurface. D3DRM's GetImage is NULL for those. */
#define TEXREC_BASE  0x0054DEF8
#define TEXREC_SIZE  0x454
#define TEXREC_COUNT (*(volatile int *)0x0054A0B4)

static struct vrmap g_vr_dtex[VR_HASH];          /* decal variants (RGB transparency key) */
static short g_vr_texdim[4096][2];
static int vr_texture_id_ex(void *tex, long rgbkey);
static int vr_texture_id(void *tex) { return vr_texture_id_ex(tex, -1); }

/* Read a texture surface as RGBA8. rgbkey >= 0: pixels of that RGB colour become
   transparent (decals); the surface's DDCKEY_SRCBLT colour key always does. */
static BYTE *vr_read_surface(void *surf, long rgbkey, int *pw, int *ph, int *pkey)
{
    void **svt = VT(surf), *pal = NULL;
    BYTE desc[0x6C], pe[256][4], *bits, *rgba;
    DWORD key = 0, ck[2], pf_flags, bpp, rm, gm, bm, am;
    int has_key = 0, w, h, pitch, x, y;
    if (((HRESULT (WINAPI *)(void *, DWORD, DWORD *))svt[16])(surf, 8 /* DDCKEY_SRCBLT */, ck) == 0) { key = ck[0]; has_key = 1; }
    memset(desc, 0, sizeof desc); *(DWORD *)desc = sizeof desc;
    if (((HRESULT (WINAPI *)(void *, RECT *, void *, DWORD, HANDLE))svt[25])(surf, NULL, desc, 0x11 /* WAIT|READONLY */, NULL) != 0)
        return NULL;
    h = *(int *)(desc + 0x08); w = *(int *)(desc + 0x0C); pitch = *(int *)(desc + 0x10);
    bits = *(BYTE **)(desc + 0x24);
    pf_flags = *(DWORD *)(desc + 0x4C); bpp = *(DWORD *)(desc + 0x54);
    rm = *(DWORD *)(desc + 0x58); gm = *(DWORD *)(desc + 0x5C); bm = *(DWORD *)(desc + 0x60); am = *(DWORD *)(desc + 0x64);
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048 || !bits || (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)) {
        ((HRESULT (WINAPI *)(void *, void *))svt[32])(surf, NULL);
        return NULL;
    }
    memset(pe, 0, sizeof pe);
    if (pf_flags & 0x20 /* DDPF_PALETTEINDEXED8 */) {
        if (((HRESULT (WINAPI *)(void *, void **))svt[20])(surf, &pal) == 0 && pal) {
            ((HRESULT (WINAPI *)(void *, DWORD, DWORD, DWORD, void *))VT(pal)[4])(pal, 0, 0, 256, pe);
            vr_release(pal);
        }
    }
    rgba = (BYTE *)malloc((size_t)w * h * 4);
    if (rgba) {
        int bytes = bpp / 8;
        for (y = 0; y < h; y++) {
            const BYTE *row = bits + (size_t)y * pitch;
            BYTE *o = rgba + (size_t)y * w * 4;
            for (x = 0; x < w; x++, o += 4) {
                DWORD v = 0;
                int k;
                for (k = 0; k < bytes; k++) v |= (DWORD)row[x * bytes + k] << (8 * k);
                if (pf_flags & 0x20) { o[0] = pe[v & 255][0]; o[1] = pe[v & 255][1]; o[2] = pe[v & 255][2]; o[3] = 255; }
                else {
                    o[0] = mask_chan(v, rm); o[1] = mask_chan(v, gm); o[2] = mask_chan(v, bm);
                    o[3] = (am && (pf_flags & 1 /* DDPF_ALPHAPIXELS */)) ? mask_chan(v, am) : 255;
                }
                if (has_key && v == key) o[3] = 0;
                if (rgbkey >= 0 && (DWORD)((o[0] << 16) | (o[1] << 8) | o[2]) == (DWORD)rgbkey) o[3] = 0;
            }
        }
    }
    ((HRESULT (WINAPI *)(void *, void *))svt[32])(surf, NULL);
    *pw = w; *ph = h; *pkey = has_key;
    return rgba;
}

static DWORD vr_hash(const BYTE *p, size_t n)
{
    DWORD h = 2166136261u;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

/* per exported texture: where it came from and what we last wrote (change detection) */
static struct { void *surf; long rgbkey; DWORD hash; } g_vr_texinfo[4096];

static int vr_write_texture(int id, const BYTE *rgba, int w, int h, int keyed)
{
    MMVR_TEXHDR hdr;
    char name[32];
    memset(&hdr, 0, sizeof hdr);
    hdr.magic = MMVR_TEX_MAGIC; hdr.width = w; hdr.height = h; hdr.flags = keyed;
    snprintf(name, sizeof name, "t%d.bin", id);
    return vr_write_file(name, &hdr, sizeof hdr, rgba, (DWORD)w * h * 4);
}

/* rgbkey >= 0: decal transparency, pixels of that RGB colour become transparent */
static int vr_texture_id_ex(void *tex, long rgbkey)
{
    struct vrmap *e = vr_slot(rgbkey >= 0 ? g_vr_dtex : g_vr_tex, tex);
    void *surf = NULL;
    int i, n, w, h, has_key;
    BYTE *rgba;
    static int nfail;
    if (!e) return -1;
    if (e->key) return e->id;
    e->key = tex; e->id = -1;
    n = TEXREC_COUNT;
    for (i = 0; i < n && i < 2048; i++) {
        BYTE *rec = (BYTE *)(TEXREC_BASE + i * TEXREC_SIZE);
        if (*(void **)(rec + 0x450) == tex) { surf = *(void **)(rec + 0x44C); break; }
    }
    if (!surf) { if (nfail++ < 5) log_msg("vr: texture %p not in the game's table (%d entries)", tex, n); return -1; }
    rgba = vr_read_surface(surf, rgbkey, &w, &h, &has_key);
    if (!rgba) { if (nfail++ < 5) log_msg("vr: texture surface read failed"); return -1; }
    e->id = g_vr_ntex++;
    if (e->id < 4096) {
        g_vr_texdim[e->id][0] = (short)w; g_vr_texdim[e->id][1] = (short)h;
        g_vr_texinfo[e->id].surf = surf; g_vr_texinfo[e->id].rgbkey = rgbkey;
        g_vr_texinfo[e->id].hash = vr_hash(rgba, (size_t)w * h * 4);
    }
    vr_write_texture(e->id, rgba, w, h, has_key || rgbkey >= 0);
    free(rgba);
    return e->id;
}

/* Animated textures: re-read a few textures per tick (round robin) and re-export the
   ones whose pixels changed, announcing them to the viewer. */
static void vr_scan_textures(void)
{
    static int next;
    int k, budget = 24;
    for (k = 0; k < budget && g_vr_ntex > 0; k++) {
        int id = next++ % (g_vr_ntex < 4096 ? g_vr_ntex : 4096), w, h, keyed;
        BYTE *rgba;
        DWORD hs;
        if (!g_vr_texinfo[id].surf) continue;
        rgba = vr_read_surface(g_vr_texinfo[id].surf, g_vr_texinfo[id].rgbkey, &w, &h, &keyed);
        if (!rgba) continue;
        hs = vr_hash(rgba, (size_t)w * h * 4);
        if (hs != g_vr_texinfo[id].hash) {
            static int nlog;
            g_vr_texinfo[id].hash = hs;
            if (vr_write_texture(id, rgba, w, h, keyed || g_vr_texinfo[id].rgbkey >= 0)) {
                uint32_t q = g_vr->texdirty_seq;
                g_vr->texdirty[q % 256] = (uint32_t)id;
                InterlockedExchange((volatile LONG *)&g_vr->texdirty_seq, (LONG)(q + 1));
            }
            if (nlog++ < 30) log_msg("vr: texture %d changed (animated)", id);
        }
        free(rgba);
    }
}

/* ---- meshes ----------------------------------------------------------------------- */
static int vr_export_builder(void *mb, int id)
{
    void *mesh = NULL;
    unsigned ng, g;
    BYTE *buf = NULL;
    size_t cap = 0, len = 0;
    MMVR_MESHHDR mh;
    char name[32];
    int ok = 0;
#define PUT(p, n) do { size_t nn_ = (n); if (len + nn_ > cap) { cap = (len + nn_) * 2 + 4096; buf = (BYTE *)realloc(buf, cap); } memcpy(buf + len, (p), nn_); len += nn_; } while (0)
    if (((PFN_MB_CREATEMESH)VT(mb)[MB_CREATEMESH])(mb, &mesh) != 0 || !mesh) return 0;
    ng = ((PFN_MESH_GCOUNT)VT(mesh)[MESH_GETGROUPCOUNT])(mesh);
    memset(&mh, 0, sizeof mh);
    mh.magic = MMVR_MESH_MAGIC;
    PUT(&mh, sizeof mh);
    for (g = 0; g < ng && g < 256; g++) {
        unsigned vc = 0, fc = 0, vpf = 0, *fd, i, nidx = 0, *idx;
        DWORD ds = 0;
        RMVERTEX *rv;
        MMVR_GROUPHDR gh;
        void *tex = NULL;
        if (((PFN_MESH_GETGROUP)VT(mesh)[MESH_GETGROUP])(mesh, g, &vc, &fc, &vpf, &ds, NULL) != 0) continue;
        if (!vc || !fc || !ds || vc > 65536 || ds > 1u << 20) continue;
        fd = (unsigned *)malloc(ds * sizeof(unsigned));
        rv = (RMVERTEX *)malloc(vc * sizeof(RMVERTEX));
        idx = (unsigned *)malloc((size_t)ds * 3 * sizeof(unsigned));
        if (!fd || !rv || !idx) { free(fd); free(rv); free(idx); continue; }
        if (((PFN_MESH_GETGROUP)VT(mesh)[MESH_GETGROUP])(mesh, g, &vc, &fc, &vpf, &ds, fd) == 0 &&
            ((PFN_MESH_GETVERTS)VT(mesh)[MESH_GETVERTICES])(mesh, g, 0, vc, rv) == 0) {
            /* faces: fixed vpf, or (vpf == 0) count-prefixed; fan-triangulate */
            unsigned p = 0, f;
            for (f = 0; f < fc && p < ds; f++) {
                unsigned n = vpf ? vpf : fd[p++], k;
                if (p + n > ds) break;
                for (k = 2; k < n; k++) {
                    unsigned a = fd[p], b = fd[p + k - 1], c = fd[p + k];
                    if (a < vc && b < vc && c < vc) { idx[nidx++] = a; idx[nidx++] = b; idx[nidx++] = c; }
                }
                p += n;
            }
            memset(&gh, 0, sizeof gh);
            gh.vcount = vc; gh.icount = nidx;
            gh.color = ((PFN_MESH_GCOLOR)VT(mesh)[MESH_GETGROUPCOLOR])(mesh, g);
            gh.tex = -1;
            if (((PFN_MESH_GTEX)VT(mesh)[MESH_GETGROUPTEX])(mesh, g, &tex) == 0 && tex) {
                gh.tex = vr_texture_id(tex);
                vr_release(tex);
            }
            PUT(&gh, sizeof gh);
            for (i = 0; i < vc; i++) {
                MMVR_VERTEX v;
                v.p[0] = rv[i].px; v.p[1] = rv[i].py; v.p[2] = rv[i].pz;
                v.n[0] = rv[i].nx; v.n[1] = rv[i].ny; v.n[2] = rv[i].nz;
                v.uv[0] = rv[i].tu; v.uv[1] = rv[i].tv;
                PUT(&v, sizeof v);
            }
            PUT(idx, nidx * sizeof(unsigned));
            ((MMVR_MESHHDR *)buf)->groups++;
        }
        free(fd); free(rv); free(idx);
    }
    vr_release(mesh);
    snprintf(name, sizeof name, "m%d.bin", id);
    if (buf) ok = vr_write_file(name, buf, (DWORD)len, NULL, 0);
    free(buf);
#undef PUT
    return ok;
}

/* D3DRM decal: a texture used directly as a visual. Drawn as a camera-facing rectangle
   of GetDecalSize world units, with the texel GetDecalOrigin at the frame position.
   Texture slots (DX5 order, checked against the vtable code): GetDecalSize 22,
   GetDecalOrigin 23, GetDecalTransparency 28, GetDecalTransparentColor 29. */
static int vr_export_decal(void *tex, int id)
{
    float dw = 1, dh = 1;
    LONG ox = 0, oy = 0;
    int tid, tw, th, transp;
    long key = -1;
    MMVR_MESHHDR mh;
    MMVR_GROUPHDR gh;
    MMVR_VERTEX v[4];
    unsigned idx[6] = { 0, 1, 2, 2, 1, 3 };
    BYTE buf[sizeof mh + sizeof gh + sizeof v + sizeof idx];
    char name[32];
    float x0, x1, y0, y1;
    int k;
    ((HRESULT (WINAPI *)(void *, float *, float *))VT(tex)[22])(tex, &dw, &dh);
    ((HRESULT (WINAPI *)(void *, LONG *, LONG *))VT(tex)[23])(tex, &ox, &oy);
    transp = ((int (WINAPI *)(void *))VT(tex)[28])(tex);
    if (transp) key = (long)(((DWORD (WINAPI *)(void *))VT(tex)[29])(tex) & 0xFFFFFF);
    tid = vr_texture_id_ex(tex, key);
    if (tid < 0 || tid >= 4096) return 0;
    tw = g_vr_texdim[tid][0]; th = g_vr_texdim[tid][1];
    if (tw <= 0 || th <= 0) return 0;
    /* texel (ox, oy) sits at the frame origin; texture y runs downwards */
    x0 = -(float)ox / tw * dw; x1 = x0 + dw;
    y1 = (float)oy / th * dh; y0 = y1 - dh;
    for (k = 0; k < 4; k++) {
        v[k].p[0] = (k & 1) ? x1 : x0; v[k].p[1] = (k & 2) ? y0 : y1; v[k].p[2] = 0;
        v[k].n[0] = 0; v[k].n[1] = 0; v[k].n[2] = -1;
        v[k].uv[0] = (k & 1) ? 1.0f : 0.0f; v[k].uv[1] = (k & 2) ? 1.0f : 0.0f;
    }
    memset(&mh, 0, sizeof mh); mh.magic = MMVR_MESH_MAGIC; mh.groups = 1; mh.flags = MMVR_MESH_BILLBOARD;
    memset(&gh, 0, sizeof gh); gh.vcount = 4; gh.icount = 6; gh.color = 0xFFFFFFFF; gh.tex = tid;
    memcpy(buf, &mh, sizeof mh); memcpy(buf + sizeof mh, &gh, sizeof gh);
    memcpy(buf + sizeof mh + sizeof gh, v, sizeof v); memcpy(buf + sizeof mh + sizeof gh + sizeof v, idx, sizeof idx);
    {
        static int nlog;
        if (nlog++ < 8) log_msg("vr: decal %d: tex %d %dx%d size %.1fx%.1f origin %ld,%ld transp %d key %06lx scale %d",
                                id, tid, tw, th, dw, dh, ox, oy, transp, key, ((int (WINAPI *)(void *))VT(tex)[27])(tex));
    }
    snprintf(name, sizeof name, "m%d.bin", id);
    return vr_write_file(name, buf, sizeof buf, NULL, 0);
}

static int vr_mesh_id(void *visual)
{
    struct vrmap *e = vr_slot(g_vr_mesh, visual);
    char cls[32];
    if (!e) return -1;
    if (e->key) return e->id;
    e->key = visual; e->id = -1;
    obj_classname(visual, cls, sizeof cls);
    if (!lstrcmpA(cls, "Texture")) {                 /* decal (camera-facing sprite) */
        e->id = g_vr_nmesh++;
        if (!vr_export_decal(visual, e->id)) log_msg("vr: decal %d export failed", e->id);
        InterlockedExchange((volatile LONG *)&g_vr->meshes_written, g_vr_nmesh);
        return e->id;
    }
    if (lstrcmpA(cls, "Builder")) {
        static int nlog;
        if (nlog++ < 40) log_msg("vr: visual class '%s' (%p) not exported", cls, visual);
        return -1;
    }
    e->id = g_vr_nmesh++;
    if (!vr_export_builder(visual, e->id)) log_msg("vr: mesh %d export failed", e->id);
    InterlockedExchange((volatile LONG *)&g_vr->meshes_written, g_vr_nmesh);
    return e->id;
}

/* ---- per-tick capture ------------------------------------------------------------- */
static MMVR_FRAME *g_vr_out;

static void mat_mul(MAT4 r, MAT4 a, MAT4 b)     /* r = a * b (row vectors) */
{
    int i, j;
    MAT4 t;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
    memcpy(r, t, sizeof t);
}

static void vr_walk(void *frame, MAT4 parent, int depth)
{
    void *arr = NULL, *o;
    MAT4 local, world;
    DWORD i, n;
    if (depth > 40) return;
    if (g_fr_gettransform(frame, local) != 0) return;
    mat_mul(world, local, parent);
    if (((PFN_GETARRAY)VT(frame)[FR_GETVISUALS])(frame, &arr) == 0 && arr) {
        n = ((PFN_ARR_SIZE)VT(arr)[ARR_GETSIZE])(arr);
        for (i = 0; i < n && i < 64; i++)
            if (((PFN_ARR_ELEM)VT(arr)[ARR_GETELEMENT])(arr, i, &o) == 0 && o) {
                int id = vr_mesh_id(o);
                vr_release(o);
                if (id >= 0 && g_vr_out->count < MMVR_MAX_INST) {
                    MMVR_INST *in = &g_vr_out->inst[g_vr_out->count++];
                    int r;
                    in->mesh = (uint32_t)id;
                    in->key = (uint32_t)(UINT_PTR)frame;
                    for (r = 0; r < 4; r++) { in->m[r][0] = world[r][0]; in->m[r][1] = world[r][1]; in->m[r][2] = world[r][2]; }
                }
            }
        vr_release(arr);
    }
    arr = NULL;
    if (((PFN_GETARRAY)VT(frame)[FR_GETCHILDREN])(frame, &arr) == 0 && arr) {
        n = ((PFN_ARR_SIZE)VT(arr)[ARR_GETSIZE])(arr);
        for (i = 0; i < n && i < 4096; i++)
            if (((PFN_ARR_ELEM)VT(arr)[ARR_GETELEMENT])(arr, i, &o) == 0 && o) {
                vr_walk(o, world, depth + 1);
                vr_release(o);
            }
        vr_release(arr);
    }
}

static void vr_begin_generation(void)
{
    char dir[MAX_PATH];
    g_vr->generation++;
    snprintf(dir, sizeof dir, "%svrcache", g_home);
    CreateDirectoryA(dir, NULL);
    snprintf(g_vr_dir, sizeof g_vr_dir, "%s\\%lu", dir, (unsigned long)g_vr->generation);
    CreateDirectoryA(g_vr_dir, NULL);
    lstrcpynA(g_vr->cache_dir, g_vr_dir, sizeof g_vr->cache_dir);
    g_vr->meshes_written = 0;
    log_msg("vr: generation %lu -> %s", (unsigned long)g_vr->generation, g_vr_dir);
}

static int g_vr_gen_open;
static void vr_capture(void)
{
    void *scene = *(void **)0x0054AF08;
    MAT4 ident = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
    LARGE_INTEGER qc, qf;
    int slot, saved_internal = g_internal;
    static int logged;
    if (!g_vr || !scene || !g_fr_gettransform) return;
    if (!g_vr_gen_open) { vr_begin_generation(); g_vr_gen_open = 1; }
    slot = (int)((g_vr->latest + 1) % MMVR_SLOTS);
    g_vr_out = &g_vr->frames[slot];
    g_vr_out->count = 0;
    g_internal = 1;
    vr_walk(scene, ident, 0);
    g_internal = saved_internal;
    {   /* game camera: child of the scene, so its transform is already world space */
        void *cam = *(void **)0x0054AF0C;
        MAT4 cm;
        int r;
        if (cam && g_fr_gettransform(cam, cm) == 0)
            for (r = 0; r < 4; r++) { g_vr_out->cam[r][0] = cm[r][0]; g_vr_out->cam[r][1] = cm[r][1]; g_vr_out->cam[r][2] = cm[r][2]; }
    }
    QueryPerformanceCounter(&qc); QueryPerformanceFrequency(&qf);
    g_vr_out->t_ms = qc.QuadPart * 1000.0 / qf.QuadPart;
    g_vr_out->origin[0] = (float)ORIGIN_X;
    g_vr_out->origin[1] = (float)ORIGIN_Z;
    vr_scan_textures();
    g_vr_out->tick = ++g_vr->ticks;
    g_vr_out->generation = g_vr->generation;
    InterlockedExchange((volatile LONG *)&g_vr->latest, slot);
    g_vr_hud_pending = 1;
    if (!logged) { log_msg("vr: first capture %lu instances, %d meshes, %d textures",
                           (unsigned long)g_vr_out->count, g_vr_nmesh, g_vr_ntex); logged = 1; }
}

static void vr_on_teardown(void)
{
    if (!g_vr) return;
    memset(g_vr_mesh, 0, sizeof g_vr_mesh); memset(g_vr_tex, 0, sizeof g_vr_tex); memset(g_vr_dtex, 0, sizeof g_vr_dtex);
    g_vr_nmesh = g_vr_ntex = 0;
    memset(g_vr_texinfo, 0, sizeof g_vr_texinfo);
    g_vr_gen_open = 0;
}


/* Note: the game must keep focus (it is minimised and its race logic pauses when it
   loses focus), so the viewer window never activates; see tools/vrviewer. */
static void vr_subclass(HWND wnd) { (void)wnd; }


/* ---- Quest controller input -> the game's DirectInput keyboard buffer (0x54a0c0) ----
 * Called from the keyboard poll hook (0x415360) once per game loop. Steering is analog
 * on the controller and digital in the game, so partial stick deflection is pulsed
 * (pressed on that fraction of polls). Input older than 0.5 s is ignored. */
static int vr_inject_input(void)
{
    static uint32_t last_seq;
    static DWORD last_t;
    static float acc;
    BYTE *k = (BYTE *)0x0054A0C0;
    DWORD now = GetTickCount();
    float st, a;
    uint32_t b;
    if (!g_vr) return 0;
    if (g_vr->in_seq != last_seq) { last_seq = g_vr->in_seq; last_t = now; }
    else if (now - last_t > 500) return 0;
    st = g_vr->in_steer; a = fabsf(st);
    if (a >= 0.15f) {
        int on;
        if (a > 0.9f) on = 1;
        else { acc += a; on = acc >= 1.0f; if (on) acc -= 1.0f; }
        if (on) k[st < 0 ? 0xCB : 0xCD] = 0x80;               /* DIK_LEFT / DIK_RIGHT */
    } else acc = 0;
    b = g_vr->in_buttons;
    if (g_vr->in_accel > 0.3f || (b & MMVR_BTN_UP)) k[0xC8] = 0x80;    /* DIK_UP */
    if (g_vr->in_brake > 0.3f || (b & MMVR_BTN_DOWN)) k[0xD0] = 0x80;  /* DIK_DOWN */
    if (b & MMVR_BTN_FIRE) k[0x1C] = 0x80;                         /* DIK_RETURN */
    if (b & MMVR_BTN_ACTION) k[0x19] = 0x80;                       /* DIK_P */
    if (b & MMVR_BTN_ESC) k[0x01] = 0x80;                          /* DIK_ESCAPE */
    return 1;
}

/* ---- HUD overlay -> shared memory (once per logic tick) ----------------------------
 * src is the 320-wide overlay surface the game blits over the 3D view with its source
 * colour key (0x407f25); copy it as RGBA with the key made transparent. */
static void vr_hud_capture(void *src, RECT *sr)
{
    BYTE desc[0x6C];
    void **svt;
    DWORD ck[2], key = 0, rm, gm, bm, bpp;
    int has_key = 0, w, h, x, y, pitch;
    BYTE *bits;
    if (!g_vr || !g_vr_hud_pending || !src || !sr) return;
    g_vr_hud_pending = 0;
    svt = VT(src);
    if (((HRESULT (WINAPI *)(void *, DWORD, DWORD *))svt[16])(src, 8, ck) == 0) { key = ck[0]; has_key = 1; }
    memset(desc, 0, sizeof desc); *(DWORD *)desc = sizeof desc;
    if (((HRESULT (WINAPI *)(void *, RECT *, void *, DWORD, HANDLE))svt[25])(src, NULL, desc, 0x11, NULL) != 0) return;
    pitch = *(int *)(desc + 0x10); bits = *(BYTE **)(desc + 0x24); bpp = *(DWORD *)(desc + 0x54);
    rm = *(DWORD *)(desc + 0x58); gm = *(DWORD *)(desc + 0x5C); bm = *(DWORD *)(desc + 0x60);
    w = min(sr->right - sr->left, 320); h = min(sr->bottom - sr->top, 240);
    if (bits && (bpp == 16 || bpp == 32) && w > 0 && h > 0) {
        for (y = 0; y < h; y++) {
            const BYTE *row = bits + (size_t)(sr->top + y) * pitch;
            uint32_t *o = g_vr->hud_rgba + y * 320;
            for (x = 0; x < w; x++) {
                DWORD v = bpp == 16 ? ((const WORD *)row)[sr->left + x] : ((const DWORD *)row)[sr->left + x];
                if (has_key && v == key) { o[x] = 0; continue; }
                o[x] = 0xFF000000u | ((DWORD)mask_chan(v, bm) << 16) | ((DWORD)mask_chan(v, gm) << 8) | mask_chan(v, rm);
            }
        }
        g_vr->hud_w = w; g_vr->hud_h = h;
        InterlockedIncrement((volatile LONG *)&g_vr->hud_seq);
    }
    ((HRESULT (WINAPI *)(void *, void *))svt[32])(src, NULL);
}

/* ---- front-end screen -> shared memory ---------------------------------------------
 * Called from the present hook (0x4058a0) before the frame goes out. Only in the menu
 * modes (no race running): copies the back buffer (0x4891e4) so the viewer can show
 * the menus in the headset. */
static void vr_screen_capture(void)
{
    void *bb = *(void **)0x004891E4, **svt;
    BYTE desc[0x6C], *bits;
    DWORD bpp, rm, gm, bm;
    int w, h, x, y, pitch;
    static DWORD last;
    DWORD now = GetTickCount();
    if (!g_vr || !bb || g_race_active || now - last < 30) return;
    last = now;
    svt = VT(bb);
    memset(desc, 0, sizeof desc); *(DWORD *)desc = sizeof desc;
    if (((HRESULT (WINAPI *)(void *, RECT *, void *, DWORD, HANDLE))svt[25])(bb, NULL, desc, 0x11, NULL) != 0) return;
    h = *(int *)(desc + 0x08); w = *(int *)(desc + 0x0C); pitch = *(int *)(desc + 0x10);
    bits = *(BYTE **)(desc + 0x24); bpp = *(DWORD *)(desc + 0x54);
    rm = *(DWORD *)(desc + 0x58); gm = *(DWORD *)(desc + 0x5C); bm = *(DWORD *)(desc + 0x60);
    if (bits && (bpp == 16 || bpp == 32) && w > 0 && h > 0 && w <= 640 && h <= 480) {
        for (y = 0; y < h; y++) {
            const BYTE *row = bits + (size_t)y * pitch;
            uint32_t *o = g_vr->screen_rgba + y * 640;
            for (x = 0; x < w; x++) {
                DWORD v = bpp == 16 ? ((const WORD *)row)[x] : ((const DWORD *)row)[x];
                o[x] = 0xFF000000u | ((DWORD)mask_chan(v, bm) << 16) | ((DWORD)mask_chan(v, gm) << 8) | mask_chan(v, rm);
            }
        }
        g_vr->screen_w = w; g_vr->screen_h = h;
        g_vr->screen_ms = now;
        InterlockedIncrement((volatile LONG *)&g_vr->screen_seq);
    }
    ((HRESULT (WINAPI *)(void *, void *))svt[32])(bb, NULL);
}

static void install_vrexport(void)
{
    char path[MAX_PATH], v[8];
    snprintf(path, sizeof path, "%smmcda.ini", g_home);
    GetPrivateProfileStringA("vr", "export", "0", v, sizeof v, path);
    g_vr_on = atoi(v) != 0 || GetEnvironmentVariableA("MMCDA_VR", NULL, 0) != 0;
    if (!g_vr_on) return;
    g_vr_map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(MMVR_SHARED), MMVR_MAP_NAME);
    if (!g_vr_map) { log_msg("vr: shared memory failed %lu", GetLastError()); return; }
    g_vr = (MMVR_SHARED *)MapViewOfFile(g_vr_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MMVR_SHARED));
    if (!g_vr) return;
    g_vr->magic = MMVR_MAGIC;
    g_vr->version = MMVR_VERSION;
    g_vr->tick_ms = (float)TICK_MS;
    log_msg("vr export on (%u bytes shared)", (unsigned)sizeof(MMVR_SHARED));
}
