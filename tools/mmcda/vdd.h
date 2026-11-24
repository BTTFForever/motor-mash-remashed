/* vdd.h - DirectDraw / Direct3D (setup) for the new renderer ([render] backend=vulkan).
 *
 * The game draws its menus, HUD and texture images with DirectDraw. Here every surface is
 * a CPU DIB section in the game's own format (8-bit palettised or RGB565), so blits, locks
 * and GDI text (GetDC) work on plain memory, and a Flip hands the back buffer to Vulkan
 * (vkpresent.h). Direct3D IM is only used by the game to pick and create a device that it
 * hands to D3DRM, so IDirect3D2 / IDirect3DDevice2 just answer those setup calls.
 * Only the methods the game uses (RENDERER.md, API census) are implemented; the rest are
 * logging stubs from com_stubs.h.
 */

static int g_vdd_trace;   /* MMCDA_VDDTRACE=1 logs DirectDraw calls */
#define VTRACE(...) do { if (g_vdd_trace) log_msg("vdd: " __VA_ARGS__); } while (0)

/* ---- common object header ---- */
typedef struct vobj { void **vt; LONG ref; void (*destroy)(struct vobj *); } vobj;

static HRESULT vobj_qi_self(vobj *o, REFIID iid, void **out, const IID *const *ok, int n, const char *who)
{
    int i;
    VTRACE("%s QI %08lX", who, iid->Data1);
    if (!out) return E_POINTER;
    for (i = 0; i < n; i++)
        if (IsEqualGUID(iid, ok[i])) { *out = o; InterlockedIncrement(&o->ref); return S_OK; }
    *out = NULL;
    log_msg("vdd: %s QueryInterface for unsupported %08lX-%04X", who, iid->Data1, iid->Data2);
    return E_NOINTERFACE;
}
static ULONG WINAPI vobj_addref(vobj *o) { return InterlockedIncrement(&o->ref); }
static ULONG WINAPI vobj_release(vobj *o)
{
    LONG r = InterlockedDecrement(&o->ref);
    if (r == 0 && o->destroy) o->destroy(o);
    return r < 0 ? 0 : r;
}

/* ---- palette ---- */
typedef struct vpal { vobj hd; PALETTEENTRY e[256]; DWORD bgra[256]; DWORD gen; } vpal;

static void vpal_update(vpal *p)
{
    int i;
    p->gen++;
    for (i = 0; i < 256; i++)
        p->bgra[i] = 0xFF000000u | (p->e[i].peRed << 16) | (p->e[i].peGreen << 8) | p->e[i].peBlue;
}
static HRESULT WINAPI vpal_GetEntries(vpal *p, DWORD f, DWORD start, DWORD n, PALETTEENTRY *e)
{
    (void)f;
    if (start + n > 256) return DDERR_INVALIDPARAMS;
    memcpy(e, p->e + start, n * sizeof *e);
    return DD_OK;
}
static HRESULT WINAPI vpal_SetEntries(vpal *p, DWORD f, DWORD start, DWORD n, PALETTEENTRY *e)
{
    (void)f;
    if (start + n > 256) return DDERR_INVALIDPARAMS;
    memcpy(p->e + start, e, n * sizeof *e);
    vpal_update(p);
    return DD_OK;
}
static HRESULT WINAPI vpal_QI(vpal *p, REFIID iid, void **out)
{
    static const IID *const ok[] = { &IID_IUnknown, &IID_IDirectDrawPalette };
    return vobj_qi_self(&p->hd, iid, out, ok, 2, "palette");
}
static HRESULT WINAPI vpal_GetCaps(vpal *p, DWORD *caps) { (void)p; *caps = DDPCAPS_8BIT | DDPCAPS_ALLOW256; return DD_OK; }
static void vpal_destroy(vobj *o) { free(o); }

/* ---- clipper (the game only attaches it to the window) ---- */
typedef struct vclip { vobj hd; HWND wnd; } vclip;
static HRESULT WINAPI vclip_SetHWnd(vclip *c, DWORD f, HWND w) { (void)f; c->wnd = w; return DD_OK; }
static HRESULT WINAPI vclip_GetHWnd(vclip *c, HWND *w) { *w = c->wnd; return DD_OK; }
static HRESULT WINAPI vclip_QI(vclip *c, REFIID iid, void **out)
{
    static const IID *const ok[] = { &IID_IUnknown, &IID_IDirectDrawClipper };
    return vobj_qi_self(&c->hd, iid, out, ok, 2, "clipper");
}
static void vclip_destroy(vobj *o) { free(o); }

/* ---- surface ---- */
typedef struct vsurf {
    vobj hd;
    DWORD caps;                 /* DDSCAPS_* */
    int w, h, bpp, pitch;
    BYTE *bits;
    HBITMAP dib;
    HDC dc;
    vpal *pal;
    DDCOLORKEY ck_src;
    int has_ck;
    struct vsurf *back;         /* flip chain: front -> back */
    struct vsurf *front;        /* back -> front */
    struct vsurf *z;            /* attached depth buffer */
    DWORD zdepth;
    int is_primary;
    void *user;                 /* renderer data (texture cache etc.) */
    DWORD gen;                  /* bumped on every write, for texture re-upload */
} vsurf;

static struct {
    HWND wnd;
    int mode_w, mode_h, mode_bpp;
    vsurf *primary;
} g_vdd;

static void vsurf_destroy(vobj *o)
{
    vsurf *s = (vsurf *)o;
    if (s->dc) DeleteDC(s->dc);
    if (s->dib) DeleteObject(s->dib);
    else free(s->bits);
    if (s->pal) vobj_release(&s->pal->hd);
    if (s->back && s->back->front == s) { s->back->front = NULL; vobj_release(&s->back->hd); }
    if (s->z) vobj_release(&s->z->hd);
    if (g_vdd.primary == s) g_vdd.primary = NULL;
    free(s);
}

static void vsurf_fill_pf(int bpp, DDPIXELFORMAT *pf)
{
    memset(pf, 0, sizeof *pf);
    pf->dwSize = sizeof *pf;
    if (bpp == 8) {
        pf->dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
        pf->dwRGBBitCount = 8;
    } else {
        pf->dwFlags = DDPF_RGB;
        pf->dwRGBBitCount = 16;
        pf->dwRBitMask = 0xF800; pf->dwGBitMask = 0x07E0; pf->dwBBitMask = 0x001F;
    }
}

