/* xload.h - loader for the game's DirectX .x model files (binary "xof 0302bin 0032").
 *
 * Replaces D3DRM's loader for the new renderer: MeshBuilder::Load (all meshes in the file,
 * frame transforms applied) and AnimationSet::Load (frame hierarchy + keyframes).
 * Templates used by the game's 1046 files: Header, Frame, FrameTransformMatrix, Mesh,
 * MeshMaterialList, Material, TextureFilename, MeshNormals, MeshTextureCoords, AnimationSet,
 * Animation, AnimationKey (+ references to named materials / frames). Plain C, no Windows
 * dependencies beyond file reading, so it carries over to the Android / Quest port.
 */

/* ---- generic parse tree ---- */
typedef struct xval { float f; int i; } xval;
typedef struct xnode {
    char type[40], name[64];
    int is_ref;                       /* {name} reference */
    xval *v; int nv, cv;              /* numbers, in file order */
    char *str[4]; int nstr;           /* strings (TextureFilename) */
    struct xnode **kid; int nkid, ckid;
} xnode;

static void xnode_free(xnode *n)
{
    int i;
    if (!n) return;
    for (i = 0; i < n->nkid; i++) xnode_free(n->kid[i]);
    for (i = 0; i < n->nstr; i++) free(n->str[i]);
    free(n->kid); free(n->v); free(n);
}
static void xnode_addv(xnode *n, float f, int iv)
{
    if (n->nv == n->cv) { n->cv = n->cv ? n->cv * 2 : 64; n->v = (xval *)realloc(n->v, n->cv * sizeof *n->v); }
    n->v[n->nv].f = f; n->v[n->nv].i = iv; n->nv++;
}
static void xnode_addkid(xnode *n, xnode *k)
{
    if (n->nkid == n->ckid) { n->ckid = n->ckid ? n->ckid * 2 : 8; n->kid = (xnode **)realloc(n->kid, n->ckid * sizeof *n->kid); }
    n->kid[n->nkid++] = k;
}

typedef struct { const BYTE *p, *end; int bad; } xrd;
enum { XT_NAME = 1, XT_STRING = 2, XT_INT = 3, XT_GUID = 5, XT_INTLIST = 6, XT_FLOATLIST = 7,
       XT_OBRACE = 10, XT_CBRACE = 11, XT_TEMPLATE = 31 };

static int xr_u16(xrd *r) { int v; if (r->p + 2 > r->end) { r->bad = 1; return -1; } v = r->p[0] | (r->p[1] << 8); r->p += 2; return v; }
static DWORD xr_u32(xrd *r) { DWORD v; if (r->p + 4 > r->end) { r->bad = 1; return 0; } memcpy(&v, r->p, 4); r->p += 4; return v; }
static int xr_peek(xrd *r) { return r->p + 2 <= r->end ? (r->p[0] | (r->p[1] << 8)) : -1; }
static void xr_name(xrd *r, char *out, int cap)
{
    DWORD n = xr_u32(r);
    if (r->p + n > r->end) { r->bad = 1; n = 0; }
    if (out) { int k = (int)min(n, (DWORD)(cap - 1)); memcpy(out, r->p, k); out[k] = 0; }
    r->p += n;
}

static void xr_skip_block(xrd *r)   /* after '{': skip to the matching '}' (templates) */
{
    int depth = 1;
    while (!r->bad && depth > 0 && r->p < r->end) {
        int t = xr_u16(r);
        switch (t) {
        case XT_NAME: xr_name(r, NULL, 0); break;
        case XT_STRING: xr_name(r, NULL, 0); xr_u16(r); break;
        case XT_INT: xr_u32(r); break;
        case XT_GUID: r->p += 16; break;
        case XT_INTLIST: { DWORD n = xr_u32(r); r->p += 4 * n; break; }
        case XT_FLOATLIST: { DWORD n = xr_u32(r); r->p += 4 * n; break; }
        case XT_OBRACE: depth++; break;
        case XT_CBRACE: depth--; break;
        default: break;
        }
    }
}

