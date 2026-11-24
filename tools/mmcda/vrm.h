/* vrm.h - Direct3D Retained Mode for the new renderer ([render] backend=vulkan).
 *
 * Our own implementation of the D3DRM objects the game uses (RENDERER.md, API census):
 * IDirect3DRM/2, Device2, Viewport, Frame/Frame2, MeshBuilder2, Mesh, Texture2, Light,
 * Material, Animation/AnimationSet and the object arrays. Frames keep the full hierarchy and
 * transforms with D3DRM's conventions (row vectors, v' = v * M, translation in row 3).
 *
 * Stage 1 (this file as first written): the scene graph is complete enough for the game to
 * run; Viewport::Render draws nothing yet and MeshBuilder::Load only records the file name.
 * Stage 2 adds the .x loader and the Vulkan draw of the frame tree.
 */

static const GUID IID_D3DRM =        { 0x2bc49361, 0x8327, 0x11cf, { 0xac, 0x4a, 0x00, 0x00, 0xc0, 0x38, 0x25, 0xa1 } };
static const GUID IID_D3DRM2 =       { 0x4516ecc8, 0x8f20, 0x11d0, { 0x9b, 0x6d, 0x00, 0x00, 0xc0, 0x78, 0x1b, 0xc3 } };
static const GUID IID_D3DRMDevice =  { 0xe9e19280, 0x6e05, 0x11cf, { 0xac, 0x4a, 0x00, 0x00, 0xc0, 0x38, 0x25, 0xa1 } };
static const GUID IID_D3DRMDevice2 = { 0x4516ec78, 0x8f20, 0x11d0, { 0x9b, 0x6d, 0x00, 0x00, 0xc0, 0x78, 0x1b, 0xc3 } };
static const GUID IID_D3DRMFrame =   { 0xeb16cb03, 0xd271, 0x11ce, { 0xac, 0x48, 0x00, 0x00, 0xc0, 0x38, 0x25, 0xa1 } };
static const GUID IID_D3DRMFrame2 =  { 0xc3dfbd60, 0x3988, 0x11d0, { 0x9e, 0xc2, 0x00, 0x00, 0xc0, 0x29, 0x1a, 0xc3 } };
static const GUID IID_D3DRMMeshBuilder =  { 0xa3a80d02, 0x6e12, 0x11cf, { 0xac, 0x4a, 0x00, 0x00, 0xc0, 0x38, 0x25, 0xa1 } };
static const GUID IID_D3DRMMeshBuilder2 = { 0x4516ec77, 0x8f20, 0x11d0, { 0x9b, 0x6d, 0x00, 0x00, 0xc0, 0x78, 0x1b, 0xc3 } };
static const GUID IID_D3DRMTexture = { 0xeb16cb09, 0xd271, 0x11ce, { 0xac, 0x48, 0x00, 0x00, 0xc0, 0x38, 0x25, 0xa1 } };
static const GUID IID_D3DRMTexture2 = { 0x120f30c0, 0x1629, 0x11d0, { 0x94, 0x1c, 0x00, 0x80, 0xc8, 0x0c, 0xfa, 0x7b } };
static const GUID IID_D3DRMViewport = { 0xeb16cb02, 0xd271, 0x11ce, { 0xac, 0x48, 0x00, 0x00, 0xc0, 0x38, 0x25, 0xa1 } };
static const GUID IID_D3DRMVisual =  { 0xeb16cb04, 0xd271, 0x11ce, { 0xac, 0x48, 0x00, 0x00, 0xc0, 0x38, 0x25, 0xa1 } };
static const GUID IID_D3DRMObject =  { 0xeb16cb00, 0xd271, 0x11ce, { 0xac, 0x48, 0x00, 0x00, 0xc0, 0x38, 0x25, 0xa1 } };
static const GUID IID_D3DRMMesh =    { 0xa3a80d01, 0x6e12, 0x11cf, { 0xac, 0x4a, 0x00, 0x00, 0xc0, 0x38, 0x25, 0xa1 } };
static const GUID IID_D3DRMLight =   { 0xeb16cb08, 0xd271, 0x11ce, { 0xac, 0x48, 0x00, 0x00, 0xc0, 0x38, 0x25, 0xa1 } };
static const GUID IID_D3DRMAnimationSet = { 0xeb16cb0e, 0xd271, 0x11ce, { 0xac, 0x48, 0x00, 0x00, 0xc0, 0x38, 0x25, 0xa1 } };

static int g_vrm_trace = -1;
static const char *vrm_mb_file(void *mb);
static const char *vrm_tex_info(void *tx);
#define RTRACE(...) do { if (g_vrm_trace < 0) g_vrm_trace = GetEnvironmentVariableA("MMCDA_VRMTRACE", NULL, 0) != 0;                          if (g_vrm_trace) log_msg("vrm: " __VA_ARGS__); } while (0)

enum { RK_RM, RK_DEVICE, RK_VIEWPORT, RK_FRAME, RK_MESHBUILDER, RK_MESH, RK_TEXTURE, RK_LIGHT,
       RK_MATERIAL, RK_ANIMATION, RK_ANIMSET, RK_ARRAY };
static const char *const k_rk_class[] = { "Direct3DRM", "Device", "Viewport", "Frame", "Builder", "Mesh",
                                          "Texture", "Light", "Material", "Animation", "AnimationSet", "Array" };

typedef void (__cdecl *RMOBJCB)(void *obj, void *arg);
typedef struct vrmobj {
    void **vt;
    LONG ref;
    int kind;
    DWORD appdata;
    char name[64];
    struct vrmdcb { RMOBJCB cb; void *arg; } *dcb;   /* destroy callbacks (any number) */
    int ndcb, cdcb;
    void (*destroy)(struct vrmobj *);
} vrmobj;

static LONG g_vrm_live;   /* live object count (log only) */

static void vrm_init(vrmobj *o, void **vt, int kind, void (*destroy)(vrmobj *))
{
    o->vt = vt; o->ref = 1; o->kind = kind; o->destroy = destroy;
    InterlockedIncrement(&g_vrm_live);
}

static ULONG WINAPI vrm_AddRef(vrmobj *o) { return InterlockedIncrement(&o->ref); }
static ULONG WINAPI vrm_Release(vrmobj *o)
{
    LONG r = InterlockedDecrement(&o->ref);
    if (r == 0) {
        int i;
        for (i = 0; i < o->ndcb; i++) o->dcb[i].cb(o, o->dcb[i].arg);
        free(o->dcb);
        InterlockedDecrement(&g_vrm_live);
        if (o->destroy) o->destroy(o);
        free(o);
    }
    return r < 0 ? 0 : r;
}
#define VRM_ADDREF(o) do { if (o) vrm_AddRef((vrmobj *)(o)); } while (0)
#define VRM_RELEASE(o) do { if (o) vrm_Release((vrmobj *)(o)); } while (0)

static HRESULT WINAPI vrm_QI(vrmobj *o, REFIID iid, void **out)
{
    int ok = IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_D3DRMObject);
    switch (o->kind) {
    case RK_RM: ok = IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_D3DRM) || IsEqualGUID(iid, &IID_D3DRM2); break;
    case RK_DEVICE: ok |= IsEqualGUID(iid, &IID_D3DRMDevice) || IsEqualGUID(iid, &IID_D3DRMDevice2); break;
    case RK_VIEWPORT: ok |= IsEqualGUID(iid, &IID_D3DRMViewport); break;
    case RK_FRAME: ok |= IsEqualGUID(iid, &IID_D3DRMFrame) || IsEqualGUID(iid, &IID_D3DRMFrame2) || IsEqualGUID(iid, &IID_D3DRMVisual); break;
    case RK_MESHBUILDER: ok |= IsEqualGUID(iid, &IID_D3DRMMeshBuilder) || IsEqualGUID(iid, &IID_D3DRMMeshBuilder2) || IsEqualGUID(iid, &IID_D3DRMVisual); break;
    case RK_MESH: ok |= IsEqualGUID(iid, &IID_D3DRMMesh) || IsEqualGUID(iid, &IID_D3DRMVisual); break;
    case RK_TEXTURE: ok |= IsEqualGUID(iid, &IID_D3DRMTexture) || IsEqualGUID(iid, &IID_D3DRMTexture2) || IsEqualGUID(iid, &IID_D3DRMVisual); break;
    case RK_LIGHT: ok |= IsEqualGUID(iid, &IID_D3DRMLight); break;
    case RK_ANIMSET: ok |= IsEqualGUID(iid, &IID_D3DRMAnimationSet); break;
    }
    if (!out) return E_POINTER;
    if (!ok) {
        *out = NULL;
        log_msg("vrm: %s QueryInterface for unsupported %08lX-%04X", k_rk_class[o->kind], iid->Data1, iid->Data2);
        return E_NOINTERFACE;
    }
    *out = o;
    vrm_AddRef(o);
    return S_OK;
}
static HRESULT WINAPI vrm_Clone(vrmobj *o, IUnknown *outer, REFIID iid, void **out) { (void)o; (void)outer; (void)iid; *out = NULL; return E_NOTIMPL; }
static HRESULT WINAPI vrm_AddDestroyCallback(vrmobj *o, RMOBJCB cb, void *arg)
{
    if (!cb) return E_FAIL;
    if (o->ndcb == o->cdcb) { o->cdcb = o->cdcb ? o->cdcb * 2 : 4; o->dcb = (struct vrmdcb *)realloc(o->dcb, o->cdcb * sizeof *o->dcb); }
    o->dcb[o->ndcb].cb = cb; o->dcb[o->ndcb].arg = arg; o->ndcb++;
    return S_OK;
}
static HRESULT WINAPI vrm_DeleteDestroyCallback(vrmobj *o, RMOBJCB cb, void *arg)
{
    int i;
    for (i = 0; i < o->ndcb; i++)
        if (o->dcb[i].cb == cb && o->dcb[i].arg == arg) { o->dcb[i] = o->dcb[--o->ndcb]; return S_OK; }
    return S_OK;
}
static HRESULT WINAPI vrm_SetAppData(vrmobj *o, DWORD d) { o->appdata = d; return S_OK; }
static DWORD WINAPI vrm_GetAppData(vrmobj *o) { return o->appdata; }
static HRESULT WINAPI vrm_SetName(vrmobj *o, const char *n) { lstrcpynA(o->name, n ? n : "", sizeof o->name); return S_OK; }
static HRESULT WINAPI vrm_copystr(const char *s, DWORD *size, char *out)
{
    DWORD n = (DWORD)strlen(s) + 1;
    if (!size) return E_POINTER;
    if (out) { if (*size < n) return E_INVALIDARG; memcpy(out, s, n); }
    *size = n;
    return S_OK;
}
static HRESULT WINAPI vrm_GetName(vrmobj *o, DWORD *size, char *out) { return vrm_copystr(o->name, size, out); }
static HRESULT WINAPI vrm_GetClassName(vrmobj *o, DWORD *size, char *out) { return vrm_copystr(k_rk_class[o->kind], size, out); }