static void **vsurf_vt(void);
static vsurf *vsurf_new(int w, int h, int bpp, DWORD caps)
{
    vsurf *s = (vsurf *)calloc(1, sizeof *s);
    s->hd.vt = vsurf_vt(); s->hd.ref = 1; s->hd.destroy = vsurf_destroy;
    s->w = w; s->h = h; s->bpp = bpp; s->caps = caps;
    if (caps & DDSCAPS_ZBUFFER) {
        s->pitch = w * 2;
        s->bits = (BYTE *)calloc(1, (size_t)s->pitch * h);
    } else {
        struct { BITMAPINFOHEADER bh; DWORD masks[256]; } bi;
        memset(&bi, 0, sizeof bi);
        bi.bh.biSize = sizeof bi.bh;
        bi.bh.biWidth = w; bi.bh.biHeight = -h;          /* top-down */
        bi.bh.biPlanes = 1; bi.bh.biBitCount = (WORD)bpp;
        if (bpp == 16) { bi.bh.biCompression = BI_BITFIELDS; bi.masks[0] = 0xF800; bi.masks[1] = 0x07E0; bi.masks[2] = 0x001F; }
        else bi.bh.biClrUsed = 256;
        s->dib = CreateDIBSection(NULL, (BITMAPINFO *)&bi, DIB_RGB_COLORS, (void **)&s->bits, NULL, 0);
        s->pitch = ((w * bpp + 31) / 32) * 4;
        if (!s->dib) { s->bits = (BYTE *)calloc(1, (size_t)s->pitch * h); }
    }
    return s;
}

static void vsurf_fill_desc(vsurf *s, DDSURFACEDESC *d)
{
    DWORD size = d->dwSize ? d->dwSize : sizeof *d;
    memset(d, 0, size);
    d->dwSize = size;
    d->dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    d->dwWidth = s->w; d->dwHeight = s->h; d->lPitch = s->pitch;
    d->ddsCaps.dwCaps = s->caps;
    if (s->back) { d->dwFlags |= DDSD_BACKBUFFERCOUNT; d->dwBackBufferCount = 1; }
    if (s->caps & DDSCAPS_ZBUFFER) {
        d->dwFlags |= DDSD_ZBUFFERBITDEPTH; d->dwZBufferBitDepth = 16;
        memset(&d->ddpfPixelFormat, 0, sizeof d->ddpfPixelFormat);
        d->ddpfPixelFormat.dwSize = sizeof d->ddpfPixelFormat;
        d->ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
        d->ddpfPixelFormat.dwZBufferBitDepth = 16;
    } else vsurf_fill_pf(s->bpp, &d->ddpfPixelFormat);
    if (s->has_ck) { d->dwFlags |= DDSD_CKSRCBLT; d->ddckCKSrcBlt = s->ck_src; }
}

static void vdd_present(vsurf *frame);

static HRESULT WINAPI vsurf_QI(vsurf *s, REFIID iid, void **out)
{
    static const IID *const ok[] = { &IID_IUnknown, &IID_IDirectDrawSurface };
    return vobj_qi_self(&s->hd, iid, out, ok, 2, "surface");
}
static HRESULT WINAPI vsurf_AddAttachedSurface(vsurf *s, vsurf *a)
{
    if (!a) return DDERR_INVALIDPARAMS;
    if (a->caps & DDSCAPS_ZBUFFER) {
        if (s->z) vobj_release(&s->z->hd);
        s->z = a; vobj_addref(&a->hd);
        return DD_OK;
    }
    log_msg("vdd: AddAttachedSurface of a non-depth surface ignored");
    return DD_OK;
}
static HRESULT WINAPI vsurf_GetAttachedSurface(vsurf *s, DDSCAPS *caps, vsurf **out)
{
    vsurf *r = NULL;
    if (caps->dwCaps & DDSCAPS_ZBUFFER) r = s->z;
    else if (caps->dwCaps & DDSCAPS_BACKBUFFER) r = s->back;
    if (!r) { *out = NULL; return DDERR_NOTFOUND; }
    vobj_addref(&r->hd);
    *out = r;
    return DD_OK;
}
static HRESULT WINAPI vsurf_GetCaps(vsurf *s, DDSCAPS *c) { c->dwCaps = s->caps; return DD_OK; }
static HRESULT WINAPI vsurf_GetSurfaceDesc(vsurf *s, DDSURFACEDESC *d) { vsurf_fill_desc(s, d); return DD_OK; }
static HRESULT WINAPI vsurf_GetPixelFormat(vsurf *s, DDPIXELFORMAT *pf) { vsurf_fill_pf(s->bpp, pf); return DD_OK; }
static HRESULT WINAPI vsurf_IsLost(vsurf *s) { (void)s; return DD_OK; }
static HRESULT WINAPI vsurf_Restore(vsurf *s) { (void)s; return DD_OK; }
static HRESULT WINAPI vsurf_Initialize(vsurf *s, void *dd, DDSURFACEDESC *d) { (void)s; (void)dd; (void)d; return DDERR_ALREADYINITIALIZED; }
static HRESULT WINAPI vsurf_GetBltStatus(vsurf *s, DWORD f) { (void)s; (void)f; return DD_OK; }
static HRESULT WINAPI vsurf_GetFlipStatus(vsurf *s, DWORD f) { (void)s; (void)f; return DD_OK; }
static HRESULT WINAPI vsurf_SetClipper(vsurf *s, void *c) { (void)s; (void)c; return DD_OK; }
static HRESULT WINAPI vsurf_GetClipper(vsurf *s, void **c) { (void)s; *c = NULL; return DDERR_NOCLIPPERATTACHED; }

static HRESULT WINAPI vsurf_Lock(vsurf *s, RECT *r, DDSURFACEDESC *d, DWORD f, HANDLE ev)
{
    VTRACE("Lock %p(%dx%dx%d caps %lX)", s, s->w, s->h, s->bpp, s->caps);
    (void)f; (void)ev;
    if (s->w == 320 && s->h >= 200 && GetEnvironmentVariableA("MMCDA_BGTRACE", NULL, 0)) {
        static DWORD seen[32]; static int n;
        DWORD ra = (DWORD)_ReturnAddress();
        int i;
        for (i = 0; i < n && seen[i] != ra; i++) ;
        if (i == n && n < 32) { seen[n++] = ra; log_msg("bgtrace: Lock %p %dx%d caps %lX from %08lX", (void *)s, s->w, s->h, s->caps, ra); }
    }
    vsurf_fill_desc(s, d);
    d->dwFlags |= DDSD_LPSURFACE;
    d->lpSurface = s->bits + (r ? (size_t)r->top * s->pitch + r->left * (s->bpp / 8) : 0);
    s->gen++;
    return DD_OK;
}
static HRESULT WINAPI vsurf_Unlock(vsurf *s, void *p) { (void)p; s->gen++; return DD_OK; }