static xnode *xr_object(xrd *r, const char *type);
static void xr_body(xrd *r, xnode *n)   /* after '{' */
{
    while (!r->bad && r->p < r->end) {
        int t = xr_u16(r);
        if (t == XT_CBRACE) return;
        switch (t) {
        case XT_INTLIST: { DWORD k, c = xr_u32(r); for (k = 0; k < c && !r->bad; k++) { DWORD v = xr_u32(r); xnode_addv(n, (float)(int)v, (int)v); } break; }
        case XT_FLOATLIST: { DWORD k, c = xr_u32(r); for (k = 0; k < c && !r->bad; k++) { float f; DWORD v = xr_u32(r); memcpy(&f, &v, 4); xnode_addv(n, f, (int)f); } break; }
        case XT_INT: { DWORD v = xr_u32(r); xnode_addv(n, (float)(int)v, (int)v); break; }
        case XT_STRING: {
            DWORD c = xr_u32(r);
            if (r->p + c > r->end) { r->bad = 1; break; }
            if (n->nstr < 4) { n->str[n->nstr] = (char *)malloc(c + 1); memcpy(n->str[n->nstr], r->p, c); n->str[n->nstr][c] = 0; n->nstr++; }
            r->p += c;
            xr_u16(r);                     /* terminator token (; or ,) */
            break;
        }
        case XT_GUID: r->p += 16; break;
        case XT_OBRACE: {                  /* reference: { name [guid] } */
            xnode *ref = (xnode *)calloc(1, sizeof *ref);
            ref->is_ref = 1;
            while (!r->bad) {
                int u = xr_u16(r);
                if (u == XT_NAME) xr_name(r, ref->name, sizeof ref->name);
                else if (u == XT_GUID) r->p += 16;
                else if (u == XT_CBRACE) break;
                else if (u < 0) break;
            }
            xnode_addkid(n, ref);
            break;
        }
        case XT_NAME: {                    /* nested data object */
            char type[40];
            xr_name(r, type, sizeof type);
            xnode_addkid(n, xr_object(r, type));
            break;
        }
        default: break;                    /* separators */
        }
    }
}
static xnode *xr_object(xrd *r, const char *type)   /* after the type name */
{
    xnode *n = (xnode *)calloc(1, sizeof *n);
    lstrcpynA(n->type, type, sizeof n->type);
    for (;;) {
        int t = xr_peek(r);
        if (t == XT_NAME) { xr_u16(r); xr_name(r, n->name, sizeof n->name); }
        else if (t == XT_GUID) { xr_u16(r); r->p += 16; }
        else break;
    }
    if (xr_u16(r) != XT_OBRACE) { r->bad = 1; return n; }
    xr_body(r, n);
    return n;
}

/* whole file -> root node whose children are the top-level objects */
static xnode *x_parse_file(const char *path)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD size, got;
    BYTE *buf;
    xrd r;
    xnode *root;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(h, NULL);
    buf = (BYTE *)malloc(size + 2);
    if (!ReadFile(h, buf, size, &got, NULL) || got != size) { CloseHandle(h); free(buf); return NULL; }
    CloseHandle(h);
    if (size < 16 || memcmp(buf, "xof ", 4) != 0 || memcmp(buf + 8, "bin ", 4) != 0 || memcmp(buf + 12, "0032", 4) != 0) {
        log_msg("xload: %s is not a binary 32-bit .x file", path);
        free(buf);
        return NULL;
    }
    r.p = buf + 16; r.end = buf + size; r.bad = 0;
    root = (xnode *)calloc(1, sizeof *root);
    strcpy(root->type, "root");
    while (!r.bad && r.p < r.end) {
        int t = xr_u16(&r);
        if (t == XT_TEMPLATE) {
            while (!r.bad && xr_u16(&r) != XT_OBRACE) {}
            xr_skip_block(&r);
        } else if (t == XT_NAME) {
            char type[40];
            xr_name(&r, type, sizeof type);
            xnode_addkid(root, xr_object(&r, type));
        } else if (t < 0) break;
    }
    if (r.bad) log_msg("xload: %s: parse error near offset %u", path, (unsigned)(r.p - buf));
    free(buf);
    return root;
}