/* fill the IUnknown + IDirect3DRMObject block (slots 0-10) */
static void vrm_obj_slots(void **vt)
{
    vt[0] = (void *)vrm_QI; vt[1] = (void *)vrm_AddRef; vt[2] = (void *)vrm_Release;
    vt[3] = (void *)vrm_Clone; vt[4] = (void *)vrm_AddDestroyCallback; vt[5] = (void *)vrm_DeleteDestroyCallback;
    vt[6] = (void *)vrm_SetAppData; vt[7] = (void *)vrm_GetAppData; vt[8] = (void *)vrm_SetName;
    vt[9] = (void *)vrm_GetName; vt[10] = (void *)vrm_GetClassName;
}

/* ---- matrices (D3DRM: row vectors, v' = v * M) ---- */
typedef float MAT[4][4];
typedef struct { float x, y, z, w; } D3DRMVECTOR4D;
static void m_ident(MAT m) { memset(m, 0, sizeof(MAT)); m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1; }
static void m_mul(MAT out, MAT a, MAT b)   /* out = a * b */
{
    MAT r;
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
    memcpy(out, r, sizeof r);
}
static void m_rot(MAT m, float x, float y, float z, float t)   /* rotation by t about axis (x,y,z) */
{
    float l = sqrtf(x * x + y * y + z * z), c = cosf(t), s = sinf(t), k;
    m_ident(m);
    if (l < 1e-9f) return;
    x /= l; y /= l; z /= l; k = 1 - c;
    /* row-vector form (transpose of the column-vector Rodrigues matrix), as D3DRMMatrixFromQuaternion */
    m[0][0] = c + x * x * k;     m[0][1] = x * y * k + z * s; m[0][2] = x * z * k - y * s;
    m[1][0] = y * x * k - z * s; m[1][1] = c + y * y * k;     m[1][2] = y * z * k + x * s;
    m[2][0] = z * x * k + y * s; m[2][1] = z * y * k - x * s; m[2][2] = c + z * z * k;
}
static int m_inv(MAT out, MAT m)   /* general 4x4 inverse */
{
    float inv[16], *a = &m[0][0], det;
    int i;
    inv[0] = a[5]*a[10]*a[15]-a[5]*a[11]*a[14]-a[9]*a[6]*a[15]+a[9]*a[7]*a[14]+a[13]*a[6]*a[11]-a[13]*a[7]*a[10];
    inv[4] = -a[4]*a[10]*a[15]+a[4]*a[11]*a[14]+a[8]*a[6]*a[15]-a[8]*a[7]*a[14]-a[12]*a[6]*a[11]+a[12]*a[7]*a[10];
    inv[8] = a[4]*a[9]*a[15]-a[4]*a[11]*a[13]-a[8]*a[5]*a[15]+a[8]*a[7]*a[13]+a[12]*a[5]*a[11]-a[12]*a[7]*a[9];
    inv[12] = -a[4]*a[9]*a[14]+a[4]*a[10]*a[13]+a[8]*a[5]*a[14]-a[8]*a[6]*a[13]-a[12]*a[5]*a[10]+a[12]*a[6]*a[9];
    inv[1] = -a[1]*a[10]*a[15]+a[1]*a[11]*a[14]+a[9]*a[2]*a[15]-a[9]*a[3]*a[14]-a[13]*a[2]*a[11]+a[13]*a[3]*a[10];
    inv[5] = a[0]*a[10]*a[15]-a[0]*a[11]*a[14]-a[8]*a[2]*a[15]+a[8]*a[3]*a[14]+a[12]*a[2]*a[11]-a[12]*a[3]*a[10];
    inv[9] = -a[0]*a[9]*a[15]+a[0]*a[11]*a[13]+a[8]*a[1]*a[15]-a[8]*a[3]*a[13]-a[12]*a[1]*a[11]+a[12]*a[3]*a[9];
    inv[13] = a[0]*a[9]*a[14]-a[0]*a[10]*a[13]-a[8]*a[1]*a[14]+a[8]*a[2]*a[13]+a[12]*a[1]*a[10]-a[12]*a[2]*a[9];
    inv[2] = a[1]*a[6]*a[15]-a[1]*a[7]*a[14]-a[5]*a[2]*a[15]+a[5]*a[3]*a[14]+a[13]*a[2]*a[7]-a[13]*a[3]*a[6];
    inv[6] = -a[0]*a[6]*a[15]+a[0]*a[7]*a[14]+a[4]*a[2]*a[15]-a[4]*a[3]*a[14]-a[12]*a[2]*a[7]+a[12]*a[3]*a[6];
    inv[10] = a[0]*a[5]*a[15]-a[0]*a[7]*a[13]-a[4]*a[1]*a[15]+a[4]*a[3]*a[13]+a[12]*a[1]*a[7]-a[12]*a[3]*a[5];
    inv[14] = -a[0]*a[5]*a[14]+a[0]*a[6]*a[13]+a[4]*a[1]*a[14]-a[4]*a[2]*a[13]-a[12]*a[1]*a[6]+a[12]*a[2]*a[5];
    inv[3] = -a[1]*a[6]*a[11]+a[1]*a[7]*a[10]+a[5]*a[2]*a[11]-a[5]*a[3]*a[10]-a[9]*a[2]*a[7]+a[9]*a[3]*a[6];
    inv[7] = a[0]*a[6]*a[11]-a[0]*a[7]*a[10]-a[4]*a[2]*a[11]+a[4]*a[3]*a[10]+a[8]*a[2]*a[7]-a[8]*a[3]*a[6];
    inv[11] = -a[0]*a[5]*a[11]+a[0]*a[7]*a[9]+a[4]*a[1]*a[11]-a[4]*a[3]*a[9]-a[8]*a[1]*a[7]+a[8]*a[3]*a[5];
    inv[15] = a[0]*a[5]*a[10]-a[0]*a[6]*a[9]-a[4]*a[1]*a[10]+a[4]*a[2]*a[9]+a[8]*a[1]*a[6]-a[8]*a[2]*a[5];
    det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (fabsf(det) < 1e-20f) { m_ident(out); return 0; }
    for (i = 0; i < 16; i++) (&out[0][0])[i] = inv[i] / det;
    return 1;
}
static void m_xform(float out[3], const float v[3], MAT m)   /* point: (v,1) * m */
{
    float r[3];
    int j;
    for (j = 0; j < 3; j++) r[j] = v[0] * m[0][j] + v[1] * m[1][j] + v[2] * m[2][j] + m[3][j];
    memcpy(out, r, sizeof r);
}

/* ---- arrays ---- */
typedef struct vrmarray { vrmobj o; int n; vrmobj **e; } vrmarray;
static void vrmarray_destroy(vrmobj *o)
{
    vrmarray *a = (vrmarray *)o;
    int i;
    for (i = 0; i < a->n; i++) VRM_RELEASE(a->e[i]);
    free(a->e);
}
static DWORD WINAPI vrmarray_GetSize(vrmarray *a) { return a->n; }
static HRESULT WINAPI vrmarray_GetElement(vrmarray *a, DWORD i, vrmobj **out)
{
    if (!out) return E_POINTER;
    if (i >= (DWORD)a->n) { *out = NULL; return E_INVALIDARG; }
    *out = a->e[i];
    VRM_ADDREF(*out);
    return S_OK;
}
static vrmarray *vrmarray_new(vrmobj **items, int n)
{
    static int init;
    vrmarray *a = (vrmarray *)calloc(1, sizeof *a);
    int i;
    if (!init) {
        init = 1;
        IDirect3DRMArray_vt_init();
        IDirect3DRMArray_vt[0] = (void *)vrm_QI;
        IDirect3DRMArray_vt[1] = (void *)vrm_AddRef;
        IDirect3DRMArray_vt[2] = (void *)vrm_Release;
        IDirect3DRMArray_vt[3] = (void *)vrmarray_GetSize;
        IDirect3DRMArray_vt[4] = (void *)vrmarray_GetElement;
    }
    vrm_init(&a->o, IDirect3DRMArray_vt, RK_ARRAY, vrmarray_destroy);
    a->n = n;
    a->e = (vrmobj **)calloc(n ? n : 1, sizeof *a->e);
    for (i = 0; i < n; i++) { a->e[i] = items[i]; VRM_ADDREF(items[i]); }
    return a;
}

/* simple pointer list */
typedef struct { vrmobj **p; int n, cap; } vlist;
static void vlist_add(vlist *l, vrmobj *o)
{
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 8; l->p = (vrmobj **)realloc(l->p, l->cap * sizeof *l->p); }
    l->p[l->n++] = o;
}
static int vlist_remove(vlist *l, vrmobj *o)
{
    int i;
    for (i = 0; i < l->n; i++)
        if (l->p[i] == o) { memmove(l->p + i, l->p + i + 1, (l->n - i - 1) * sizeof *l->p); l->n--; return 1; }
    return 0;
}
static int vlist_has(vlist *l, vrmobj *o) { int i; for (i = 0; i < l->n; i++) if (l->p[i] == o) return 1; return 0; }