static HRESULT WINAPI vsurf_GetDC(vsurf *s, HDC *out)
{
    VTRACE("GetDC %p(%dx%d caps %lX)", s, s->w, s->h, s->caps);
    if (GetEnvironmentVariableA("MMCDA_BGTRACE", NULL, 0)) {
        static DWORD seen[32]; static int n;
        DWORD ra = (DWORD)_ReturnAddress();
        int i;
        for (i = 0; i < n && seen[i] != ra; i++) ;
        if (i == n && n < 32) { seen[n++] = ra; log_msg("bgtrace: GetDC %p %dx%d caps %lX from %08lX", (void *)s, s->w, s->h, s->caps, ra); }
    }
    if (!s->dib) return DDERR_CANTCREATEDC;
    if (!s->dc) {
        s->dc = CreateCompatibleDC(NULL);
        SelectObject(s->dc, s->dib);
    }
    if (s->bpp == 8 && s->pal) {
        RGBQUAD q[256];
        int i;
        for (i = 0; i < 256; i++) {
            q[i].rgbRed = s->pal->e[i].peRed; q[i].rgbGreen = s->pal->e[i].peGreen;
            q[i].rgbBlue = s->pal->e[i].peBlue; q[i].rgbReserved = 0;
        }
        SetDIBColorTable(s->dc, 0, 256, q);
    }
    *out = s->dc;
    return DD_OK;
}
static HRESULT WINAPI vsurf_ReleaseDC(vsurf *s, HDC dc) { (void)dc; GdiFlush(); s->gen++; return DD_OK; }

static HRESULT WINAPI vsurf_SetColorKey(vsurf *s, DWORD f, DDCOLORKEY *k)
{
    if (!(f & DDCKEY_SRCBLT)) return DD_OK;            /* only source keys are used */
    if (k) { s->ck_src = *k; s->has_ck = 1; } else s->has_ck = 0;
    return DD_OK;
}
static HRESULT WINAPI vsurf_GetColorKey(vsurf *s, DWORD f, DDCOLORKEY *k)
{
    if (!(f & DDCKEY_SRCBLT) || !s->has_ck) return DDERR_NOCOLORKEY;
    *k = s->ck_src;
    return DD_OK;
}
static HRESULT WINAPI vsurf_SetPalette(vsurf *s, vpal *p)
{
    if (p) vobj_addref(&p->hd);
    if (s->pal) vobj_release(&s->pal->hd);
    s->pal = p;
    s->gen++;
    return DD_OK;
}
static HRESULT WINAPI vsurf_GetPalette(vsurf *s, vpal **p)
{
    if (!s->pal) { *p = NULL; return DDERR_NOPALETTEATTACHED; }
    vobj_addref(&s->pal->hd);
    *p = s->pal;
    return DD_OK;
}

static void vsurf_fill(vsurf *s, const RECT *r, DWORD v)
{
    int x, y, bytes = s->bpp / 8;
    RECT c = { 0, 0, s->w, s->h };
    if (s->caps & DDSCAPS_ZBUFFER) bytes = 2;
    if (r) IntersectRect(&c, &c, r);
    for (y = c.top; y < c.bottom; y++) {
        BYTE *row = s->bits + (size_t)y * s->pitch;
        if (bytes == 1) memset(row + c.left, (BYTE)v, c.right - c.left);
        else if (bytes == 2) { WORD *p = (WORD *)row; for (x = c.left; x < c.right; x++) p[x] = (WORD)v; }
        else { DWORD *p = (DWORD *)row; for (x = c.left; x < c.right; x++) p[x] = v; }
    }
    s->gen++;
}

/* copy src rect to dst rect (nearest-neighbour stretch), optional source colour key */
static void vsurf_copy(vsurf *d, const RECT *dr0, vsurf *s, const RECT *sr0, int keyed, DWORD key)
{
    RECT dr = dr0 ? *dr0 : (RECT){ 0, 0, d->w, d->h };
    RECT sr = sr0 ? *sr0 : (RECT){ 0, 0, s->w, s->h };
    int dw = dr.right - dr.left, dh = dr.bottom - dr.top, sw = sr.right - sr.left, sh = sr.bottom - sr.top;
    int x, y;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    for (y = max(dr.top, 0); y < min(dr.bottom, d->h); y++) {
        int sy = sr.top + (int)((long long)(y - dr.top) * sh / dh);
        BYTE *drow = d->bits + (size_t)y * d->pitch;
        const BYTE *srow;
        if (sy < 0 || sy >= s->h) continue;
        srow = s->bits + (size_t)sy * s->pitch;
        for (x = max(dr.left, 0); x < min(dr.right, d->w); x++) {
            int sx = sr.left + (int)((long long)(x - dr.left) * sw / dw);
            DWORD v;
            if (sx < 0 || sx >= s->w) continue;
            if (s->bpp == 8) v = srow[sx]; else v = ((const WORD *)srow)[sx];
            if (keyed && v >= key && v <= key) continue;
            if (d->bpp == s->bpp) {
                if (d->bpp == 8) drow[x] = (BYTE)v; else ((WORD *)drow)[x] = (WORD)v;
            } else if (d->bpp == 16 && s->bpp == 8 && s->pal) {   /* palette to 565 */
                PALETTEENTRY e = s->pal->e[v];
                ((WORD *)drow)[x] = (WORD)(((e.peRed >> 3) << 11) | ((e.peGreen >> 2) << 5) | (e.peBlue >> 3));
            }
        }
    }
    d->gen++;
}

