/*
 * lvt.h - Outlaws LVT level format parser
 *
 * LVT is a text-based level format (Jedi Engine style).
 * Levels consist of convex sectors connected via portal walls.
 *
 * Coordinate system (Jedi Engine):
 *   X = right, Z = forward (horizontal axes on the map)
 *   Y = up (vertical, floor/ceiling heights)
 *
 * Each sector has:
 *   - A list of 2D vertices (X, Z)
 *   - A list of walls connecting those vertices
 *   - Floor Y and ceiling Y values
 *   - Floor and ceiling texture references
 *
 * Each wall has:
 *   - Two vertex indices (V1, V2)
 *   - Texture reference (MID = main, TOP = upper, BOT = lower)
 *   - Optional adjoin to another sector (portal)
 */
#pragma once

#include "engine.h"

#define LVT_MAX_NAME       64
#define LVT_MAX_TEXTURES   256
#define LVT_MAX_VERTICES   512
#define LVT_MAX_WALLS      512
#define LVT_MAX_SECTORS    1024

/* Sector flags (first word) — Ghidra sky_DrawCeiling/sky_DrawFloor @0x4b6190/
 * 0x4b6410: bit 0 = ceiling is sky, bit 1 = floor is sky (pit). Bits 30/31 =
 * sloped floor / sloped ceiling (set by the loader from SLOPEDFLOOR/CEILING). */
#define LVT_SEC_FLAG_SKY_CEIL   0x1u  /* Ceiling rendered as parallax sky */
#define LVT_SEC_FLAG_SKY_FLOOR  0x2u  /* Floor rendered as parallax sky (pit) */
/* Bit 8 (0x100): sector is an automatic DOOR — Outlaws auto-creates a mask-
 * scroll door for it at level load with no INF entry (level_LoadSectors @0x41de4c
 * tests flags1 & 0x100 → FUN_0045cbb0 case 6). The door panel is the ADJOIN_MID
 * mask on its walls; opening slides the panel out of view and opens the passage.
 * Bit 9 (0x200) = the door opens DOWNWARD instead of up (direction). NOTE: 0x200
 * WITHOUT 0x100 is NOT a door (just a vestigial direction bit) — see
 * project_doors_flag memory. */
#define LVT_SEC_FLAG_DOOR       0x100u
#define LVT_SEC_FLAG_DOOR_DOWN  0x200u

/* Wall flags (first word) — from LVT binary analysis */
#define LVT_WALL_FLAG_SKY_BOUNDARY  0x20000u  /* Sky-boundary portal wall: skip rendering */
#define LVT_WALL_FLAG_ADJOIN_MID    0x02000u  /* Portal wall renders MID texture (fence/barrier) */

/* Texture UV offset */
typedef struct {
    f32 u, v;
} TexOffset;

/* Wall texture slot */
typedef struct {
    i32       tex_id;   /* Index into level texture list, -1 = none */
    TexOffset offset;
} WallTex;

/* A wall segment between two vertices */
typedef struct {
    u32    id;              /* Original sector ID (hash) */
    i32    v1, v2;          /* Vertex indices (into sector's vertex list) */
    WallTex mid, top, bot, overlay;
    i32    adjoin;          /* Adjacent sector index, -1 = solid wall */
    i32    mirror;          /* Mirror wall index in adjacent sector */
    i32    dadjoin;         /* Dual adjoin (3-sector portal for ADJOIN_MID) */
    i32    dmirror;         /* Dual mirror wall index */
    u32    flags;           /* Wall flags (word 1) */
    u32    flags2;          /* Wall flags (word 2) — WF3_SOLID_WALL = bit 1 */
    i32    light;           /* Additional light offset */
    bool   is_window;       /* Breakable glass window (mask wall with WIN texture) */
    bool   window_broken;   /* Glass window that has been shot out */
} LvtWall;