/* ---- frame ---- */
typedef void (__cdecl *RMMOVECB)(void *frame, void *ctx, float delta);
typedef struct vframe {
    vrmobj o;
    struct vframe *parent;
    vlist kids, visuals, lights;
    MAT m;                                  /* local transform */
    struct vrmmcb { RMMOVECB cb; void *ctx; } *mcb;   /* move callbacks (any number) */
    int nmcb, cmcb;
    float rot_axis[3], rot_theta;           /* SetRotation: applied per Move */
    float vel[3];
    DWORD bg, color, fog_color;
    int fog_on, fog_mode, matmode, zmode, sortmode, wrap_u, wrap_v;
    float fog_start, fog_end, fog_density;
    vrmobj *texture, *bg_image;
} vframe;

static void vframe_destroy(vrmobj *o)
{
    vframe *f = (vframe *)o;
    int i;
    for (i = 0; i < f->kids.n; i++) { ((vframe *)f->kids.p[i])->parent = NULL; VRM_RELEASE(f->kids.p[i]); }
    for (i = 0; i < f->visuals.n; i++) VRM_RELEASE(f->visuals.p[i]);
    for (i = 0; i < f->lights.n; i++) VRM_RELEASE(f->lights.p[i]);
    free(f->kids.p); free(f->visuals.p); free(f->lights.p); free(f->mcb);
    VRM_RELEASE(f->texture); VRM_RELEASE(f->bg_image);
}

static void vframe_nancheck(vframe *f, const char *op, float a, float b, float c, float d)
{
    static int logged;
    int i;
    for (i = 0; i < 16; i++) {
        float v = (&f->m[0][0])[i];
        if (v != v || v > 1e30f || v < -1e30f) {
            if (logged++ < 8) log_msg("vrm: NaN/inf in frame %p after %s(%g, %g, %g, %g) m %g %g %g / %g %g %g", f, op, a, b, c, d,
                                      f->m[0][0], f->m[0][1], f->m[0][2], f->m[3][0], f->m[3][1], f->m[3][2]);
            return;
        }
    }
}

static void vframe_world(vframe *f, MAT out)
{
    m_ident(out);
    for (; f; f = f->parent) m_mul(out, out, f->m);
}
/* matrix taking coordinates in 'ref' space to f's parent space (NULL ref = parent) */
static void vframe_ref_to_parent(vframe *f, vframe *ref, MAT out)
{
    MAT rw, pw, ipw;
    if (!ref || ref == f->parent) { m_ident(out); return; }
    vframe_world(ref, rw);
    vframe_world(f->parent, pw);
    m_inv(ipw, pw);
    m_mul(out, rw, ipw);
}
static void vframe_combine(vframe *f, int type, MAT t)
{
    if (type == 0) memcpy(f->m, t, sizeof(MAT));
    else if (type == 1) m_mul(f->m, t, f->m);      /* BEFORE */
    else m_mul(f->m, f->m, t);                      /* AFTER */
}