/* debug (MMCDA_BGTRACE): who copies a full-screen picture onto the back buffer */
static void vdd_bgtrace(vsurf *d, const RECT *dr, vsurf *s, DWORD ra)
{
    static struct { void *s; DWORD ra; } seen[32];
    static int n;
    int i;
    if (!s || !GetEnvironmentVariableA("MMCDA_BGTRACE", NULL, 0)) return;
    if (!(d->caps & DDSCAPS_BACKBUFFER) && dr && (dr->right - dr->left) * (dr->bottom - dr->top) < 160 * 120) return;
    for (i = 0; i < n; i++) if (seen[i].s == s && seen[i].ra == ra) return;
    if (n < 32) { seen[n].s = s; seen[n].ra = ra; n++; }
    log_msg("bgtrace: %p (%dx%d) -> %p %dx%d at %ld,%ld-%ld,%ld from %08lX", s, s->w, s->h, (void *)d, d->w, d->h,
            dr ? dr->left : 0, dr ? dr->top : 0, dr ? dr->right : 0, dr ? dr->bottom : 0, ra);
}
static HRESULT WINAPI vsurf_Blt(vsurf *d, RECT *dr, vsurf *s, RECT *sr, DWORD f, DDBLTFX *fx)
{
    VTRACE("Blt dst %p(%dx%d caps %lX) src %p(%dx%d) flags %lX", d, d->w, d->h, d->caps, s, s ? s->w : 0, s ? s->h : 0, f);
    if (f & DDBLT_COLORFILL) {
        /* A fill of (most of) the back buffer is a background clear (the game's, or MMCDA's
           hard clear with the sky colour), not 2D drawing: the 3D pass clears to the scene
           background itself, and the 2D layer goes on top, so leave it transparent. */
        DWORD c = fx ? fx->dwFillColor : 0;
        if ((d->caps & DDSCAPS_BACKBUFFER) &&
            (!dr || (dr->right - dr->left) * (dr->bottom - dr->top) * 10 >= d->w * d->h * 9)) c = 0;
        vsurf_fill(d, dr, c);
    }
    else if (f & DDBLT_DEPTHFILL) vsurf_fill(d, dr, fx ? fx->dwFillDepth : 0xFFFF);
    else if (s) {
        int keyed = 0;
        DWORD key = 0;
        if ((f & DDBLT_KEYSRCOVERRIDE) && fx) { keyed = 1; key = fx->ddckSrcColorkey.dwColorSpaceLowValue; }
        else if ((f & DDBLT_KEYSRC) && s->has_ck) { keyed = 1; key = s->ck_src.dwColorSpaceLowValue; }
        vdd_bgtrace(d, dr, s, (DWORD)_ReturnAddress());
        vsurf_copy(d, dr, s, sr, keyed, key);
    } else {
        static int once;
        if (!once++) log_msg("vdd: Blt flags %08lX without source ignored", f);
    }
    if (d->is_primary && d == g_vdd.primary) vdd_present(d);
    return DD_OK;
}
static HRESULT WINAPI vsurf_BltFast(vsurf *d, DWORD x, DWORD y, vsurf *s, RECT *sr, DWORD trans)
{
    VTRACE("BltFast dst %p(%dx%d caps %lX) at %lu,%lu src %p(%dx%d)", d, d->w, d->h, d->caps, x, y, s, s->w, s->h);
    RECT r = sr ? *sr : (RECT){ 0, 0, s->w, s->h }, dr;
    SetRect(&dr, x, y, x + (r.right - r.left), y + (r.bottom - r.top));
    vdd_bgtrace(d, &dr, s, (DWORD)_ReturnAddress());
    vsurf_copy(d, &dr, s, &r, (trans & DDBLTFAST_SRCCOLORKEY) && s->has_ck, s->ck_src.dwColorSpaceLowValue);
    if (d->is_primary && d == g_vdd.primary) vdd_present(d);
    return DD_OK;
}
static HRESULT WINAPI vsurf_Flip(vsurf *s, vsurf *over, DWORD f)
{
    /* only the current display's chain reaches the screen: the front end keeps flipping its
       old 320x240 surfaces for a while after a mode switch (dgVoodoo ignores those too) */
    if (s != g_vdd.primary) return DD_OK;
    VTRACE("Flip %p", s);
    (void)over; (void)f;
    if (s->back) vdd_present(s->back);
    return DD_OK;
}

static void **vsurf_vt(void)
{
    static int init;
    if (!init) {
        void **v = IDirectDrawSurface_vt;
        init = 1;
        IDirectDrawSurface_vt_init();
        v[IDirectDrawSurface_QueryInterface] = (void *)vsurf_QI;
        v[IDirectDrawSurface_AddRef] = (void *)vobj_addref;
        v[IDirectDrawSurface_Release] = (void *)vobj_release;
        v[IDirectDrawSurface_AddAttachedSurface] = (void *)vsurf_AddAttachedSurface;
        v[IDirectDrawSurface_Blt] = (void *)vsurf_Blt;
        v[IDirectDrawSurface_BltFast] = (void *)vsurf_BltFast;
        v[IDirectDrawSurface_Flip] = (void *)vsurf_Flip;
        v[IDirectDrawSurface_GetAttachedSurface] = (void *)vsurf_GetAttachedSurface;
        v[IDirectDrawSurface_GetBltStatus] = (void *)vsurf_GetBltStatus;
        v[IDirectDrawSurface_GetCaps] = (void *)vsurf_GetCaps;
        v[IDirectDrawSurface_GetClipper] = (void *)vsurf_GetClipper;
        v[IDirectDrawSurface_GetColorKey] = (void *)vsurf_GetColorKey;
        v[IDirectDrawSurface_GetDC] = (void *)vsurf_GetDC;
        v[IDirectDrawSurface_GetFlipStatus] = (void *)vsurf_GetFlipStatus;
        v[IDirectDrawSurface_GetPalette] = (void *)vsurf_GetPalette;
        v[IDirectDrawSurface_GetPixelFormat] = (void *)vsurf_GetPixelFormat;
        v[IDirectDrawSurface_GetSurfaceDesc] = (void *)vsurf_GetSurfaceDesc;
        v[IDirectDrawSurface_Initialize] = (void *)vsurf_Initialize;
        v[IDirectDrawSurface_IsLost] = (void *)vsurf_IsLost;
        v[IDirectDrawSurface_Lock] = (void *)vsurf_Lock;
        v[IDirectDrawSurface_ReleaseDC] = (void *)vsurf_ReleaseDC;
        v[IDirectDrawSurface_Restore] = (void *)vsurf_Restore;
        v[IDirectDrawSurface_SetClipper] = (void *)vsurf_SetClipper;
        v[IDirectDrawSurface_SetColorKey] = (void *)vsurf_SetColorKey;
        v[IDirectDrawSurface_SetPalette] = (void *)vsurf_SetPalette;
        v[IDirectDrawSurface_Unlock] = (void *)vsurf_Unlock;
    }
    return IDirectDrawSurface_vt;
}

/* ---- presenting ---- */
static void (*g_vdd_render3d)(void);   /* vrm.h: draws the frame's 3D, then the 2D overlay */