static xnode *x_find_named(xnode *n, const char *type, const char *name)
{
    int i;
    if (!n->is_ref && !_stricmp(n->name, name) && (!type || !strcmp(n->type, type))) return n;
    for (i = 0; i < n->nkid; i++) { xnode *f = x_find_named(n->kid[i], type, name); if (f) return f; }
    return NULL;
}

/* ---- geometry built from the tree ---- */
typedef struct xvert { float p[3], n[3], uv[2]; } xvert;
typedef struct xgroup {
    vrmobj *tex;             /* texture from the load callback (AddRef'd), or NULL */
    float col[4], power, spec[3], emis[3];
    xvert *v; int nv, cv;    /* triangle list (3 vertices per triangle) */
} xgroup;
struct xgeo {
    xgroup *g; int ng;
    float bmin[3], bmax[3];
    int nverts, nfaces;
};

static void xgeo_free(xgeo *g)
{
    int i;
    if (!g) return;
    for (i = 0; i < g->ng; i++) { VRM_RELEASE(g->g[i].tex); free(g->g[i].v); }
    free(g->g);
    free(g);
}

typedef struct { RMTEXCB cb; void *ctx; char dir[MAX_PATH]; } xtexctx;

static vrmobj *x_load_texture(xtexctx *tc, const char *name)
{
    vrmobj *t = NULL;
    char buf[MAX_PATH];
    if (!name || !*name) return NULL;
    lstrcpynA(buf, name, sizeof buf);
    if (tc->cb) {
        if (tc->cb(buf, tc->ctx, &t) != 0) t = NULL;
        /* the game's callback (0x42B540) hands out its own cached texture without AddRef
           (it releases its table at teardown); keep our own reference like D3DRM does */
        VRM_ADDREF(t);
    }
    if (!t) log_msg("xload: texture %s not provided", name);
    return t;
}

static int x_group_for(xgeo *g, xnode *mat, xtexctx *tc, xnode *root)
{
    int i;
    float col[4] = { 1, 1, 1, 1 }, power = 0, spec[3] = { 0, 0, 0 }, emis[3] = { 0, 0, 0 };
    const char *texname = NULL;
    vrmobj *tex;
    if (mat && mat->is_ref) mat = x_find_named(root, "Material", mat->name);
    if (mat) {
        if (mat->nv >= 4) for (i = 0; i < 4; i++) col[i] = mat->v[i].f;
        if (mat->nv >= 5) power = mat->v[4].f;
        if (mat->nv >= 8) for (i = 0; i < 3; i++) spec[i] = mat->v[5 + i].f;
        if (mat->nv >= 11) for (i = 0; i < 3; i++) emis[i] = mat->v[8 + i].f;
        for (i = 0; i < mat->nkid; i++)
            if (!strcmp(mat->kid[i]->type, "TextureFilename") && mat->kid[i]->nstr) texname = mat->kid[i]->str[0];
    }
    tex = texname ? x_load_texture(tc, texname) : NULL;
    for (i = 0; i < g->ng; i++)          /* reuse an identical group */
        if (g->g[i].tex == tex && !memcmp(g->g[i].col, col, sizeof col)) { VRM_RELEASE(tex); return i; }
    g->g = (xgroup *)realloc(g->g, (g->ng + 1) * sizeof *g->g);
    memset(&g->g[g->ng], 0, sizeof g->g[0]);
    g->g[g->ng].tex = tex;
    memcpy(g->g[g->ng].col, col, sizeof col);
    g->g[g->ng].power = power;
    memcpy(g->g[g->ng].spec, spec, sizeof spec);
    memcpy(g->g[g->ng].emis, emis, sizeof emis);
    return g->ng++;
}