/* A map sector (convex polygon) */
typedef struct {
    u32  id;                        /* Sector hash ID */
    char name[LVT_MAX_NAME];

    /* Geometry */
    Vec2 vertices[LVT_MAX_VERTICES];
    u32  vertex_count;
    LvtWall walls[LVT_MAX_WALLS];
    u32  wall_count;

    /* Floor */
    f32  floor_y;
    i32  floor_tex;
    TexOffset floor_offset;
    f32  floor_rot_deg;   /* Flat texture rotation (FLOOR Y 5th field, degrees) */

    /* Ceiling */
    f32  ceil_y;
    i32  ceil_tex;
    TexOffset ceil_offset;
    f32  ceil_rot_deg;

    /* Lighting / physics */
    i32  ambient;
    f32  friction;
    f32  gravity;
    f32  elasticity;   /* Bounce restitution (LVT ELASTICITY, default 0.3) */

    /* Sector flags */
    u32  flags;
    i32  layer;

    /* Sloped geometry (SLOPEDFLOOR / SLOPEDCEILING) */
    bool has_slope_floor;
    i32  slope_floor_wall;   /* pivot wall index */
    i32  slope_floor_angle;  /* angle in 14-bit fixed point (16384 = 90°) */
    bool has_slope_ceil;
    i32  slope_ceil_wall;
    i32  slope_ceil_angle;

    /* Morph door (MORPH_SPIN/MOVE) that has swung open far enough that its leaf
     * no longer blocks the doorway. Set by inf_update each frame; when true,
     * collision treats this sector's walls as passable so the player can walk
     * through the open door. */
    bool door_open;

    /* Flag-door (LVT_SEC_FLAG_DOOR / 0x100): an auto-created mask-scroll door
     * with no INF entry. When true, this sector's ADJOIN_MID panel walls are
     * SOLID while the door is shut (door_open == false) and the panel is drawn;
     * once opened they become passable (via door_open) and the panel is hidden.
     * door_slide is the 0..1 open amount (for the sliding-panel animation). */
    bool is_flag_door;
    f32  door_slide;

    /* Water sector (Jedi 2-sector scheme): a floor OR ceiling water texture
     * (e.g. GAWATER1) marks a swimmable sector. Above-water sectors carry the
     * water on the FLOOR (surface seen from above); the underwater sector below
     * (VADJOIN-linked) carries it on the CEILING (surface seen from below) with
     * a real ground floor. The player swims in both. water_at_ceiling = true for
     * the underwater half (surface = ceil_y); otherwise surface = floor_y. */
    bool is_water;
    bool water_at_ceiling;
} LvtSector;

/* Complete parsed level */
typedef struct {
    char name[256];
    char music_file[128];      /* Music track filename from LVT header */
    char palette_name[LVT_MAX_NAME]; /* Primary palette (e.g. "simms") → "simms.pcx" */
    char textures[LVT_MAX_TEXTURES][LVT_MAX_NAME];
    u32  texture_count;

    LvtSector *sectors;
    u32        sector_count;

    /* Sky parallax */
    f32 parallax_x, parallax_y;
} LvtLevel;

/*
 * Parse a LVT level from a text buffer.
 * Returns true on success. Call lvt_free() to release resources.
 */
bool lvt_parse(LvtLevel *level, const char *text, u32 text_len);

/* Free level resources. */
void lvt_free(LvtLevel *level);

/* Find sector index by ID hash. Returns -1 if not found. */
i32 lvt_find_sector(const LvtLevel *level, u32 id);

/* Find sector index by name string (case-insensitive). Returns -1 if not found. */
i32 lvt_find_sector_by_name(const LvtLevel *level, const char *name);

/* Floor / ceiling height of a sector at world point (x,z), accounting for
 * SLOPEDFLOOR / SLOPEDCEILING (ramps). Returns the flat floor_y/ceil_y when the
 * sector has no slope. Used by both the renderer and collision so ramps/stairs
 * are walkable, not just visual. */
f32 lvt_floor_at(const LvtSector *s, f32 x, f32 z);
f32 lvt_ceil_at(const LvtSector *s, f32 x, f32 z);

/* Height of a sloped plane (base height `base`, hinged on wall `wall_idx`,
 * raised by 14-bit-fixed `angle_fixed`) at (x,z). Exact Outlaws/TFE slope math —
 * shared by the renderer and collision so geometry and physics always agree. */
f32 lvt_slope_height(const LvtSector *s, f32 base, i32 wall_idx,
                     i32 angle_fixed, f32 x, f32 z);

/* Returns true if the given texture index refers to DEFAULT.PCX (any position). */
bool lvt_is_default_tex(const LvtLevel *level, i32 tex_id);