static void vdd_present(vsurf *frame)
{
    int aw = frame->w, ah = frame->h;
    vsurf *prim = g_vdd.primary;
    {   /* MMCDA_VKDUMP=n: write the n-th presented frame to vkframe.bin (w, h, bpp, pitch, bits) */
        static int count, want = -1;
        if (want < 0) { char v[16] = ""; GetEnvironmentVariableA("MMCDA_VKDUMP", v, sizeof v); want = atoi(v); }
        if (want > 0 && ++count == want) {
            char path[MAX_PATH]; FILE *f;
            snprintf(path, sizeof path, "%svkframe.bin", g_home);
            if ((f = fopen(path, "wb"))) {
                int hdr[4] = { frame->w, frame->h, frame->bpp, frame->pitch };
                fwrite(hdr, sizeof hdr, 1, f); fwrite(frame->bits, frame->pitch, frame->h, f); fclose(f);
            }
        }
    }
    {   /* MMCDA_SEQ=1: dump the 2D layer of the first 40 frames of each non-menu mode (vkseq_NN.bin) */
        static int seq = -1, n, lastw;
        if (seq < 0) seq = GetEnvironmentVariableA("MMCDA_SEQ", NULL, 0) != 0;
        if (frame->w != lastw) { n = 0; lastw = frame->w; }
        if (seq && frame->w != 320 && n < 40) {
            char path[MAX_PATH]; FILE *f;
            if (n == 5 || n == 15 || n == 25 || n == 36 || n == 39) {   /* game globals snapshot */
                snprintf(path, sizeof path, "%svkmem_%02d.bin", g_home, n);
                if ((f = fopen(path, "wb"))) { fwrite((void *)0x43E000, 1, 0x635000 - 0x43E000, f); fclose(f); }
            }
            snprintf(path, sizeof path, "%svkseq_%02d.bin", g_home, n++);
            if ((f = fopen(path, "wb"))) {
                int hdr[4] = { frame->w, frame->h, frame->bpp, frame->pitch };
                fwrite(hdr, sizeof hdr, 1, f); fwrite(frame->bits, frame->pitch, frame->h, f); fclose(f);
            }
        }
    }
    if (!g_vdd.wnd || !vkp_init(g_vdd.wnd)) return;
    if (aw * 5 == ah * 8) ah = aw * 3 / 4;          /* 320x200 / 640x400 shown at 4:3 */
    vkp_present(frame->bits, frame->w, frame->h, frame->pitch, frame->bpp,
                frame->bpp == 8 ? ((frame->pal ? frame->pal : (prim ? prim->pal : NULL)) ?
                    (frame->pal ? frame->pal : prim->pal)->bgra : NULL) : NULL, aw, ah);
}

/* ---- Direct3D (setup only) ---- */
typedef struct vd3ddev { vobj hd; vsurf *target; } vd3ddev;
typedef struct vd3d { vobj hd; } vd3d;
static vd3d g_vd3d;

static void vd3d_fill_desc(D3DDEVICEDESC *d, int hw)
{
    DWORD size = d->dwSize ? d->dwSize : sizeof *d;
    memset(d, 0, size);
    d->dwSize = size;
    if (!hw) return;
    d->dwFlags = D3DDD_COLORMODEL | D3DDD_DEVCAPS | D3DDD_TRICAPS | D3DDD_LINECAPS |
                 D3DDD_DEVICERENDERBITDEPTH | D3DDD_DEVICEZBUFFERBITDEPTH | D3DDD_MAXBUFFERSIZE |
                 D3DDD_MAXVERTEXCOUNT;
    d->dcmColorModel = D3DCOLOR_RGB;
    d->dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_EXECUTESYSTEMMEMORY |
                   D3DDEVCAPS_TEXTURESYSTEMMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY |
                   D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_HWRASTERIZATION;
    d->dpcTriCaps.dwSize = sizeof d->dpcTriCaps;
    d->dpcTriCaps.dwMiscCaps = D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW;
    d->dpcTriCaps.dwRasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_SUBPIXEL;
    d->dpcTriCaps.dwZCmpCaps = 0xFF;
    d->dpcTriCaps.dwSrcBlendCaps = d->dpcTriCaps.dwDestBlendCaps = 0x1FFF;
    d->dpcTriCaps.dwAlphaCmpCaps = 0xFF;
    d->dpcTriCaps.dwShadeCaps = 0xFFFFF;
    d->dpcTriCaps.dwTextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_TRANSPARENCY | D3DPTEXTURECAPS_ALPHA;
    d->dpcTriCaps.dwTextureFilterCaps = D3DPTFILTERCAPS_NEAREST | D3DPTFILTERCAPS_LINEAR;
    d->dpcTriCaps.dwTextureBlendCaps = D3DPTBLENDCAPS_MODULATE | D3DPTBLENDCAPS_DECAL | D3DPTBLENDCAPS_COPY;
    d->dpcTriCaps.dwTextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_CLAMP;
    d->dpcLineCaps = d->dpcTriCaps;
    d->dwDeviceRenderBitDepth = DDBD_16;
    d->dwDeviceZBufferBitDepth = DDBD_16;
    d->dwMaxBufferSize = 0;
    d->dwMaxVertexCount = 65535;
}

static HRESULT WINAPI vd3d_QI(vd3d *d, REFIID iid, void **out)
{
    static const IID *const ok[] = { &IID_IUnknown, &IID_IDirect3D2 };
    return vobj_qi_self(&d->hd, iid, out, ok, 2, "IDirect3D2");
}
/* The game skips hardware devices (flag 0x62FAB4 is set) and runs on Direct3D's software
   devices; with dgVoodoo these are its emulated Ramp, RGB and MMX devices. Report the same
   three, with the same capabilities (as dgVoodoo reports them, DX3-size descriptions). */
static const DWORD k_d3d_ramp[51] = { 0x000000CC, 0x000001FF, 0x00000001, 0x0000055B, 0x00000008, 0x00000001, 0x00000001, 0x00000010, 0x0000000F, 0x00000002, 0x00000008, 0x00000038, 0x00000070, 0x00000051, 0x000000FF, 0x00000000, 0x00000000, 0x00000000, 0x0000A145, 0x0000000B, 0x00000005, 0x00000042, 0x00000001, 0x00000004, 0x00000004, 0x00000038, 0x00000070, 0x00000051, 0x000000FF, 0x00000000, 0x00000000, 0x00000000, 0x0000A145, 0x0000000B, 0x00000005, 0x00000042, 0x00000001, 0x00000004, 0x00000004, 0x00000F00, 0x00000400, 0x00000000, 0x000007E5, 0x00000001, 0x00000001, 0x00000800, 0x00000800, 0x00000000, 0x00000000, 0x00000000, 0x00000000 };
static const DWORD k_d3d_rgb[51] = { 0x000000CC, 0x000001FF, 0x00000002, 0x00000D51, 0x00000008, 0x00000001, 0x00000001, 0x00000010, 0x00000007, 0x00000001, 0x00000008, 0x00000038, 0x00000003, 0x000361B1, 0x000000FF, 0x00001FFF, 0x000007FF, 0x000000FF, 0x000C528A, 0x0000001D, 0x0000003F, 0x000000CF, 0x0000001F, 0x00000000, 0x00000000, 0x00000038, 0x00000073, 0x000361B1, 0x000000FF, 0x00001FFF, 0x000007FF, 0x000000FF, 0x000C528A, 0x0000001D, 0x0000003F, 0x000000CF, 0x0000001F, 0x00000000, 0x00000000, 0x00000D00, 0x00000600, 0x00000000, 0x00010000, 0x00000001, 0x00000001, 0x00000800, 0x00000800, 0x00000000, 0x00000000, 0x00000000, 0x00000000 };