static void x_push(xgroup *gr, const xvert *v)
{
    if (gr->nv == gr->cv) { gr->cv = gr->cv ? gr->cv * 2 : 96; gr->v = (xvert *)realloc(gr->v, gr->cv * sizeof *gr->v); }
    gr->v[gr->nv++] = *v;
}

/* append one Mesh node (transformed by m) to geo */
static void x_add_mesh(xgeo *geo, xnode *mesh, MAT m, xtexctx *tc, xnode *root)
{
    int nvert, nface, i, k, pos;
    float (*vp)[3];
    xnode *mats = NULL, *norms = NULL, *uvs = NULL;
    int *facemat = NULL, nmatnodes = 0, matgroups[64];
    if (mesh->nv < 2) return;
    nvert = mesh->v[0].i;
    if (nvert <= 0 || 1 + nvert * 3 >= mesh->nv) return;
    vp = (float (*)[3])malloc(nvert * sizeof *vp);
    for (i = 0; i < nvert; i++) {
        float p[3] = { mesh->v[1 + i * 3].f, mesh->v[2 + i * 3].f, mesh->v[3 + i * 3].f };
        m_xform(vp[i], p, m);
    }
    pos = 1 + nvert * 3;
    nface = mesh->v[pos++].i;
    for (i = 0; i < mesh->nkid; i++) {
        xnode *k2 = mesh->kid[i];
        if (!strcmp(k2->type, "MeshMaterialList")) mats = k2;
        else if (!strcmp(k2->type, "MeshNormals")) norms = k2;
        else if (!strcmp(k2->type, "MeshTextureCoords")) uvs = k2;
    }
    /* material per face */
    if (mats && mats->nv >= 2) {
        int nfi = mats->v[1].i;
        facemat = (int *)calloc(nface, sizeof *facemat);
        for (i = 0; i < nface && i < nfi && 2 + i < mats->nv; i++) facemat[i] = mats->v[2 + i].i;
        for (i = 0; i < mats->nkid && nmatnodes < 64; i++) matgroups[nmatnodes++] = x_group_for(geo, mats->kid[i], tc, root);
    }
    if (!nmatnodes) { matgroups[0] = x_group_for(geo, NULL, tc, root); nmatnodes = 1; }
    {
        int npos = pos, nnorm = 0, nfn = 0, nposn = 0;
        float (*nv)[3] = NULL;
        if (norms && norms->nv > 1) {
            nnorm = norms->v[0].i;
            nv = (float (*)[3])malloc((nnorm ? nnorm : 1) * sizeof *nv);
            for (i = 0; i < nnorm && 1 + i * 3 + 2 < norms->nv; i++) {
                float n3[3] = { norms->v[1 + i * 3].f, norms->v[2 + i * 3].f, norms->v[3 + i * 3].f };
                MAT r; memcpy(r, m, sizeof r); r[3][0] = r[3][1] = r[3][2] = 0;
                m_xform(nv[i], n3, r);
                v3_norm(nv[i]);
            }
            nposn = 1 + nnorm * 3;
            nfn = nposn < norms->nv ? norms->v[nposn++].i : 0;
        }
        for (i = 0; i < nface && npos < mesh->nv; i++) {
            int cnt = mesh->v[npos++].i, gi = matgroups[0], ncnt = 0, nstart = 0;
            xgroup *gr;
            if (facemat && facemat[i] >= 0 && facemat[i] < nmatnodes) gi = matgroups[facemat[i]];
            gr = &geo->g[gi];
            if (nv && i < nfn && nposn < norms->nv) { ncnt = norms->v[nposn].i; nstart = nposn + 1; nposn += 1 + ncnt; }
            if (cnt < 3 || npos + cnt > mesh->nv) { npos += max(cnt, 0); continue; }
            for (k = 1; k + 1 < cnt; k++) {          /* fan triangulation */
                int corner[3] = { 0, k, k + 1 }, c;
                xvert tri[3];
                for (c = 0; c < 3; c++) {
                    int vi = mesh->v[npos + corner[c]].i;
                    xvert *xv = &tri[c];
                    memset(xv, 0, sizeof *xv);
                    if (vi < 0 || vi >= nvert) vi = 0;
                    memcpy(xv->p, vp[vi], sizeof xv->p);
                    if (uvs && 1 + vi * 2 + 1 < uvs->nv) { xv->uv[0] = uvs->v[1 + vi * 2].f; xv->uv[1] = uvs->v[2 + vi * 2].f; }
                    if (nv && corner[c] < ncnt) {
                        int ni = norms->v[nstart + corner[c]].i;
                        if (ni >= 0 && ni < nnorm) memcpy(xv->n, nv[ni], sizeof xv->n);
                    }
                }
                if (!nv) {   /* no normals in the file: flat face normal */
                    float a[3], b[3], fn[3];
                    for (c = 0; c < 3; c++) { a[c] = tri[1].p[c] - tri[0].p[c]; b[c] = tri[2].p[c] - tri[0].p[c]; }
                    v3_cross(fn, a, b); v3_norm(fn);
                    for (c = 0; c < 3; c++) memcpy(tri[c].n, fn, sizeof fn);
                }
                if (fabsf(tri[0].uv[0] - tri[1].uv[0]) < 1e-4f && fabsf(tri[0].uv[0] - tri[2].uv[0]) < 1e-4f &&
                    fabsf(tri[0].uv[1] - tri[1].uv[1]) < 1e-4f && fabsf(tri[0].uv[1] - tri[2].uv[1]) < 1e-4f) {
                    /* (within 1e-4, far below a texel: the taxi's bumper has a stray 5.7e-43) */
                    /* one texture coordinate for the whole face: the model paints it one colour,
                       the texel D3DRM samples there (wrapping, so v = 1.0 is the top row). Often
                       it sits exactly on the texture's edge (the taxi's bumper uses (0, 1)), where
                       rounding across the face flips between texels on opposite sides: a speckle.
                       Wrap it into [0, 1) and move it a hair into its texel. */
                    for (c = 0; c < 2; c++) {
                        float u = tri[0].uv[c] - floorf(tri[0].uv[c]), d = (1.0f - u) * 0.5f;
                        u += d < 1e-4f ? d : 1e-4f;
                        tri[0].uv[c] = tri[1].uv[c] = tri[2].uv[c] = u;
                    }
                }
                for (c = 0; c < 3; c++) x_push(gr, &tri[c]);
                {   /* bounds */
                    int a2, d;
                    for (a2 = 0; a2 < 3; a2++)
                        for (d = 0; d < 3; d++) {
                            if (tri[a2].p[d] < geo->bmin[d]) geo->bmin[d] = tri[a2].p[d];
                            if (tri[a2].p[d] > geo->bmax[d]) geo->bmax[d] = tri[a2].p[d];
                        }
                }
            }
            geo->nfaces++;
            npos += cnt;
        }
        free(nv);
    }
    geo->nverts += nvert;
    free(vp); free(facemat);
}