static HRESULT WINAPI vframe_AddChild(vframe *f, vframe *c)
{
    RTRACE("AddChild %p <- %p", f, c);
    if (!c) return E_POINTER;
    if (c->parent == f) return S_OK;
    VRM_ADDREF(c);
    if (c->parent) { vlist_remove(&c->parent->kids, &c->o); VRM_RELEASE(c); }
    c->parent = f;
    vlist_add(&f->kids, &c->o);
    return S_OK;
}
static HRESULT WINAPI vframe_DeleteChild(vframe *f, vframe *c)
{
    if (!c || !vlist_remove(&f->kids, &c->o)) return E_INVALIDARG;
    c->parent = NULL;
    VRM_RELEASE(c);
    return S_OK;
}
static HRESULT WINAPI vframe_AddLight(vframe *f, vrmobj *l) { if (!l) return E_POINTER; VRM_ADDREF(l); vlist_add(&f->lights, l); return S_OK; }
static HRESULT WINAPI vframe_DeleteLight(vframe *f, vrmobj *l) { if (vlist_remove(&f->lights, l)) VRM_RELEASE(l); return S_OK; }
static HRESULT WINAPI vframe_AddVisual(vframe *f, vrmobj *v)
{
    if (!v) return E_POINTER;
    RTRACE("AddVisual frame %p (parent %p) <- %s %p %s", f, f->parent, k_rk_class[v->kind], v,
           v->kind == RK_MESHBUILDER ? vrm_mb_file(v) : v->kind == RK_TEXTURE ? vrm_tex_info(v) : "");
    if (vlist_has(&f->visuals, v)) return S_OK;
    VRM_ADDREF(v); vlist_add(&f->visuals, v);
    return S_OK;
}
static HRESULT WINAPI vframe_DeleteVisual(vframe *f, vrmobj *v) { if (vlist_remove(&f->visuals, v)) VRM_RELEASE(v); return S_OK; }
static HRESULT WINAPI vframe_AddMoveCallback(vframe *f, RMMOVECB cb, void *ctx)
{
    if (!cb) return E_FAIL;
    if (f->nmcb == f->cmcb) { f->cmcb = f->cmcb ? f->cmcb * 2 : 4; f->mcb = (struct vrmmcb *)realloc(f->mcb, f->cmcb * sizeof *f->mcb); }
    f->mcb[f->nmcb].cb = cb; f->mcb[f->nmcb].ctx = ctx; f->nmcb++;
    return S_OK;
}
static HRESULT WINAPI vframe_AddMoveCallback2(vframe *f, RMMOVECB cb, void *ctx, DWORD fl) { (void)fl; return vframe_AddMoveCallback(f, cb, ctx); }
static HRESULT WINAPI vframe_DeleteMoveCallback(vframe *f, RMMOVECB cb, void *ctx)
{
    int i;
    for (i = 0; i < f->nmcb; i++)
        if (f->mcb[i].cb == cb && f->mcb[i].ctx == ctx) {
            memmove(f->mcb + i, f->mcb + i + 1, (f->nmcb - i - 1) * sizeof f->mcb[0]);
            f->nmcb--;
            break;
        }
    return S_OK;
}
static HRESULT WINAPI vframe_AddTransform(vframe *f, int type, MAT t)
{
    vframe_combine(f, type, t);
    vframe_nancheck(f, "AddTransform", (float)type, t[0][0], t[3][0], t[3][1]);
    if (f->m[0][0] != f->m[0][0]) {
        static int n;
        HMODULE m = NULL;
        DWORD ra = (DWORD)_ReturnAddress();
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ra, &m);
        if (n++ < 4) log_msg("vrm: NaN AddTransform on %p from %s+%lX (module base %p)", f, m == GetModuleHandleA(NULL) ? "exe" : "dll", ra - (DWORD)m, m);
    }
    return S_OK;
}
static HRESULT WINAPI vframe_AddTranslation(vframe *f, int type, float x, float y, float z)
{
    MAT t; m_ident(t); t[3][0] = x; t[3][1] = y; t[3][2] = z;
    vframe_combine(f, type, t);
    vframe_nancheck(f, "AddTranslation", x, y, z, (float)type);
    return S_OK;
}
static HRESULT WINAPI vframe_AddScale(vframe *f, int type, float x, float y, float z)
{
    MAT t; m_ident(t); t[0][0] = x; t[1][1] = y; t[2][2] = z;
    vframe_combine(f, type, t);
    vframe_nancheck(f, "AddScale", x, y, z, (float)type);
    return S_OK;
}
static HRESULT WINAPI vframe_AddRotation(vframe *f, int type, float x, float y, float z, float th)
{
    MAT t; m_rot(t, x, y, z, th);
    vframe_combine(f, type, t);
    vframe_nancheck(f, "AddRotation", x, y, z, th);
    return S_OK;
}
static HRESULT WINAPI vframe_GetTransform(vframe *f, MAT out) { memcpy(out, f->m, sizeof(MAT)); return S_OK; }
static HRESULT WINAPI vframe_SetPosition(vframe *f, vframe *ref, float x, float y, float z)
{
    MAT r;
    float p[3] = { x, y, z };
    vframe_ref_to_parent(f, ref, r);
    m_xform(p, p, r);
    f->m[3][0] = p[0]; f->m[3][1] = p[1]; f->m[3][2] = p[2];
    vframe_nancheck(f, "SetPosition", x, y, z, 0);
    return S_OK;
}
static HRESULT WINAPI vframe_GetPosition(vframe *f, vframe *ref, D3DVECTOR *out)
{
    MAT r, ir;
    float p[3] = { f->m[3][0], f->m[3][1], f->m[3][2] };
    vframe_ref_to_parent(f, ref, r);
    m_inv(ir, r);
    m_xform(p, p, ir);
    out->x = p[0]; out->y = p[1]; out->z = p[2];
    return S_OK;
}
static void v3_norm(float v[3]) { float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]); if (l > 1e-12f) { v[0] /= l; v[1] /= l; v[2] /= l; } }
static void v3_cross(float o[3], const float a[3], const float b[3])
{
    float r[3] = { a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0] };
    memcpy(o, r, sizeof r);
}
static HRESULT WINAPI vframe_SetOrientation(vframe *f, vframe *ref, float dx, float dy, float dz, float ux, float uy, float uz)
{
    float z[3] = { dx, dy, dz }, y[3] = { ux, uy, uz }, x[3];
    MAT rot, r;
    int i;
    v3_norm(z);
    if (z[0] * z[0] + z[1] * z[1] + z[2] * z[2] < 1e-12f) { z[0] = 0; z[1] = 0; z[2] = 1; }
    v3_cross(x, y, z);
    if (x[0] * x[0] + x[1] * x[1] + x[2] * x[2] < 1e-12f) {
        /* up is parallel to the direction (or zero): use the axis least aligned with it,
           as D3DRM still produces a valid orientation (dust puffs face straight up) */
        float alt[3] = { 0, 0, 0 };
        if (fabsf(z[0]) <= fabsf(z[1]) && fabsf(z[0]) <= fabsf(z[2])) alt[0] = 1;
        else if (fabsf(z[1]) <= fabsf(z[2])) alt[1] = 1;
        else alt[2] = 1;
        v3_cross(x, alt, z);
    }
    v3_norm(x);
    v3_cross(y, z, x);
    m_ident(rot);
    for (i = 0; i < 3; i++) { rot[0][i] = x[i]; rot[1][i] = y[i]; rot[2][i] = z[i]; }
    vframe_ref_to_parent(f, ref, r);
    r[3][0] = r[3][1] = r[3][2] = 0;                 /* directions: rotation part only */
    m_mul(rot, rot, r);
    for (i = 0; i < 3; i++) { f->m[0][i] = rot[0][i]; f->m[1][i] = rot[1][i]; f->m[2][i] = rot[2][i]; }
    vframe_nancheck(f, "SetOrientation", dx, dy, dz, uy);
    return S_OK;
}
static HRESULT WINAPI vframe_GetOrientation(vframe *f, vframe *ref, D3DVECTOR *dir, D3DVECTOR *up)
{
    MAT r, ir, rot;
    float d[3], u[3], zero[3] = { 0, 0, 0 };
    vframe_ref_to_parent(f, ref, r);
    m_inv(ir, r);
    memcpy(rot, f->m, sizeof rot);
    rot[3][0] = rot[3][1] = rot[3][2] = 0;
    m_mul(rot, rot, ir);
    m_xform(zero, zero, ir);
    d[0] = rot[2][0]; d[1] = rot[2][1]; d[2] = rot[2][2];
    u[0] = rot[1][0]; u[1] = rot[1][1]; u[2] = rot[1][2];
    v3_norm(d); v3_norm(u);
    if (dir) { dir->x = d[0]; dir->y = d[1]; dir->z = d[2]; }
    if (up) { up->x = u[0]; up->y = u[1]; up->z = u[2]; }
    return S_OK;
}
static HRESULT WINAPI vframe_SetRotation(vframe *f, vframe *ref, float x, float y, float z, float th)
{
    (void)ref;
    f->rot_axis[0] = x; f->rot_axis[1] = y; f->rot_axis[2] = z; f->rot_theta = th;
    vframe_nancheck(f, "SetRotation", x, y, z, th);
    return S_OK;
}
static HRESULT WINAPI vframe_GetRotation(vframe *f, vframe *ref, D3DVECTOR *axis, float *th)
{
    (void)ref;
    axis->x = f->rot_axis[0]; axis->y = f->rot_axis[1]; axis->z = f->rot_axis[2]; *th = f->rot_theta;
    return S_OK;
}
static HRESULT WINAPI vframe_SetVelocity(vframe *f, vframe *ref, float x, float y, float z, BOOL wr)
{
    (void)ref; (void)wr;
    f->vel[0] = x; f->vel[1] = y; f->vel[2] = z;
    vframe_nancheck(f, "SetVelocity", x, y, z, 0);
    return S_OK;
}
static HRESULT WINAPI vframe_GetVelocity(vframe *f, vframe *ref, D3DVECTOR *v, BOOL wr)
{
    (void)ref; (void)wr;
    v->x = f->vel[0]; v->y = f->vel[1]; v->z = f->vel[2];
    return S_OK;
}
static void vframe_move(vframe *f, float delta)
{
    int i, n;
    vframe **kids;
    if (f->nmcb) {   /* callbacks may add or remove callbacks: run a snapshot */
        int n = f->nmcb;
        struct vrmmcb *snap = (struct vrmmcb *)_alloca(n * sizeof *snap);
        memcpy(snap, f->mcb, n * sizeof *snap);
        for (i = 0; i < n; i++) snap[i].cb(f, snap[i].ctx, delta);
        vframe_nancheck(f, "MoveCallback", delta, 0, 0, 0);
    }
    if (f->rot_theta != 0) {
        MAT r;
        m_rot(r, f->rot_axis[0], f->rot_axis[1], f->rot_axis[2], f->rot_theta * delta);
        {   /* rotate about the frame's own origin */
            float t[3] = { f->m[3][0], f->m[3][1], f->m[3][2] };
            f->m[3][0] = f->m[3][1] = f->m[3][2] = 0;
            m_mul(f->m, r, f->m);
            f->m[3][0] = t[0]; f->m[3][1] = t[1]; f->m[3][2] = t[2];
        }
    }
    for (i = 0; i < 3; i++) f->m[3][i] += f->vel[i] * delta;
    vframe_nancheck(f, "Move", delta, f->rot_theta, f->vel[0], f->rot_axis[1]);
    /* callbacks may change the child list: walk a snapshot */
    n = f->kids.n;
    if (!n) return;
    kids = (vframe **)_alloca(n * sizeof *kids);
    memcpy(kids, f->kids.p, n * sizeof *kids);
    for (i = 0; i < n; i++) { VRM_ADDREF(kids[i]); }
    for (i = 0; i < n; i++) { vframe_move(kids[i], delta); VRM_RELEASE(kids[i]); }
}
static HRESULT WINAPI vframe_Move(vframe *f, float delta) { vframe_move(f, delta); return S_OK; }
static HRESULT WINAPI vframe_GetParent(vframe *f, vframe **out) { *out = f->parent; VRM_ADDREF(f->parent); return S_OK; }
static HRESULT WINAPI vframe_GetScene(vframe *f, vframe **out)
{
    while (f->parent) f = f->parent;
    *out = f; VRM_ADDREF(f);
    return S_OK;
}
static HRESULT WINAPI vframe_GetChildren(vframe *f, vrmarray **out) { *out = vrmarray_new(f->kids.p, f->kids.n); return S_OK; }
static HRESULT WINAPI vframe_GetVisuals(vframe *f, vrmarray **out) { *out = vrmarray_new(f->visuals.p, f->visuals.n); return S_OK; }
static HRESULT WINAPI vframe_GetLights(vframe *f, vrmarray **out) { *out = vrmarray_new(f->lights.p, f->lights.n); return S_OK; }
static HRESULT WINAPI vframe_Transform(vframe *f, D3DVECTOR *d, D3DVECTOR *s)
{
    MAT w; float p[3] = { s->x, s->y, s->z };
    vframe_world(f, w); m_xform(p, p, w);
    d->x = p[0]; d->y = p[1]; d->z = p[2];
    return S_OK;
}
static HRESULT WINAPI vframe_InverseTransform(vframe *f, D3DVECTOR *d, D3DVECTOR *s)
{
    MAT w, iw; float p[3] = { s->x, s->y, s->z };
    vframe_world(f, w); m_inv(iw, w); m_xform(p, p, iw);
    d->x = p[0]; d->y = p[1]; d->z = p[2];
    return S_OK;
}
static HRESULT WINAPI vframe_LookAt(vframe *f, vframe *target, vframe *ref, int constraint)
{
    MAT pw, ipw, tw;
    float t[3] = { 0, 0, 0 }, d[3];
    (void)constraint;
    if (!target) return E_POINTER;
    vframe_world(target, tw); m_xform(t, t, tw);
    vframe_world(f->parent, pw); m_inv(ipw, pw); m_xform(t, t, ipw);   /* target in parent space */
    d[0] = t[0] - f->m[3][0]; d[1] = t[1] - f->m[3][1]; d[2] = t[2] - f->m[3][2];
    (void)ref;
    vframe_nancheck(f, "LookAt-pre", d[0], d[1], d[2], 0);
    return vframe_SetOrientation(f, NULL, d[0], d[1], d[2], 0, 1, 0);
}
static DWORD WINAPI vframe_GetSceneBackground(vframe *f) { return f->bg; }
static HRESULT WINAPI vframe_SetSceneBackground(vframe *f, DWORD c) { f->bg = c; return S_OK; }
static DWORD vrm_rgb(float r, float g, float b)
{
    int ir = (int)(r * 255 + 0.5f), ig = (int)(g * 255 + 0.5f), ib = (int)(b * 255 + 0.5f);
    ir = ir < 0 ? 0 : ir > 255 ? 255 : ir; ig = ig < 0 ? 0 : ig > 255 ? 255 : ig; ib = ib < 0 ? 0 : ib > 255 ? 255 : ib;
    return 0xFF000000u | (ir << 16) | (ig << 8) | ib;
}
static HRESULT WINAPI vframe_SetSceneBackgroundRGB(vframe *f, float r, float g, float b) { f->bg = vrm_rgb(r, g, b); return S_OK; }
static HRESULT WINAPI vframe_SetSceneBackgroundDepth(vframe *f, void *s) { (void)f; (void)s; return S_OK; }
static HRESULT WINAPI vframe_SetSceneBackgroundImage(vframe *f, vrmobj *t) { VRM_ADDREF(t); VRM_RELEASE(f->bg_image); f->bg_image = t; return S_OK; }
static HRESULT WINAPI vframe_SetSceneFogEnable(vframe *f, BOOL on)
{
    if (on != f->fog_on) log_msg("vrm: scene fog %s (mode %d, %.1f..%.1f, density %.3f, colour %08lX)", on ? "on" : "off",
                                 f->fog_mode, f->fog_start, f->fog_end, f->fog_density, f->fog_color);
    f->fog_on = on; return S_OK;
}
static BOOL WINAPI vframe_GetSceneFogEnable(vframe *f) { return f->fog_on; }
static HRESULT WINAPI vframe_SetSceneFogColor(vframe *f, DWORD c) { f->fog_color = c; return S_OK; }
static DWORD WINAPI vframe_GetSceneFogColor(vframe *f) { return f->fog_color; }
static HRESULT WINAPI vframe_SetSceneFogMode(vframe *f, int m) { f->fog_mode = m; return S_OK; }
static DWORD WINAPI vframe_GetSceneFogMode(vframe *f) { return f->fog_mode; }
static HRESULT WINAPI vframe_SetSceneFogParams(vframe *f, float s, float e, float d) { f->fog_start = s; f->fog_end = e; f->fog_density = d; return S_OK; }
static HRESULT WINAPI vframe_GetSceneFogParams(vframe *f, float *s, float *e, float *d) { *s = f->fog_start; *e = f->fog_end; *d = f->fog_density; return S_OK; }
static HRESULT WINAPI vframe_SetColor(vframe *f, DWORD c) { f->color = c; return S_OK; }
static DWORD WINAPI vframe_GetColor(vframe *f) { return f->color; }
static HRESULT WINAPI vframe_SetColorRGB(vframe *f, float r, float g, float b) { f->color = vrm_rgb(r, g, b); return S_OK; }
static HRESULT WINAPI vframe_SetMaterialMode(vframe *f, int m) { f->matmode = m; return S_OK; }
static DWORD WINAPI vframe_GetMaterialMode(vframe *f) { return f->matmode; }
static HRESULT WINAPI vframe_SetZbufferMode(vframe *f, int m) { f->zmode = m; return S_OK; }
static DWORD WINAPI vframe_GetZbufferMode(vframe *f) { return f->zmode; }
static HRESULT WINAPI vframe_SetSortMode(vframe *f, int m) { f->sortmode = m; return S_OK; }
static DWORD WINAPI vframe_GetSortMode(vframe *f) { return f->sortmode; }
static HRESULT WINAPI vframe_SetTexture(vframe *f, vrmobj *t) { VRM_ADDREF(t); VRM_RELEASE(f->texture); f->texture = t; return S_OK; }
static HRESULT WINAPI vframe_GetTexture(vframe *f, vrmobj **t) { *t = f->texture; VRM_ADDREF(f->texture); return S_OK; }
static HRESULT WINAPI vframe_SetTextureTopology(vframe *f, BOOL u, BOOL v) { f->wrap_u = u; f->wrap_v = v; return S_OK; }
static HRESULT WINAPI vframe_GetTextureTopology(vframe *f, BOOL *u, BOOL *v) { *u = f->wrap_u; *v = f->wrap_v; return S_OK; }