static HRESULT WINAPI vd3d_EnumDevices(vd3d *d, LPD3DENUMDEVICESCALLBACK cb, void *ctx)
{
    static const GUID ramp = { 0xF2086B20, 0x259F, 0x11CF, { 0xA3, 0x1A, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56 } };
    static const GUID rgb  = { 0xA4665C60, 0x2673, 0x11CF, { 0xA3, 0x1A, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56 } };
    static const GUID mmx  = { 0x881949A1, 0xD6F3, 0x11D0, { 0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29 } };
    struct { const GUID *g; const char *desc, *name; const DWORD *caps; } devs[3] = {
        { &ramp, "Vulkan (Ramp)", "Ramp Emulation", k_d3d_ramp },
        { &rgb, "Vulkan (RGB)", "RGB Emulation", k_d3d_rgb },
        { &mmx, "Vulkan (MMX)", "MMX Emulation", k_d3d_rgb } };
    int i;
    (void)d;
    VTRACE("EnumDevices");
    for (i = 0; i < 3; i++) {
        DWORD hw[64], sw[64];
        GUID g = *devs[i].g;
        memset(hw, 0, sizeof hw); memset(sw, 0, sizeof sw);
        hw[0] = 0xCC;
        memcpy(sw, devs[i].caps, 51 * 4);
        if (cb(&g, (char *)devs[i].desc, (char *)devs[i].name, (D3DDEVICEDESC *)hw, (D3DDEVICEDESC *)sw, ctx) != D3DENUMRET_OK) break;
    }
    return D3D_OK;
}
static void **vd3ddev_vt(void);
static void vd3ddev_destroy(vobj *o) { vd3ddev *v = (vd3ddev *)o; if (v->target) vobj_release(&v->target->hd); free(v); }
static HRESULT WINAPI vd3d_CreateDevice(vd3d *d, REFCLSID cls, vsurf *target, vd3ddev **out)
{
    {
        VTRACE("CreateDevice");
    }
    vd3ddev *v = (vd3ddev *)calloc(1, sizeof *v);
    (void)d; (void)cls;
    v->hd.vt = vd3ddev_vt(); v->hd.ref = 1; v->hd.destroy = vd3ddev_destroy;
    v->target = target;
    if (target) vobj_addref(&target->hd);
    *out = v;
    return D3D_OK;
}
static HRESULT WINAPI vd3ddev_QI(vd3ddev *v, REFIID iid, void **out)
{
    static const IID *const ok[] = { &IID_IUnknown, &IID_IDirect3DDevice2 };
    return vobj_qi_self(&v->hd, iid, out, ok, 2, "IDirect3DDevice2");
}
static HRESULT WINAPI vd3ddev_GetCaps(vd3ddev *v, D3DDEVICEDESC *hw, D3DDEVICEDESC *sw)
{
    (void)v;
    if (hw) vd3d_fill_desc(hw, 1);
    if (sw) vd3d_fill_desc(sw, 0);
    return D3D_OK;
}
static HRESULT WINAPI vd3ddev_Viewport(vd3ddev *v, void *vp) { (void)v; (void)vp; return D3D_OK; }
static HRESULT WINAPI vd3ddev_EnumTextureFormats(vd3ddev *v, LPD3DENUMTEXTUREFORMATSCALLBACK cb, void *ctx)
{
    DDSURFACEDESC d;
    int bpp;
    (void)v;
    for (bpp = 8; bpp <= 16; bpp += 8) {
        memset(&d, 0, sizeof d);
        d.dwSize = sizeof d;
        d.dwFlags = DDSD_CAPS | DDSD_PIXELFORMAT;
        d.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
        vsurf_fill_pf(bpp, &d.ddpfPixelFormat);
        if (cb(&d, ctx) != D3DENUMRET_OK) break;
    }
    return D3D_OK;
}
static HRESULT WINAPI vd3ddev_GetDirect3D(vd3ddev *v, vd3d **out) { (void)v; *out = &g_vd3d; vobj_addref(&g_vd3d.hd); return D3D_OK; }
static HRESULT WINAPI vd3ddev_Ok0(vd3ddev *v) { (void)v; return D3D_OK; }
static HRESULT WINAPI vd3ddev_Ok2(vd3ddev *v, DWORD a, DWORD b) { (void)v; (void)a; (void)b; return D3D_OK; }

static void **vd3ddev_vt(void)
{
    static int init;
    if (!init) {
        void **t = IDirect3DDevice2_vt;
        init = 1;
        IDirect3DDevice2_vt_init();
        t[IDirect3DDevice2_QueryInterface] = (void *)vd3ddev_QI;
        t[IDirect3DDevice2_AddRef] = (void *)vobj_addref;
        t[IDirect3DDevice2_Release] = (void *)vobj_release;
        t[IDirect3DDevice2_GetCaps] = (void *)vd3ddev_GetCaps;
        t[IDirect3DDevice2_AddViewport] = (void *)vd3ddev_Viewport;
        t[IDirect3DDevice2_DeleteViewport] = (void *)vd3ddev_Viewport;
        t[IDirect3DDevice2_EnumTextureFormats] = (void *)vd3ddev_EnumTextureFormats;
        t[IDirect3DDevice2_BeginScene] = (void *)vd3ddev_Ok0;
        t[IDirect3DDevice2_EndScene] = (void *)vd3ddev_Ok0;
        t[IDirect3DDevice2_GetDirect3D] = (void *)vd3ddev_GetDirect3D;
        t[IDirect3DDevice2_SetRenderState] = (void *)vd3ddev_Ok2;
        t[IDirect3DDevice2_SetLightState] = (void *)vd3ddev_Ok2;
    }
    return IDirect3DDevice2_vt;
}

/* ---- DirectDraw ---- */
typedef struct vdd { vobj hd; } vdd;

static HRESULT WINAPI vdd_QI(vdd *d, REFIID iid, void **out)
{
    static const IID *const ok[] = { &IID_IUnknown, &IID_IDirectDraw };
    if (IsEqualGUID(iid, &IID_IDirect3D2)) {
        g_vd3d.hd.ref++;
        *out = &g_vd3d;
        return S_OK;
    }
    return vobj_qi_self(&d->hd, iid, out, ok, 2, "IDirectDraw");
}
static void vdd_destroy(vobj *o) { free(o); }