/* all meshes under n, with frame transforms applied (MeshBuilder::Load semantics) */
static void x_collect_meshes(xgeo *geo, xnode *n, MAT m, xtexctx *tc, xnode *root)
{
    int i;
    MAT local;
    memcpy(local, m, sizeof local);
    if (!strcmp(n->type, "Frame")) {
        for (i = 0; i < n->nkid; i++)
            if (!strcmp(n->kid[i]->type, "FrameTransformMatrix") && n->kid[i]->nv >= 16) {
                MAT fm; int k;
                for (k = 0; k < 16; k++) (&fm[0][0])[k] = n->kid[i]->v[k].f;
                m_mul(local, fm, m);
            }
    }
    if (!strcmp(n->type, "Mesh")) { x_add_mesh(geo, n, local, tc, root); return; }
    if (!strcmp(n->type, "Material") || !strcmp(n->type, "AnimationSet")) return;
    for (i = 0; i < n->nkid; i++)
        if (!n->kid[i]->is_ref) x_collect_meshes(geo, n->kid[i], local, tc, root);
}

static void x_resolve_path(const char *file, char *out, int cap)
{
    char sp[1024], *tok, *ctx = NULL;
    if (GetFileAttributesA(file) != INVALID_FILE_ATTRIBUTES || !g_vrm) { lstrcpynA(out, file, cap); return; }
    lstrcpynA(sp, g_vrm->search, sizeof sp);
    for (tok = strtok_s(sp, ";", &ctx); tok; tok = strtok_s(NULL, ";", &ctx)) {
        snprintf(out, cap, "%s\\%s", tok, file);
        if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return;
    }
    lstrcpynA(out, file, cap);
}