static void **vframe_vt(void)
{
    static int init;
    void **t = IDirect3DRMFrame2_vt;
    if (!init) {
        init = 1;
        IDirect3DRMFrame2_vt_init();
        vrm_obj_slots(t);
#define S(n, f) t[IDirect3DRMFrame2_##n] = (void *)f
        S(AddChild, vframe_AddChild); S(AddLight, vframe_AddLight); S(AddMoveCallback, vframe_AddMoveCallback);
        S(AddTransform, vframe_AddTransform); S(AddTranslation, vframe_AddTranslation); S(AddScale, vframe_AddScale);
        S(AddRotation, vframe_AddRotation); S(AddVisual, vframe_AddVisual); S(GetChildren, vframe_GetChildren);
        S(GetColor, vframe_GetColor); S(GetLights, vframe_GetLights); S(GetMaterialMode, vframe_GetMaterialMode);
        S(GetParent, vframe_GetParent); S(GetPosition, vframe_GetPosition); S(GetRotation, vframe_GetRotation);
        S(GetScene, vframe_GetScene); S(GetSortMode, vframe_GetSortMode); S(GetTexture, vframe_GetTexture);
        S(GetTransform, vframe_GetTransform); S(GetVelocity, vframe_GetVelocity); S(GetOrientation, vframe_GetOrientation);
        S(GetVisuals, vframe_GetVisuals); S(GetTextureTopology, vframe_GetTextureTopology);
        S(InverseTransform, vframe_InverseTransform); S(LookAt, vframe_LookAt); S(Move, vframe_Move);
        S(DeleteChild, vframe_DeleteChild); S(DeleteLight, vframe_DeleteLight); S(DeleteMoveCallback, vframe_DeleteMoveCallback);
        S(DeleteVisual, vframe_DeleteVisual); S(GetSceneBackground, vframe_GetSceneBackground);
        S(GetSceneFogColor, vframe_GetSceneFogColor); S(GetSceneFogEnable, vframe_GetSceneFogEnable);
        S(GetSceneFogMode, vframe_GetSceneFogMode); S(GetSceneFogParams, vframe_GetSceneFogParams);
        S(SetSceneBackground, vframe_SetSceneBackground); S(SetSceneBackgroundRGB, vframe_SetSceneBackgroundRGB);
        S(SetSceneBackgroundDepth, vframe_SetSceneBackgroundDepth); S(SetSceneBackgroundImage, vframe_SetSceneBackgroundImage);
        S(SetSceneFogEnable, vframe_SetSceneFogEnable); S(SetSceneFogColor, vframe_SetSceneFogColor);
        S(SetSceneFogMode, vframe_SetSceneFogMode); S(SetSceneFogParams, vframe_SetSceneFogParams);
        S(SetColor, vframe_SetColor); S(SetColorRGB, vframe_SetColorRGB); S(GetZbufferMode, vframe_GetZbufferMode);
        S(SetMaterialMode, vframe_SetMaterialMode); S(SetOrientation, vframe_SetOrientation);
        S(SetPosition, vframe_SetPosition); S(SetRotation, vframe_SetRotation); S(SetSortMode, vframe_SetSortMode);
        S(SetTexture, vframe_SetTexture); S(SetTextureTopology, vframe_SetTextureTopology);
        S(SetVelocity, vframe_SetVelocity); S(SetZbufferMode, vframe_SetZbufferMode); S(Transform, vframe_Transform);
        S(AddMoveCallback2, vframe_AddMoveCallback2);
#undef S
    }
    return t;
}
static vframe *vframe_new(vframe *parent)
{
    vframe *f = (vframe *)calloc(1, sizeof *f);
    vrm_init(&f->o, vframe_vt(), RK_FRAME, vframe_destroy);
    m_ident(f->m);
    f->bg = 0xFF000000u;
    f->fog_end = 1; f->fog_density = 1;
    if (parent) { vframe_AddChild(parent, f); }
    return f;
}

/* ---- texture ---- */
typedef struct vtex {
    vrmobj o;
    vsurf *surf;
    float decal_w, decal_h;
    LONG decal_x, decal_y;
    DWORD decal_scale, decal_transparent, decal_tcolor, colors, shades;
    void *gpu;           /* renderer cache */
    DWORD gpu_gen;
} vtex;
static void (*g_vrm_tex_free)(vtex *t);    /* renderer: drop the GPU copy */
static void vtex_destroy(vrmobj *o) { vtex *t = (vtex *)o; if (g_vrm_tex_free) g_vrm_tex_free(t); if (t->surf) vobj_release(&t->surf->hd); }
static HRESULT WINAPI vtex_InitFromSurface(vtex *t, vsurf *s)
{
    if (s) vobj_addref(&s->hd);
    if (t->surf) vobj_release(&t->surf->hd);
    t->surf = s;
    return S_OK;
}
static HRESULT WINAPI vtex_Changed(vtex *t, BOOL px, BOOL pal) { (void)px; (void)pal; t->gpu_gen = 0; return S_OK; }
static HRESULT WINAPI vtex_SetColors(vtex *t, DWORD n) { t->colors = n; return S_OK; }
static HRESULT WINAPI vtex_SetShades(vtex *t, DWORD n) { t->shades = n; return S_OK; }
static DWORD WINAPI vtex_GetColors(vtex *t) { return t->colors; }
static DWORD WINAPI vtex_GetShades(vtex *t) { return t->shades; }
static HRESULT WINAPI vtex_SetDecalSize(vtex *t, float w, float h) { t->decal_w = w; t->decal_h = h; return S_OK; }
static HRESULT WINAPI vtex_GetDecalSize(vtex *t, float *w, float *h) { *w = t->decal_w; *h = t->decal_h; return S_OK; }
static HRESULT WINAPI vtex_SetDecalOrigin(vtex *t, LONG x, LONG y) { t->decal_x = x; t->decal_y = y; return S_OK; }
static HRESULT WINAPI vtex_GetDecalOrigin(vtex *t, LONG *x, LONG *y) { *x = t->decal_x; *y = t->decal_y; return S_OK; }
static HRESULT WINAPI vtex_SetDecalScale(vtex *t, DWORD s) { t->decal_scale = s; return S_OK; }
static DWORD WINAPI vtex_GetDecalScale(vtex *t) { return t->decal_scale; }
static HRESULT WINAPI vtex_SetDecalTransparency(vtex *t, BOOL b) { t->decal_transparent = b; return S_OK; }
static DWORD WINAPI vtex_GetDecalTransparency(vtex *t) { return t->decal_transparent; }
static HRESULT WINAPI vtex_SetDecalTransparentColor(vtex *t, DWORD c) { t->decal_tcolor = c; return S_OK; }
static DWORD WINAPI vtex_GetDecalTransparentColor(vtex *t) { return t->decal_tcolor; }
static DWORD WINAPI vtex_GetImage(vtex *t) { (void)t; return 0; }   /* like the real one for surface textures */
static HRESULT WINAPI vtex_GenerateMIPMap(vtex *t, DWORD f) { (void)t; (void)f; return S_OK; }
static vtex *vtex_new(void)
{
    static int init;
    vtex *t = (vtex *)calloc(1, sizeof *t);
    void **v = IDirect3DRMTexture2_vt;
    if (!init) {
        init = 1;
        IDirect3DRMTexture2_vt_init();
        vrm_obj_slots(v);
#define S(n, f) v[IDirect3DRMTexture2_##n] = (void *)f
        S(InitFromSurface, vtex_InitFromSurface); S(Changed, vtex_Changed); S(SetColors, vtex_SetColors);
        S(SetShades, vtex_SetShades); S(SetDecalSize, vtex_SetDecalSize); S(SetDecalOrigin, vtex_SetDecalOrigin);
        S(SetDecalScale, vtex_SetDecalScale); S(SetDecalTransparency, vtex_SetDecalTransparency);
        S(SetDecalTransparentColor, vtex_SetDecalTransparentColor); S(GetDecalSize, vtex_GetDecalSize);
        S(GetDecalOrigin, vtex_GetDecalOrigin); S(GetImage, vtex_GetImage); S(GetShades, vtex_GetShades);
        S(GetColors, vtex_GetColors); S(GetDecalScale, vtex_GetDecalScale); S(GetDecalTransparency, vtex_GetDecalTransparency);
        S(GetDecalTransparentColor, vtex_GetDecalTransparentColor); S(GenerateMIPMap, vtex_GenerateMIPMap);
#undef S
    }
    vrm_init(&t->o, v, RK_TEXTURE, vtex_destroy);
    t->decal_w = t->decal_h = 1;
    t->decal_scale = 1;
    t->colors = 8; t->shades = 16;
    return t;
}