static HRESULT WINAPI vdd_SetCooperativeLevel(vdd *d, HWND w, DWORD f)
{
    {
        VTRACE("SetCooperativeLevel %p %lX", w, f);
    }
    (void)d; (void)f;
    if (w) g_vdd.wnd = w;
    return DD_OK;
}
static HRESULT WINAPI vdd_SetDisplayMode(vdd *d, DWORD w, DWORD h, DWORD bpp)
{
    (void)d;
    g_vdd.mode_w = w; g_vdd.mode_h = h; g_vdd.mode_bpp = bpp;
    log_msg("vdd: display mode %lux%lux%lu", w, h, bpp);
    return DD_OK;
}
static HRESULT WINAPI vdd_RestoreDisplayMode(vdd *d) { (void)d; return DD_OK; }
static HRESULT WINAPI vdd_WaitForVerticalBlank(vdd *d, DWORD f, HANDLE e) { (void)d; (void)f; (void)e; return DD_OK; }
static HRESULT WINAPI vdd_FlipToGDISurface(vdd *d) { (void)d; return DD_OK; }

static HRESULT WINAPI vdd_GetDisplayMode(vdd *d, DDSURFACEDESC *s)
{
    {
        VTRACE("GetDisplayMode");
    }
    DWORD size = s->dwSize ? s->dwSize : sizeof *s;
    int w = g_vdd.mode_w, h = g_vdd.mode_h, bpp = g_vdd.mode_bpp;
    (void)d;
    if (!w) { w = GetSystemMetrics(SM_CXSCREEN); h = GetSystemMetrics(SM_CYSCREEN); bpp = 16; }
    memset(s, 0, size);
    s->dwSize = size;
    s->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    s->dwWidth = w; s->dwHeight = h; s->lPitch = w * bpp / 8;
    vsurf_fill_pf(bpp, &s->ddpfPixelFormat);
    return DD_OK;
}

static HRESULT WINAPI vdd_EnumDisplayModes(vdd *d, DWORD f, DDSURFACEDESC *filter, void *ctx, LPDDENUMMODESCALLBACK cb)
{
    {
        VTRACE("EnumDisplayModes");
    }
    static const WORD modes[][3] = {
        { 320, 200, 16 }, { 320, 240, 16 }, { 512, 384, 16 }, { 640, 360, 16 },
        { 640, 400, 16 }, { 640, 480, 16 }, { 800, 600, 16 }, { 854, 480, 16 }, { 960, 540, 16 },
        { 1024, 576, 16 }, { 1024, 768, 16 } };
    int i;
    (void)d; (void)f; (void)filter;
    for (i = 0; i < (int)(sizeof modes / sizeof modes[0]); i++) {
        DDSURFACEDESC s;
        memset(&s, 0, sizeof s);
        s.dwSize = sizeof s;
        s.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
        s.dwWidth = modes[i][0]; s.dwHeight = modes[i][1];
        s.lPitch = modes[i][0] * modes[i][2] / 8;
        s.dwRefreshRate = 60;
        vsurf_fill_pf(modes[i][2], &s.ddpfPixelFormat);
        if (cb(&s, ctx) != DDENUMRET_OK) break;
    }
    return DD_OK;
}

static HRESULT WINAPI vdd_GetCaps(vdd *d, DDCAPS *hw, DDCAPS *hel)
{
    {
        VTRACE("GetCaps %lu %lu", hw ? hw->dwSize : 0, hel ? hel->dwSize : 0);
    }
    (void)d;
    if (hw) {
        DWORD size = hw->dwSize ? hw->dwSize : sizeof *hw;
        memset(hw, 0, size);
        hw->dwSize = size;
        hw->dwCaps = DDCAPS_3D | DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTDEPTHFILL | DDCAPS_BLTSTRETCH |
                     DDCAPS_COLORKEY | DDCAPS_PALETTE | DDCAPS_CANBLTSYSMEM;
        hw->dwCaps2 = DDCAPS2_NOPAGELOCKREQUIRED | DDCAPS2_WIDESURFACES;
        hw->dwCKeyCaps = DDCKEYCAPS_SRCBLT;
        hw->dwPalCaps = DDPCAPS_8BIT;
        hw->dwZBufferBitDepths = DDBD_16;
        hw->dwVidMemTotal = hw->dwVidMemFree = 256u << 20;
        /* the game passes the DirectX 5 DDCAPS (364 bytes); the SDK's is the larger DX7 one,
           so only write fields that fit the caller's size */
        if (size >= offsetof(DDCAPS, ddsCaps) + sizeof(DWORD)) hw->ddsCaps.dwCaps = DDSCAPS_3DDEVICE | DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_TEXTURE |
                             DDSCAPS_ZBUFFER | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_PRIMARYSURFACE |
                             DDSCAPS_VIDEOMEMORY | DDSCAPS_SYSTEMMEMORY | DDSCAPS_PALETTE;
    }
    if (hel) {
        /* software-emulation caps: the game draws its sprites with the 2D overlay (as with
           dgVoodoo) only when these report stretch blits (DDCAPS_BLTSTRETCH, checked at 0x40712D);
           otherwise it turns every sprite into a D3DRM decal */
        DWORD size = hel->dwSize ? hel->dwSize : sizeof *hel;
        memset(hel, 0, size);
        hel->dwSize = size;
        hel->dwCaps = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTSTRETCH | DDCAPS_COLORKEY | DDCAPS_PALETTE;
        hel->dwCKeyCaps = DDCKEYCAPS_SRCBLT;
        hel->dwPalCaps = DDPCAPS_8BIT;
    }
    return DD_OK;
}

static HRESULT WINAPI vdd_CreatePalette(vdd *d, DWORD f, PALETTEENTRY *e, vpal **out, IUnknown *outer)
{
    static void *vt[7];
    vpal *p = (vpal *)calloc(1, sizeof *p);
    (void)d; (void)f; (void)outer;
    if (!vt[0]) {
        int i;
        IDirectDrawPalette_vt_init();
        for (i = 0; i < 7; i++) vt[i] = IDirectDrawPalette_vt[i];
        vt[IDirectDrawPalette_QueryInterface] = (void *)vpal_QI;
        vt[IDirectDrawPalette_AddRef] = (void *)vobj_addref;
        vt[IDirectDrawPalette_Release] = (void *)vobj_release;
        vt[IDirectDrawPalette_GetCaps] = (void *)vpal_GetCaps;
        vt[IDirectDrawPalette_GetEntries] = (void *)vpal_GetEntries;
        vt[IDirectDrawPalette_SetEntries] = (void *)vpal_SetEntries;
    }
    p->hd.vt = vt; p->hd.ref = 1; p->hd.destroy = vpal_destroy;
    if (e) memcpy(p->e, e, sizeof p->e);
    vpal_update(p);
    *out = p;
    return DD_OK;
}

