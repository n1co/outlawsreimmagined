/*
 * world.h - World/level management
 *
 * Loads a complete level from the LAB archives:
 *   - Level geometry (LVT)
 *   - Level textures (PCX from oltex.lab)
 *   - Object placement (OBT → entities)
 *   - Sprite textures (WAX from olobj.lab)
 */
#pragma once

#include "engine.h"
#include "lab.h"
#include "lvt.h"
#include "renderer.h"
#include "entity.h"
#include "inf.h"
#include "msg.h"
#include "mission.h"
#include "audio.h"

/* Archives context - all open LAB files */
typedef struct Archives {
    LabArchive  geo;    /* OLGEO.LAB - level geometry */
    LabArchive  tex;    /* oltex.lab - textures */
    LabArchive  sfx;    /* olsfx.lab - sound effects */
    LabArchive  main;   /* outlaws.lab - main data */
    LabArchive  obj;    /* olobj.lab - 3D objects and sprites */
    LabArchive  weap;   /* olweap.lab - weapons */

    /* Patch / DLC archives (olpatch1..3.lab). These OVERRIDE the base archives
     * and add the later campaign levels + "A Handful of Missions" DLC and the
     * multiplayer maps. Higher index = higher precedence. */
    LabArchive  patch[3];
    bool        patch_open[3];

    char        data_dir[512];
    bool        opened;

    /* Base palette from OLPAL.PCX — never modified after archives_open().
     * Some entries are reserved (magenta 255,0,255) as level-specific slots. */
    u8          base_palette[256][3];
    /* Working palette = base_palette merged with level-specific colors.
     * Rebuilt each world_load() by filling magenta slots in base_palette
     * with colors from the level's own palette file. */
    u8          palette[256][3];
    bool        palette_loaded;

    /* Master UI palette from OLPAL.PCX — used for first-person weapon sprites,
     * ammo cartridges and HUD widgets, which are authored against this palette
     * (not the per-level palette). Loaded once at archives_open(). */
    u8          hud_palette[256][3];
    bool        hud_palette_loaded;
} Archives;

typedef struct {
    LvtLevel    lvt;        /* Parsed level geometry */
    EntityList  entities;   /* Entities loaded from OBT */
    InfSystem   inf;        /* Interactive scripting (doors/elevators) */
    MsgTable    messages;   /* LOCAL.MSG id→text table (USER_MSG, key hints) */
    MissionState mission;   /* boss / objective state (Sanchez in TOWN) */
    u32         missing_textures; /* count of level textures not found (health check) */
    u32         inf_unresolved;   /* count of INF sectors that failed to resolve */
    bool        loaded;

    /* Player start (from OBT or fallback) */
    Vec3        player_start;
    f32         player_start_yaw;

    /* Current music track */
    char        music_file[128];
} World;

/* Open all LAB archives. Returns true if essential archives opened. */
bool archives_open(Archives *arc, const char *data_dir);

/*
 * Look up a file across ALL archives with patch precedence: patch3 → patch2 →
 * patch1 → geo → main → tex → obj → weap → sfx. This is how Outlaws resolves
 * data — patch/DLC archives override the base game. Returns NULL if not found.
 */
const u8 *archives_get(const Archives *arc, const char *name, u32 *out_size);

/* Close all archives. */
void archives_close(Archives *arc);

/*
 * Load a level by name. Uploads textures and builds GPU meshes.
 * Loads entities from OBT and attempts to load their WAX sprites.
 */
/* Non-const arc because level loading may update the palette. */
bool world_load(World *world, Archives *arc,
                Renderer *r, const char *level_name);

/* Free world resources. */
void world_free(World *world);