/* ---- light / material ---- */
typedef struct vlight {
    vrmobj o;
    int type;
    float r, g, b, range, umbra, penumbra, att0, att1, att2;
    vrmobj *enable_frame;
} vlight;
static HRESULT WINAPI vlight_SetType(vlight *l, int t) { l->type = t; return S_OK; }
static DWORD WINAPI vlight_GetType(vlight *l) { return l->type; }
static HRESULT WINAPI vlight_SetColor(vlight *l, DWORD c) { l->r = ((c >> 16) & 255) / 255.f; l->g = ((c >> 8) & 255) / 255.f; l->b = (c & 255) / 255.f; return S_OK; }
static HRESULT WINAPI vlight_SetColorRGB(vlight *l, float r, float g, float b) { l->r = r; l->g = g; l->b = b; return S_OK; }
static DWORD WINAPI vlight_GetColor(vlight *l) { return vrm_rgb(l->r, l->g, l->b); }
static HRESULT WINAPI vlight_SetF(float *p, float v) { *p = v; return S_OK; }
static HRESULT WINAPI vlight_SetRange(vlight *l, float v) { return vlight_SetF(&l->range, v); }
static HRESULT WINAPI vlight_SetUmbra(vlight *l, float v) { return vlight_SetF(&l->umbra, v); }
static HRESULT WINAPI vlight_SetPenumbra(vlight *l, float v) { return vlight_SetF(&l->penumbra, v); }
static HRESULT WINAPI vlight_SetAtt0(vlight *l, float v) { return vlight_SetF(&l->att0, v); }
static HRESULT WINAPI vlight_SetAtt1(vlight *l, float v) { return vlight_SetF(&l->att1, v); }
static HRESULT WINAPI vlight_SetAtt2(vlight *l, float v) { return vlight_SetF(&l->att2, v); }
static float WINAPI vlight_GetRange(vlight *l) { return l->range; }
static HRESULT WINAPI vlight_SetEnableFrame(vlight *l, vrmobj *f) { VRM_ADDREF(f); VRM_RELEASE(l->enable_frame); l->enable_frame = f; return S_OK; }
static void vlight_destroy(vrmobj *o) { VRM_RELEASE(((vlight *)o)->enable_frame); }
static vlight *vlight_new(int type, float r, float g, float b)
{
    static int init;
    vlight *l = (vlight *)calloc(1, sizeof *l);
    void **v = IDirect3DRMLight_vt;
    if (!init) {
        init = 1;
        IDirect3DRMLight_vt_init();
        vrm_obj_slots(v);
#define S(n, f) v[IDirect3DRMLight_##n] = (void *)f
        S(SetType, vlight_SetType); S(SetColor, vlight_SetColor); S(SetColorRGB, vlight_SetColorRGB);
        S(SetRange, vlight_SetRange); S(SetUmbra, vlight_SetUmbra); S(SetPenumbra, vlight_SetPenumbra);
        S(SetConstantAttenuation, vlight_SetAtt0); S(SetLinearAttenuation, vlight_SetAtt1);
        S(SetQuadraticAttenuation, vlight_SetAtt2); S(GetRange, vlight_GetRange); S(GetColor, vlight_GetColor);
        S(GetType, vlight_GetType); S(SetEnableFrame, vlight_SetEnableFrame);
#undef S
    }
    vrm_init(&l->o, v, RK_LIGHT, vlight_destroy);
    l->type = type; l->r = r; l->g = g; l->b = b;
    l->range = 256; l->att0 = 1;
    return l;
}

typedef struct vmat { vrmobj o; float power, sr, sg, sb, er, eg, eb; } vmat;
static HRESULT WINAPI vmat_SetPower(vmat *m, float p) { m->power = p; return S_OK; }
static float WINAPI vmat_GetPower(vmat *m) { return m->power; }
static HRESULT WINAPI vmat_SetSpecular(vmat *m, float r, float g, float b) { m->sr = r; m->sg = g; m->sb = b; return S_OK; }
static HRESULT WINAPI vmat_SetEmissive(vmat *m, float r, float g, float b) { m->er = r; m->eg = g; m->eb = b; return S_OK; }
static vmat *vmat_new(float power)
{
    static int init;
    vmat *m = (vmat *)calloc(1, sizeof *m);
    void **v = IDirect3DRMMaterial_vt;
    if (!init) {
        init = 1;
        IDirect3DRMMaterial_vt_init();
        vrm_obj_slots(v);
        v[IDirect3DRMMaterial_SetPower] = (void *)vmat_SetPower;
        v[IDirect3DRMMaterial_GetPower] = (void *)vmat_GetPower;
        v[IDirect3DRMMaterial_SetSpecular] = (void *)vmat_SetSpecular;
        v[IDirect3DRMMaterial_SetEmissive] = (void *)vmat_SetEmissive;
    }
    vrm_init(&m->o, v, RK_MATERIAL, NULL);
    m->power = power;
    return m;
}

/* ---- mesh builder (stage 1: records the file; geometry comes with the .x loader) ---- */
typedef struct vmb {
    vrmobj o;
    char file[MAX_PATH];
    int quality, perspective, colorsource;
    DWORD color;
    vrmobj *texture, *material;
    void *geo;           /* stage 2: loaded geometry */
} vmb;
typedef struct xgeo xgeo;
static void xgeo_free(xgeo *g);
static void x_geo_box(void *geo, D3DVECTOR *box);
static void vmb_destroy(vrmobj *o) { vmb *m = (vmb *)o; VRM_RELEASE(m->texture); VRM_RELEASE(m->material); xgeo_free((xgeo *)m->geo); }
typedef HRESULT (__cdecl *RMTEXCB)(char *name, void *arg, vrmobj **tex);
static HRESULT (*g_vrm_xload)(vmb *mb, const char *file, RMTEXCB cb, void *ctx);   /* stage 2 */
static HRESULT WINAPI vmb_Load(vmb *m, void *src, void *name, DWORD flags, RMTEXCB cb, void *ctx)
{
    (void)name;
    if ((flags & 0xF) != 0) { log_msg("vrm: MeshBuilder::Load flags %lX not supported", flags); return E_NOTIMPL; }
    lstrcpynA(m->file, (const char *)src, sizeof m->file);
    if (g_vrm_xload) return g_vrm_xload(m, m->file, cb, ctx);
    return S_OK;
}
static const char *vrm_mb_file(void *mb) { return ((vmb *)mb)->file; }
static const char *vrm_tex_info(void *tx)
{
    static char b[160];
    vtex *t = (vtex *)tx;
    snprintf(b, sizeof b, "surf %dx%dx%d ck %d decal %.1fx%.1f scale %lu org %ld,%ld transp %lu",
             t->surf ? t->surf->w : 0, t->surf ? t->surf->h : 0, t->surf ? t->surf->bpp : 0, t->surf ? t->surf->has_ck : 0,
             t->decal_w, t->decal_h, t->decal_scale, t->decal_x, t->decal_y, t->decal_transparent);
    return b;
}
static HRESULT WINAPI vmb_SetQuality(vmb *m, int q) { m->quality = q; return S_OK; }
static DWORD WINAPI vmb_GetQuality(vmb *m) { return m->quality; }
static HRESULT WINAPI vmb_SetPerspective(vmb *m, BOOL p) { m->perspective = p; return S_OK; }
static DWORD WINAPI vmb_GetPerspective(vmb *m) { return m->perspective; }
static HRESULT WINAPI vmb_SetColorSource(vmb *m, int s) { m->colorsource = s; return S_OK; }
static DWORD WINAPI vmb_GetColorSource(vmb *m) { return m->colorsource; }
static HRESULT WINAPI vmb_SetColor(vmb *m, DWORD c) { m->color = c; return S_OK; }
static HRESULT WINAPI vmb_SetColorRGB(vmb *m, float r, float g, float b) { m->color = vrm_rgb(r, g, b); return S_OK; }
static HRESULT WINAPI vmb_SetTexture(vmb *m, vrmobj *t) { VRM_ADDREF(t); VRM_RELEASE(m->texture); m->texture = t; return S_OK; }
static HRESULT WINAPI vmb_SetMaterial(vmb *m, vrmobj *t) { VRM_ADDREF(t); VRM_RELEASE(m->material); m->material = t; return S_OK; }
static HRESULT WINAPI vmb_GetBox(vmb *m, D3DVECTOR *box) { if (m->geo) x_geo_box(m->geo, box); else memset(box, 0, 2 * sizeof *box); return S_OK; }
static DWORD WINAPI vmb_Count0(vmb *m) { (void)m; return 0; }
static vmb *vmb_new(void)
{
    static int init;
    vmb *m = (vmb *)calloc(1, sizeof *m);
    void **v = IDirect3DRMMeshBuilder2_vt;
    if (!init) {
        init = 1;
        IDirect3DRMMeshBuilder2_vt_init();
        vrm_obj_slots(v);
#define S(n, f) v[IDirect3DRMMeshBuilder2_##n] = (void *)f
        S(Load, vmb_Load); S(SetQuality, vmb_SetQuality); S(GetQuality, vmb_GetQuality);
        S(SetPerspective, vmb_SetPerspective); S(GetPerspective, vmb_GetPerspective);
        S(SetColorSource, vmb_SetColorSource); S(GetColorSource, vmb_GetColorSource);
        S(SetColor, vmb_SetColor); S(SetColorRGB, vmb_SetColorRGB); S(SetTexture, vmb_SetTexture);
        S(SetMaterial, vmb_SetMaterial); S(GetBox, vmb_GetBox); S(GetFaceCount, vmb_Count0); S(GetVertexCount, vmb_Count0);
#undef S
    }
    vrm_init(&m->o, v, RK_MESHBUILDER, vmb_destroy);
    m->quality = 0x51;   /* D3DRMRENDER_GOURAUD */
    m->color = 0xFFFFFFFF;
    return m;
}