static xgeo *x_load_geo(const char *file, RMTEXCB cb, void *ctx)
{
    char path[MAX_PATH];
    xnode *root;
    xgeo *geo;
    xtexctx tc;
    MAT id;
    x_resolve_path(file, path, sizeof path);
    if (!(root = x_parse_file(path))) { log_msg("xload: cannot read %s", path); return NULL; }
    geo = (xgeo *)calloc(1, sizeof *geo);
    geo->bmin[0] = geo->bmin[1] = geo->bmin[2] = 1e30f;
    geo->bmax[0] = geo->bmax[1] = geo->bmax[2] = -1e30f;
    tc.cb = cb; tc.ctx = ctx;
    m_ident(id);
    x_collect_meshes(geo, root, id, &tc, root);
    if (geo->bmin[0] > geo->bmax[0]) memset(geo->bmin, 0, sizeof geo->bmin), memset(geo->bmax, 0, sizeof geo->bmax);
    xnode_free(root);
    return geo;
}

static HRESULT x_meshbuilder_load(vmb *mb, const char *file, RMTEXCB cb, void *ctx)
{
    xgeo *g = x_load_geo(file, cb, ctx);
    if (!g) return 0x88760000 | 781;   /* D3DRMERR_FILENOTFOUND */
    if (mb->geo) xgeo_free((xgeo *)mb->geo);
    mb->geo = g;
    return S_OK;
}

static void x_geo_box(void *geo, D3DVECTOR *box)
{
    xgeo *g = (xgeo *)geo;
    box[0].x = g->bmin[0]; box[0].y = g->bmin[1]; box[0].z = g->bmin[2];
    box[1].x = g->bmax[0]; box[1].y = g->bmax[1]; box[1].z = g->bmax[2];
}

/* ---- AnimationSet::Load: frames + keyframes ---- */
typedef struct xkey { float t, v[16]; } xkey;
typedef struct xanim {
    vframe *frame;                    /* AddRef'd */
    xkey *rot, *scl, *pos, *mtx;      /* key types 0, 1, 2, 4 */
    int nrot, nscl, npos, nmtx;
    MAT base;                         /* the frame's transform from the file */
} xanim;
struct xanimset_data { xanim *a; int na; };