static HRESULT WINAPI vdd_CreateClipper(vdd *d, DWORD f, vclip **out, IUnknown *outer)
{
    static void *vt[9];
    vclip *c = (vclip *)calloc(1, sizeof *c);
    (void)d; (void)f; (void)outer;
    if (!vt[0]) {
        int i;
        IDirectDrawClipper_vt_init();
        for (i = 0; i < 9; i++) vt[i] = IDirectDrawClipper_vt[i];
        vt[IDirectDrawClipper_QueryInterface] = (void *)vclip_QI;
        vt[IDirectDrawClipper_AddRef] = (void *)vobj_addref;
        vt[IDirectDrawClipper_Release] = (void *)vobj_release;
        vt[IDirectDrawClipper_SetHWnd] = (void *)vclip_SetHWnd;
        vt[IDirectDrawClipper_GetHWnd] = (void *)vclip_GetHWnd;
    }
    c->hd.vt = vt; c->hd.ref = 1; c->hd.destroy = vclip_destroy;
    *out = c;
    return DD_OK;
}

static HRESULT WINAPI vdd_CreateSurface(vdd *d, DDSURFACEDESC *sd, vsurf **out, IUnknown *outer)
{
    DWORD caps = (sd->dwFlags & DDSD_CAPS) ? sd->ddsCaps.dwCaps : 0;
    int w = (sd->dwFlags & DDSD_WIDTH) ? (int)sd->dwWidth : g_vdd.mode_w;
    int h = (sd->dwFlags & DDSD_HEIGHT) ? (int)sd->dwHeight : g_vdd.mode_h;
    int bpp = g_vdd.mode_bpp ? g_vdd.mode_bpp : 16;
    vsurf *s;
    (void)d; (void)outer;
    if (sd->dwFlags & DDSD_PIXELFORMAT) {
        if (sd->ddpfPixelFormat.dwFlags & DDPF_PALETTEINDEXED8) bpp = 8;
        else if (sd->ddpfPixelFormat.dwFlags & DDPF_RGB) bpp = sd->ddpfPixelFormat.dwRGBBitCount;
        if (bpp != 8 && bpp != 16) {
            log_msg("vdd: CreateSurface %d bpp not supported", bpp);
            return DDERR_INVALIDPIXELFORMAT;
        }
    }
    if (caps & DDSCAPS_PRIMARYSURFACE) { w = g_vdd.mode_w; h = g_vdd.mode_h; }
    if (w <= 0 || h <= 0) { log_msg("vdd: CreateSurface without a size (caps %08lX)", caps); return DDERR_INVALIDPARAMS; }
    s = vsurf_new(w, h, bpp, caps);
    if (caps & DDSCAPS_ZBUFFER) s->zdepth = (sd->dwFlags & DDSD_ZBUFFERBITDEPTH) ? sd->dwZBufferBitDepth : 16;
    if (sd->dwFlags & DDSD_CKSRCBLT) { s->ck_src = sd->ddckCKSrcBlt; s->has_ck = 1; }
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        s->is_primary = 1;
        g_vdd.primary = s;
        if ((sd->dwFlags & DDSD_BACKBUFFERCOUNT) && sd->dwBackBufferCount) {
            DWORD bc = (caps & ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE)) | DDSCAPS_BACKBUFFER;
            s->back = vsurf_new(w, h, bpp, bc);
            s->back->front = s;
            s->caps |= DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE;
        }
    }
    VTRACE("surface %dx%dx%d caps %08lX%s", w, h, bpp, caps, s->back ? " +back buffer" : "");
    *out = s;
    return DD_OK;
}

static vdd *vdd_new(void)
{
    static int init;
    vdd *d = (vdd *)calloc(1, sizeof *d);
    if (!init) {
        void **t = IDirectDraw_vt;
        init = 1;
        IDirectDraw_vt_init();
        t[IDirectDraw_QueryInterface] = (void *)vdd_QI;
        t[IDirectDraw_AddRef] = (void *)vobj_addref;
        t[IDirectDraw_Release] = (void *)vobj_release;
        t[IDirectDraw_CreateClipper] = (void *)vdd_CreateClipper;
        t[IDirectDraw_CreatePalette] = (void *)vdd_CreatePalette;
        t[IDirectDraw_CreateSurface] = (void *)vdd_CreateSurface;
        t[IDirectDraw_EnumDisplayModes] = (void *)vdd_EnumDisplayModes;
        t[IDirectDraw_FlipToGDISurface] = (void *)vdd_FlipToGDISurface;
        t[IDirectDraw_GetCaps] = (void *)vdd_GetCaps;
        t[IDirectDraw_GetDisplayMode] = (void *)vdd_GetDisplayMode;
        t[IDirectDraw_RestoreDisplayMode] = (void *)vdd_RestoreDisplayMode;
        t[IDirectDraw_SetCooperativeLevel] = (void *)vdd_SetCooperativeLevel;
        t[IDirectDraw_SetDisplayMode] = (void *)vdd_SetDisplayMode;
        t[IDirectDraw_WaitForVerticalBlank] = (void *)vdd_WaitForVerticalBlank;
        IDirect3D2_vt_init();
        IDirect3D2_vt[IDirect3D2_QueryInterface] = (void *)vd3d_QI;
        IDirect3D2_vt[IDirect3D2_AddRef] = (void *)vobj_addref;
        IDirect3D2_vt[IDirect3D2_Release] = (void *)vobj_release;
        IDirect3D2_vt[IDirect3D2_EnumDevices] = (void *)vd3d_EnumDevices;
        IDirect3D2_vt[IDirect3D2_CreateDevice] = (void *)vd3d_CreateDevice;
        g_vd3d.hd.vt = IDirect3D2_vt;
        g_vd3d.hd.ref = 1;
    }
    d->hd.vt = IDirectDraw_vt; d->hd.ref = 1; d->hd.destroy = vdd_destroy;
    return d;
}

static HRESULT WINAPI vdd_DirectDrawCreate(GUID *g, vdd **out, IUnknown *outer)
{
    (void)g; (void)outer;
    g_vdd_trace = GetEnvironmentVariableA("MMCDA_VDDTRACE", NULL, 0) != 0;
    *out = vdd_new();
    log_msg("vdd: DirectDrawCreate");
    return DD_OK;
}