/* ---- animation set (stage 1: accepted, no keys) ---- */
typedef struct xanimset_data xanimset_data;
static xanimset_data *x_load_animset(const char *file, RMTEXCB cb, void *ctx, vframe *parent);
static void x_animset_free(xanimset_data *d);
static void x_animset_settime(xanimset_data *d, float t);
typedef struct vanimset { vrmobj o; float time; xanimset_data *data; } vanimset;
static void vanimset_destroy(vrmobj *o) { x_animset_free(((vanimset *)o)->data); }
static HRESULT WINAPI vanimset_Load(vanimset *a, void *src, void *name, DWORD f, RMTEXCB cb, void *ctx, vframe *parent)
{
    (void)name;
    if ((f & 0xF) != 0) { log_msg("vrm: AnimationSet::Load flags %lX not supported", f); return E_NOTIMPL; }
    x_animset_free(a->data);
    a->data = x_load_animset((const char *)src, cb, ctx, parent);
    return a->data ? S_OK : (0x88760000 | 781);
}
static HRESULT WINAPI vanimset_SetTime(vanimset *a, float t) { a->time = t; x_animset_settime(a->data, t); return S_OK; }
static vanimset *vanimset_new(void)
{
    static int init;
    vanimset *a = (vanimset *)calloc(1, sizeof *a);
    void **v = IDirect3DRMAnimationSet_vt;
    if (!init) {
        init = 1;
        IDirect3DRMAnimationSet_vt_init();
        vrm_obj_slots(v);
        v[IDirect3DRMAnimationSet_Load] = (void *)vanimset_Load;
        v[IDirect3DRMAnimationSet_SetTime] = (void *)vanimset_SetTime;
    }
    vrm_init(&a->o, v, RK_ANIMSET, vanimset_destroy);
    return a;
}

/* ---- device ---- */
typedef struct vrmdev {
    vrmobj o;
    vd3ddev *d3ddev;
    DWORD w, h, quality, shades, buffers, texquality, rendermode;
    BOOL dither;
} vrmdev;
static void vrmdev_destroy(vrmobj *o) { vrmdev *d = (vrmdev *)o; if (d->d3ddev) vobj_release(&d->d3ddev->hd); }
static HRESULT WINAPI vrmdev_Update(vrmdev *d) { (void)d; return S_OK; }
static HRESULT WINAPI vrmdev_SetBufferCount(vrmdev *d, DWORD n) { d->buffers = n; return S_OK; }
static DWORD WINAPI vrmdev_GetBufferCount(vrmdev *d) { return d->buffers; }
static HRESULT WINAPI vrmdev_SetDither(vrmdev *d, BOOL b) { d->dither = b; return S_OK; }
static DWORD WINAPI vrmdev_GetDither(vrmdev *d) { return d->dither; }
static HRESULT WINAPI vrmdev_SetShades(vrmdev *d, DWORD n) { d->shades = n; return S_OK; }
static DWORD WINAPI vrmdev_GetShades(vrmdev *d) { return d->shades; }
static HRESULT WINAPI vrmdev_SetQuality(vrmdev *d, DWORD q) { d->quality = q; return S_OK; }
static DWORD WINAPI vrmdev_GetQuality(vrmdev *d) { return d->quality; }
static HRESULT WINAPI vrmdev_SetTextureQuality(vrmdev *d, DWORD q) { d->texquality = q; return S_OK; }
static DWORD WINAPI vrmdev_GetTextureQuality(vrmdev *d) { return d->texquality; }
static DWORD WINAPI vrmdev_GetWidth(vrmdev *d) { return d->w; }
static DWORD WINAPI vrmdev_GetHeight(vrmdev *d) { return d->h; }
static DWORD WINAPI vrmdev_GetColorModel(vrmdev *d) { (void)d; return D3DCOLOR_RGB; }
static DWORD WINAPI vrmdev_Zero(vrmdev *d) { (void)d; return 0; }
static HRESULT WINAPI vrmdev_SetRenderMode(vrmdev *d, DWORD m) { d->rendermode = m; return S_OK; }
static DWORD WINAPI vrmdev_GetRenderMode(vrmdev *d) { return d->rendermode; }
static HRESULT WINAPI vrmdev_GetD3DDev(vrmdev *d, vd3ddev **out) { *out = d->d3ddev; if (d->d3ddev) vobj_addref(&d->d3ddev->hd); return S_OK; }
static vrmdev *vrmdev_new(vd3ddev *dd)
{
    static int init;
    vrmdev *d = (vrmdev *)calloc(1, sizeof *d);
    void **v = IDirect3DRMDevice2_vt;
    if (!init) {
        init = 1;
        IDirect3DRMDevice2_vt_init();
        vrm_obj_slots(v);
#define S(n, f) v[IDirect3DRMDevice2_##n] = (void *)f
        S(Update, vrmdev_Update); S(SetBufferCount, vrmdev_SetBufferCount); S(GetBufferCount, vrmdev_GetBufferCount);
        S(SetDither, vrmdev_SetDither); S(GetDither, vrmdev_GetDither); S(SetShades, vrmdev_SetShades);
        S(GetShades, vrmdev_GetShades); S(SetQuality, vrmdev_SetQuality); S(GetQuality, vrmdev_GetQuality);
        S(SetTextureQuality, vrmdev_SetTextureQuality); S(GetTextureQuality, vrmdev_GetTextureQuality);
        S(GetWidth, vrmdev_GetWidth); S(GetHeight, vrmdev_GetHeight); S(GetColorModel, vrmdev_GetColorModel);
        S(GetTrianglesDrawn, vrmdev_Zero); S(GetWireframeOptions, vrmdev_Zero);
        S(SetRenderMode, vrmdev_SetRenderMode); S(GetRenderMode, vrmdev_GetRenderMode);
        S(GetDirect3DDevice, vrmdev_GetD3DDev); S(GetDirect3DDevice2, vrmdev_GetD3DDev);
#undef S
    }
    vrm_init(&d->o, v, RK_DEVICE, vrmdev_destroy);
    d->d3ddev = dd;
    if (dd) {
        vobj_addref(&dd->hd);
        if (dd->target) { d->w = dd->target->w; d->h = dd->target->h; }
    }
    d->quality = 0x51; d->shades = 32; d->buffers = 2; d->dither = TRUE;
    return d;
}

/* ---- viewport ---- */
typedef struct vrmvp {
    vrmobj o;
    vrmdev *dev;
    vframe *camera;
    LONG x, y;
    DWORD w, h;
    float front, back, field, pl, pr, pb, pt;
    int projection, uniform, has_plane;
    vframe *last_scene;              /* stage 2: what Render was asked to draw */
} vrmvp;
static void vrmvp_destroy(vrmobj *o) { vrmvp *v = (vrmvp *)o; VRM_RELEASE(v->dev); VRM_RELEASE(v->camera); }
/* the view window at distance 1 from the camera (what the projection uses).
   D3DRM: SetPlane gives the window on the front clipping plane (so it scales with front);
   SetField gives its half size. With uniform scaling (the default) pixels stay square: the
   larger window dimension takes the given extent and the other follows from the aspect
   (the game passes +-0.52 x +-0.684 at 320x240, and only the horizontal is used). */