static vframe *x_build_frame(xnode *fr, vframe *parent, xtexctx *tc, xnode *root, xnode **names, vframe **made, int *nmade)
{
    vframe *f = vframe_new(parent);
    xgeo *geo = NULL;
    int i;
    MAT id;
    m_ident(id);
    vrm_SetName(&f->o, fr->name);
    if (*nmade < 256) { names[*nmade] = fr; made[(*nmade)++] = f; }
    for (i = 0; i < fr->nkid; i++) {
        xnode *k = fr->kid[i];
        if (k->is_ref) continue;
        if (!strcmp(k->type, "FrameTransformMatrix") && k->nv >= 16) {
            int j;
            for (j = 0; j < 16; j++) (&f->m[0][0])[j] = k->v[j].f;
        } else if (!strcmp(k->type, "Mesh")) {
            if (!geo) {
                geo = (xgeo *)calloc(1, sizeof *geo);
                geo->bmin[0] = geo->bmin[1] = geo->bmin[2] = 1e30f;
                geo->bmax[0] = geo->bmax[1] = geo->bmax[2] = -1e30f;
            }
            x_add_mesh(geo, k, id, tc, root);
        } else if (!strcmp(k->type, "Frame")) {
            vframe *c = x_build_frame(k, f, tc, root, names, made, nmade);
            VRM_RELEASE(c);                 /* the parent holds it */
        }
    }
    if (geo) {
        vmb *mb = vmb_new();
        mb->geo = geo;
        vframe_AddVisual(f, &mb->o);
        VRM_RELEASE(mb);
    }
    return f;
}

static void x_keys(xnode *k, xanim *a)
{
    /* AnimationKey: keyType, nKeys, then per key: time, nValues, values... */
    int type, n, i, p = 2;
    xkey **dst;
    int *cnt;
    if (k->nv < 2) return;
    type = k->v[0].i; n = k->v[1].i;
    switch (type) {
    case 0: dst = &a->rot; cnt = &a->nrot; break;
    case 1: dst = &a->scl; cnt = &a->nscl; break;
    case 2: dst = &a->pos; cnt = &a->npos; break;
    case 4: dst = &a->mtx; cnt = &a->nmtx; break;
    default: return;
    }
    *dst = (xkey *)calloc(n > 0 ? n : 1, sizeof **dst);
    for (i = 0; i < n && p + 1 < k->nv; i++) {
        int nv2 = k->v[p + 1].i, j;
        (*dst)[i].t = (float)k->v[p].i;
        for (j = 0; j < nv2 && j < 16 && p + 2 + j < k->nv; j++) (*dst)[i].v[j] = k->v[p + 2 + j].f;
        p += 2 + nv2;
        (*cnt)++;
    }
}

static xanimset_data *x_load_animset(const char *file, RMTEXCB cb, void *ctx, vframe *parent)
{
    char path[MAX_PATH];
    xnode *root, *names[256];
    vframe *made[256];
    int nmade = 0, i, j;
    xtexctx tc;
    xanimset_data *d;
    x_resolve_path(file, path, sizeof path);
    if (!(root = x_parse_file(path))) { log_msg("xload: cannot read %s", path); return NULL; }
    tc.cb = cb; tc.ctx = ctx;
    d = (xanimset_data *)calloc(1, sizeof *d);
    for (i = 0; i < root->nkid; i++)
        if (!strcmp(root->kid[i]->type, "Frame") && parent) {
            vframe *f = x_build_frame(root->kid[i], parent, &tc, root, names, made, &nmade);
            VRM_RELEASE(f);
        }
    for (i = 0; i < root->nkid; i++) {
        xnode *as = root->kid[i];
        if (strcmp(as->type, "AnimationSet") != 0) continue;
        for (j = 0; j < as->nkid; j++) {
            xnode *an = as->kid[j];
            xanim a;
            int k, m;
            if (strcmp(an->type, "Animation") != 0) continue;
            memset(&a, 0, sizeof a);
            for (k = 0; k < an->nkid; k++) {
                xnode *c = an->kid[k];
                if (c->is_ref) {
                    for (m = 0; m < nmade; m++)
                        if (!a.frame && !_stricmp(names[m]->name, c->name)) {
                            a.frame = made[m];
                            VRM_ADDREF(a.frame);
                            memcpy(a.base, made[m]->m, sizeof a.base);
                        }
                } else if (!strcmp(c->type, "AnimationKey")) x_keys(c, &a);
            }
            if (!a.frame) { free(a.rot); free(a.scl); free(a.pos); free(a.mtx); continue; }
            d->a = (xanim *)realloc(d->a, (d->na + 1) * sizeof *d->a);
            d->a[d->na++] = a;
        }
    }
    xnode_free(root);
    return d;
}

