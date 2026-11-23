/* Shared layout between MMCDA.dll (32-bit, writer) and the VR viewer (64-bit, reader).
 * Only fixed-size types; no pointers. */
#pragma once
#include <stdint.h>

#define MMVR_MAP_NAME   "Local\\MotorMashVR"
#define MMVR_MAGIC      0x52564D4Du   /* 'MMVR' */
#define MMVR_VERSION    5
#define MMVR_SLOTS      3
#define MMVR_MAX_INST   6144
#define MMVR_MESH_MAGIC 0x4853454Du   /* 'MESH' */
#define MMVR_TEX_MAGIC  0x58455454u   /* 'TTEX' */

#pragma pack(push, 4)
typedef struct {
    uint32_t mesh;          /* id: <cache_dir>\m<id>.bin */
    uint32_t key;           /* frame identity (low 32 bits of the D3DRM frame pointer) */
    float    m[4][3];       /* D3DRM row-vector world matrix: rows 0-2 basis, row 3 translation
                               (scene units, relative to the floating origin) */
} MMVR_INST;

typedef struct {
    uint32_t tick;          /* increasing per captured logic tick */
    uint32_t generation;
    uint32_t count;
    uint32_t pad;
    double   t_ms;          /* QueryPerformanceCounter time of capture, in ms */
    float    origin[2];     /* floating origin X, Z: absolute = translation + (ox, 0, oz) */
    float    cam[4][3];     /* game camera world matrix (same convention as instances) */
    MMVR_INST inst[MMVR_MAX_INST];
} MMVR_FRAME;

typedef struct {
    uint32_t magic, version;
    volatile uint32_t generation;      /* bumps every race: new cache dir, new ids */
    volatile uint32_t latest;          /* index of the newest complete frame slot */
    volatile uint32_t ticks;
    volatile uint32_t meshes_written;  /* mesh files available in this generation */
    float    tick_ms;
    uint32_t pad;
    char     cache_dir[260];
    MMVR_FRAME frames[MMVR_SLOTS];

    /* viewer -> game: controller input, merged into the game's DirectInput keyboard */
    volatile uint32_t in_seq;          /* bumped by the viewer every frame it is running */
    float    in_steer;                 /* -1 left .. +1 right (pulsed into the arrow keys) */
    float    in_accel, in_brake;       /* 0..1 (> 0.3 = key down) */
    volatile uint32_t in_buttons;      /* MMVR_BTN_* */

    /* game -> viewer: the 320-wide HUD overlay, RGBA8 (alpha 0 = transparent) */
    volatile uint32_t hud_seq;
    uint32_t hud_w, hud_h;
    uint32_t hud_rgba[320 * 240];

    /* game -> viewer: textures whose pixels changed (animated); file rewritten first */
    volatile uint32_t texdirty_seq;    /* entry i lives at texdirty[i % 256] */
    uint32_t texdirty[256];

    /* game -> viewer: the whole game screen while in the front end (menus), RGBA8 */
    volatile uint32_t screen_seq;
    volatile uint32_t screen_ms;       /* GetTickCount of the last copy (menus showing) */
    uint32_t screen_w, screen_h;
    uint32_t screen_rgba[640 * 480];
} MMVR_SHARED;

#define MMVR_BTN_FIRE   1u      /* Enter (fire / menu select) */
#define MMVR_BTN_ACTION 2u      /* P */
#define MMVR_BTN_ESC    4u      /* Esc */
#define MMVR_BTN_UP     8u      /* Up arrow (menus) */
#define MMVR_BTN_DOWN  16u      /* Down arrow (menus) */

/* mesh file: MMVR_MESHHDR, then per group: MMVR_GROUPHDR, vcount MMVR_VERTEX, icount uint32 */
#define MMVR_MESH_BILLBOARD 1   /* decal: quad in x/y, always faces the viewer */
typedef struct { uint32_t magic, groups, flags, pad; } MMVR_MESHHDR;
typedef struct { uint32_t vcount, icount, color; int32_t tex; } MMVR_GROUPHDR;  /* color = D3DCOLOR ARGB */
typedef struct { float p[3], n[3], uv[2]; } MMVR_VERTEX;
/* texture file: MMVR_TEXHDR, then width*height RGBA8 */
typedef struct { uint32_t magic, width, height, flags; } MMVR_TEXHDR;
#pragma pack(pop)