static void vrmvp_plane(vrmvp *v, float *l, float *r, float *b, float *t)
{
    float fr = v->front > 0 ? v->front : 1;
    if (v->has_plane) { *l = v->pl / fr; *r = v->pr / fr; *b = v->pb / fr; *t = v->pt / fr; }
    else { *l = -v->field / fr; *r = v->field / fr; *b = -v->field / fr; *t = v->field / fr; }
    if (v->uniform && v->w && v->h) {
        if (v->w >= v->h) {
            float half = (*r - *l) * 0.5f * v->h / v->w, c = (*t + *b) * 0.5f;
            *b = c - half; *t = c + half;
        } else {
            float half = (*t - *b) * 0.5f * v->w / v->h, c = (*r + *l) * 0.5f;
            *l = c - half; *r = c + half;
        }
    }
}
static void (*g_vrm_on_render)(vrmvp *vp, vframe *scene);    /* stage 2 */
static void (*g_vrm_on_clear)(vrmvp *vp);
static HRESULT WINAPI vrmvp_Clear(vrmvp *v) { if (g_vrm_on_clear) g_vrm_on_clear(v); return S_OK; }
static HRESULT WINAPI vrmvp_Render(vrmvp *v, vframe *scene) { RTRACE("Render vp %p scene %p camera %p", v, scene, v->camera); if (g_vrm_on_render) g_vrm_on_render(v, scene); return S_OK; }
static HRESULT WINAPI vrmvp_SetFront(vrmvp *v, float f) { v->front = f; return S_OK; }
static HRESULT WINAPI vrmvp_SetBack(vrmvp *v, float f) { v->back = f; return S_OK; }
static HRESULT WINAPI vrmvp_SetField(vrmvp *v, float f) { v->field = f; v->has_plane = 0; return S_OK; }
static float WINAPI vrmvp_GetFront(vrmvp *v) { return v->front; }
static float WINAPI vrmvp_GetBack(vrmvp *v) { return v->back; }
static float WINAPI vrmvp_GetField(vrmvp *v) { return v->field; }
static HRESULT WINAPI vrmvp_SetUniformScaling(vrmvp *v, BOOL b) { v->uniform = b; return S_OK; }
static DWORD WINAPI vrmvp_GetUniformScaling(vrmvp *v) { return v->uniform; }
static HRESULT WINAPI vrmvp_SetCamera(vrmvp *v, vframe *c) { VRM_ADDREF(c); VRM_RELEASE(v->camera); v->camera = c; return S_OK; }
static HRESULT WINAPI vrmvp_GetCamera(vrmvp *v, vframe **c) { *c = v->camera; VRM_ADDREF(v->camera); return S_OK; }
static HRESULT WINAPI vrmvp_GetDevice(vrmvp *v, vrmdev **d) { *d = v->dev; VRM_ADDREF(v->dev); return S_OK; }
static HRESULT WINAPI vrmvp_SetProjection(vrmvp *v, int p) { v->projection = p; return S_OK; }
static DWORD WINAPI vrmvp_GetProjection(vrmvp *v) { return v->projection; }
static HRESULT WINAPI vrmvp_SetPlane(vrmvp *v, float l, float r, float b, float t) { v->pl = l; v->pr = r; v->pb = b; v->pt = t; v->has_plane = 1; return S_OK; }
static HRESULT WINAPI vrmvp_GetPlane(vrmvp *v, float *l, float *r, float *b, float *t)
{
    if (v->has_plane) { *l = v->pl; *r = v->pr; *b = v->pb; *t = v->pt; }
    else { *l = -v->field; *r = v->field; *b = -v->field; *t = v->field; }
    return S_OK;
}
static HRESULT WINAPI vrmvp_Configure(vrmvp *v, LONG x, LONG y, DWORD w, DWORD h) { v->x = x; v->y = y; v->w = w; v->h = h; return S_OK; }
static HRESULT WINAPI vrmvp_ForceUpdate(vrmvp *v, DWORD a, DWORD b, DWORD c, DWORD d) { (void)v; (void)a; (void)b; (void)c; (void)d; return S_OK; }
static DWORD WINAPI vrmvp_GetX(vrmvp *v) { return v->x; }
static DWORD WINAPI vrmvp_GetY(vrmvp *v) { return v->y; }
static DWORD WINAPI vrmvp_GetWidth(vrmvp *v) { return v->w; }
static DWORD WINAPI vrmvp_GetHeight(vrmvp *v) { return v->h; }
/* world point -> homogeneous screen point (x/w, y/w are pixels) */
static HRESULT WINAPI vrmvp_Transform(vrmvp *v, D3DRMVECTOR4D *d, D3DVECTOR *s)
{
    MAT cw, icw;
    float p[3] = { s->x, s->y, s->z }, l, r, b, t, sx, sy;
    if (!v->camera) return E_FAIL;
    vframe_world(v->camera, cw); m_inv(icw, cw); m_xform(p, p, icw);
    vrmvp_plane(v, &l, &r, &b, &t);
    if (p[2] <= 1e-6f) { d->x = d->y = d->z = 0; d->w = 0; return S_OK; }   /* behind the camera */
    sx = p[0] / p[2]; sy = p[1] / p[2];
    d->w = p[2];
    d->x = (v->x + (sx - l) / (r - l) * v->w) * p[2];
    d->y = (v->y + (t - sy) / (t - b) * v->h) * p[2];
    d->z = (v->back > v->front ? (p[2] - v->front) / (v->back - v->front) : 0) * p[2];
    return S_OK;
}
static vrmvp *vrmvp_new(vrmdev *dev, vframe *cam, LONG x, LONG y, DWORD w, DWORD h)
{
    static int init;
    vrmvp *p = (vrmvp *)calloc(1, sizeof *p);
    void **v = IDirect3DRMViewport_vt;
    if (!init) {
        init = 1;
        IDirect3DRMViewport_vt_init();
        vrm_obj_slots(v);
#define S(n, f) v[IDirect3DRMViewport_##n] = (void *)f
        S(Clear, vrmvp_Clear); S(Render, vrmvp_Render); S(SetFront, vrmvp_SetFront); S(SetBack, vrmvp_SetBack);
        S(SetField, vrmvp_SetField); S(SetUniformScaling, vrmvp_SetUniformScaling); S(SetCamera, vrmvp_SetCamera);
        S(SetProjection, vrmvp_SetProjection); S(Transform, vrmvp_Transform); S(Configure, vrmvp_Configure);
        S(ForceUpdate, vrmvp_ForceUpdate); S(SetPlane, vrmvp_SetPlane); S(GetCamera, vrmvp_GetCamera);
        S(GetDevice, vrmvp_GetDevice); S(GetPlane, vrmvp_GetPlane); S(GetUniformScaling, vrmvp_GetUniformScaling);
        S(GetX, vrmvp_GetX); S(GetY, vrmvp_GetY); S(GetWidth, vrmvp_GetWidth); S(GetHeight, vrmvp_GetHeight);
        S(GetField, vrmvp_GetField); S(GetBack, vrmvp_GetBack); S(GetFront, vrmvp_GetFront);
        S(GetProjection, vrmvp_GetProjection);
#undef S
    }
    vrm_init(&p->o, v, RK_VIEWPORT, vrmvp_destroy);
    p->dev = dev; VRM_ADDREF(dev);
    p->camera = cam; VRM_ADDREF(cam);
    p->x = x; p->y = y; p->w = w; p->h = h;
    p->front = 1; p->back = 100; p->field = 0.5f; p->uniform = TRUE;
    return p;
}

/* ---- IDirect3DRM / IDirect3DRM2 (one object, one superset vtable) ---- */
typedef struct vrm { vrmobj o; char search[1024]; } vrm;
static HRESULT WINAPI vrm_CreateFrame(vrm *r, vframe *parent, vframe **out) { (void)r; *out = vframe_new(parent); return S_OK; }
static HRESULT WINAPI vrm_CreateMeshBuilder(vrm *r, vmb **out) { (void)r; *out = vmb_new(); return S_OK; }
static HRESULT WINAPI vrm_CreateAnimationSet(vrm *r, vanimset **out) { (void)r; *out = vanimset_new(); return S_OK; }
static HRESULT WINAPI vrm_CreateLight(vrm *r, int type, DWORD c, vlight **out)
{
    (void)r;
    *out = vlight_new(type, ((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f);
    return S_OK;
}
static HRESULT WINAPI vrm_CreateLightRGB(vrm *r, int type, float cr, float cg, float cb, vlight **out) { (void)r; *out = vlight_new(type, cr, cg, cb); return S_OK; }
static HRESULT WINAPI vrm_CreateMaterial(vrm *r, float p, vmat **out) { (void)r; *out = vmat_new(p); return S_OK; }
static HRESULT WINAPI vrm_CreateDeviceFromD3D(vrm *r, void *d3d, vd3ddev *dev, vrmdev **out) { (void)r; (void)d3d; *out = vrmdev_new(dev); return S_OK; }
static HRESULT WINAPI vrm_CreateViewport(vrm *r, vrmdev *dev, vframe *cam, DWORD x, DWORD y, DWORD w, DWORD h, vrmvp **out)
{
    (void)r;
    *out = vrmvp_new(dev, cam, x, y, w, h);
    if (g_vrm_live < 0) log_msg("");   /* keep the counter referenced */
    return S_OK;
}
static HRESULT WINAPI vrm_CreateTextureFromSurface(vrm *r, vsurf *s, vtex **out) { (void)r; *out = vtex_new(); vtex_InitFromSurface(*out, s); return S_OK; }
static HRESULT WINAPI vrm_CreateObject(vrm *r, REFCLSID cls, IUnknown *outer, REFIID iid, void **out)
{
    vrmobj *o = NULL;
    (void)r; (void)cls; (void)outer;
    if (IsEqualGUID(iid, &IID_D3DRMTexture) || IsEqualGUID(iid, &IID_D3DRMTexture2)) o = &vtex_new()->o;
    else if (IsEqualGUID(iid, &IID_D3DRMFrame) || IsEqualGUID(iid, &IID_D3DRMFrame2)) o = &vframe_new(NULL)->o;
    else if (IsEqualGUID(iid, &IID_D3DRMMeshBuilder) || IsEqualGUID(iid, &IID_D3DRMMeshBuilder2)) o = &vmb_new()->o;
    else if (IsEqualGUID(iid, &IID_D3DRMLight)) o = &vlight_new(0, 1, 1, 1)->o;
    else if (IsEqualGUID(iid, &IID_D3DRMAnimationSet)) o = &vanimset_new()->o;
    if (!o) {
        log_msg("vrm: CreateObject for unsupported interface %08lX-%04X", iid->Data1, iid->Data2);
        *out = NULL;
        return E_NOTIMPL;
    }
    *out = o;
    return S_OK;
}
static HRESULT WINAPI vrm_SetSearchPath(vrm *r, const char *p) { lstrcpynA(r->search, p ? p : "", sizeof r->search); return S_OK; }
static HRESULT WINAPI vrm_AddSearchPath(vrm *r, const char *p)
{
    size_t n = strlen(r->search);
    if (n && n < sizeof r->search - 2) { r->search[n++] = ';'; r->search[n] = 0; }
    lstrcpynA(r->search + n, p ? p : "", (int)(sizeof r->search - n));
    return S_OK;
}
static HRESULT WINAPI vrm_GetSearchPath(vrm *r, DWORD *size, char *out) { return vrm_copystr(r->search, size, out); }
static HRESULT WINAPI vrm_Ok1(vrm *r, DWORD a) { (void)r; (void)a; return S_OK; }
static HRESULT WINAPI vrm_Tick(vrm *r, float t) { (void)r; (void)t; return S_OK; }
static vrm *g_vrm;

static HRESULT WINAPI vrm_Direct3DRMCreate(vrm **out)
{
    static int init;
    void **v = IDirect3DRM2_vt;
    if (!init) {
        init = 1;
        IDirect3DRM2_vt_init();
        v[0] = (void *)vrm_QI; v[1] = (void *)vrm_AddRef; v[2] = (void *)vrm_Release;
#define S(n, f) v[IDirect3DRM2_##n] = (void *)f
        S(CreateObject, vrm_CreateObject); S(CreateFrame, vrm_CreateFrame); S(CreateMeshBuilder, vrm_CreateMeshBuilder);
        S(CreateAnimationSet, vrm_CreateAnimationSet); S(CreateLight, vrm_CreateLight); S(CreateLightRGB, vrm_CreateLightRGB);
        S(CreateMaterial, vrm_CreateMaterial); S(CreateDeviceFromD3D, vrm_CreateDeviceFromD3D);
        S(CreateTextureFromSurface, vrm_CreateTextureFromSurface); S(CreateViewport, vrm_CreateViewport);
        S(SetSearchPath, vrm_SetSearchPath); S(AddSearchPath, vrm_AddSearchPath); S(GetSearchPath, vrm_GetSearchPath);
        S(SetDefaultTextureColors, vrm_Ok1); S(SetDefaultTextureShades, vrm_Ok1); S(Tick, vrm_Tick);
#undef S
    }
    g_vrm = (vrm *)calloc(1, sizeof *g_vrm);
    vrm_init(&g_vrm->o, v, RK_RM, NULL);
    *out = g_vrm;
    log_msg("vrm: Direct3DRMCreate");
    return S_OK;
}