static void x_animset_free(xanimset_data *d)
{
    int i;
    if (!d) return;
    for (i = 0; i < d->na; i++) {
        VRM_RELEASE(d->a[i].frame);
        free(d->a[i].rot); free(d->a[i].scl); free(d->a[i].pos); free(d->a[i].mtx);
    }
    free(d->a); free(d);
}

static void x_key_lerp(xkey *k, int n, float t, float *out, int nv)
{
    int i, j;
    if (n <= 0) return;
    if (t <= k[0].t || n == 1) { memcpy(out, k[0].v, nv * 4); return; }
    if (t >= k[n - 1].t) { memcpy(out, k[n - 1].v, nv * 4); return; }
    for (i = 0; i + 1 < n && k[i + 1].t < t; i++) {}
    {
        float span = k[i + 1].t - k[i].t, a = (t - k[i].t) / (span > 0 ? span : 1);
        for (j = 0; j < nv; j++) out[j] = k[i].v[j] + (k[i + 1].v[j] - k[i].v[j]) * a;
    }
}

static void x_animset_settime(xanimset_data *d, float t)
{
    int i;
    if (!d) return;
    for (i = 0; i < d->na; i++) {
        xanim *a = &d->a[i];
        MAT m, s, r;
        if (a->nmtx) {
            float v[16];
            x_key_lerp(a->mtx, a->nmtx, t, v, 16);
            memcpy(a->frame->m, v, sizeof v);
            continue;
        }
        m_ident(m);
        if (a->nscl) {
            float v[3];
            x_key_lerp(a->scl, a->nscl, t, v, 3);
            m_ident(s); s[0][0] = v[0]; s[1][1] = v[1]; s[2][2] = v[2];
            m_mul(m, m, s);
        }
        if (a->nrot) {
            float q[4], l, w, x, y, z;
            x_key_lerp(a->rot, a->nrot, t, q, 4);          /* (w, x, y, z), renormalised */
            l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            if (l > 1e-9f) { q[0] /= l; q[1] /= l; q[2] /= l; q[3] /= l; }
            w = q[0]; x = q[1]; y = q[2]; z = q[3];
            m_ident(r);   /* row-vector form, as D3DRMMatrixFromQuaternion */
            r[0][0] = 1 - 2 * (y * y + z * z); r[0][1] = 2 * (x * y + w * z);     r[0][2] = 2 * (x * z - w * y);
            r[1][0] = 2 * (x * y - w * z);     r[1][1] = 1 - 2 * (x * x + z * z); r[1][2] = 2 * (y * z + w * x);
            r[2][0] = 2 * (x * z + w * y);     r[2][1] = 2 * (y * z - w * x);     r[2][2] = 1 - 2 * (x * x + y * y);
            m_mul(m, m, r);
        } else if (!a->nscl) {
            memcpy(m, a->base, sizeof m);   /* keep the file's rotation and scale */
            m[3][0] = m[3][1] = m[3][2] = 0;
        }
        if (a->npos) {
            float v[3];
            x_key_lerp(a->pos, a->npos, t, v, 3);
            m[3][0] = v[0]; m[3][1] = v[1]; m[3][2] = v[2];
        } else {
            m[3][0] = a->base[3][0]; m[3][1] = a->base[3][1]; m[3][2] = a->base[3][2];
        }
        memcpy(a->frame->m, m, sizeof m);
    }
}
