/*
 * entity.h - Game entity (enemies, items, decorations)
 *
 * Entities are loaded from OBT files and driven by the AI/game systems.
 * Each entity has a billboard sprite rendered facing the camera.
 */
#pragma once

#include "engine.h"
#include "obt.h"
#include "lab.h"
#include "lvt.h"

/* -------------------------------------------------------------------------
 * Entity kinds
 * ---------------------------------------------------------------------- */
typedef enum {
    ENTITY_NONE        = 0,
    ENTITY_ENEMY,
    ENTITY_ITEM_HEALTH,
    ENTITY_ITEM_AMMO,
    ENTITY_ITEM_KEY,
    ENTITY_ITEM_WEAPON,
    ENTITY_DECORATION,
    ENTITY_TRIGGER,
} EntityKind;

/* -------------------------------------------------------------------------
 * Enemy AI states
 * ---------------------------------------------------------------------- */
typedef enum {
    AI_IDLE  = 0,
    AI_ALERT,
    AI_ATTACK,
    AI_PAIN,
    AI_DEAD,
} AiState;

/* -------------------------------------------------------------------------
 * Animation states (per-entity animation sequences)
 * ---------------------------------------------------------------------- */
#define ENTITY_MAX_SEQ_FRAMES 16
#define ENTITY_NUM_ANIM_STATES 6

typedef enum {
    ANIM_IDLE = 0,
    ANIM_WALK,
    ANIM_ATTACK,
    ANIM_PAIN,
    ANIM_DIE,
    ANIM_DEAD,
} AnimState;

/* Per-state animation sequence data */
typedef struct {
    u32 dir_frames[8][ENTITY_MAX_SEQ_FRAMES]; /* texture IDs per direction per frame */
    /* World-space billboard size of each frame's cell (per direction). Enemy
     * frames vary in size (a lying corpse cell is wide, a standing pose tall),
     * so the billboard must size to the current frame, not the idle cell. */
    f32 fw[8][ENTITY_MAX_SEQ_FRAMES];
    f32 fh[8][ENTITY_MAX_SEQ_FRAMES];
    u32 frame_count;                           /* number of frames in this sequence */
    u32 dt_ms;                                 /* ms per frame */
} EntityAnimSeq;

/* -------------------------------------------------------------------------
 * Scenery (Inv_Object) choreography state machine — Ghidra RE of olwin.exe
 * (Inv_Object@0x418da0, Wax_NextFrame@0x44ca00, WaxInst_SetState@0x44c7e0):
 * an NWX chor is a bytecode script; state 0 is the spawn state; a qualifying
 * hit/nudge advances the object to the NEXT chor (no hit points); the chor's
 * terminal opcode decides the aftermath:
 *   0xFFFF STOP      -> hold last frame forever (debris stays)
 *   0xFFFE LOOP      -> repeat (ambient anims like lantern flames)
 *   0xFFF8 TERMINATE -> delete the actor (bottle shatters away)
 *   0xFFFC SETSTATE  -> jump to chor N (N=0: indestructible clang reaction)
 * A 0xFFFD PLAYSOUND opcode carries a sounds.lst index played at the object.
 * ---------------------------------------------------------------------- */
#define SCN_MAX_CHORS   6
#define SCN_MAX_FRAMES  16

typedef enum {
    SCN_END_STOP = 0,
    SCN_END_LOOP,
    SCN_END_TERMINATE,
    SCN_END_SETSTATE,
} ScnEnd;

typedef struct {
    u32 tex[SCN_MAX_FRAMES];      /* Display frame textures (renderer ids) */
    f32 fw[SCN_MAX_FRAMES];       /* World-space size per frame */
    f32 fh[SCN_MAX_FRAMES];
    u32 nframes;
    u32 dt_ms;                    /* Per-frame display time (from chor rate) */
    i32 sound_idx;                /* sounds.lst index (-1 = none) */
    u8  end;                      /* ScnEnd */
    u8  end_state;                /* target chor for SCN_END_SETSTATE */
} ScnChor;

/* Ray-solidity / reaction class (actor+0x78 in the original):
 * 0 = 0x0400 no TYPE field: rays pass through, purely decorative
 * 1 = 0x1000 TYPE != SHOOT (e.g. NUDGE): ray-solid, reacts to USE/concussion
 * 2 = 0x2000 TYPE SHOOT: ray-solid, reacts to bullets AND USE/concussion */
typedef enum {
    SCENERY_PASS  = 0,
    SCENERY_NUDGE = 1,
    SCENERY_SHOOT = 2,
} SceneryType;

/* -------------------------------------------------------------------------
 * Entity definition (one per entity in the level)
 * ---------------------------------------------------------------------- */
#define ENTITY_NAME_LEN 64

typedef struct {
    bool        active;
    EntityKind  kind;

    Vec3        pos;
    f32         yaw;      /* Facing direction (radians) */

    /* Health / combat */
    i32         health;
    i32         max_health;

    /* AI */
    AiState     ai_state;
    f32         ai_timer;      /* Seconds until next AI action */
    f32         attack_timer;  /* Seconds until next shot */
    f32         alert_range;   /* Distance at which entity notices player */

    /* Rendering - 8 directional sprite textures (dir_tex[0..7]) */
    u32         sprite_tex;           /* Primary texture (dir 0 / fallback) */
    u32         sprite_dir_tex[8];    /* Per-direction textures, 0=missing */
    f32         sprite_w;             /* World-space billboard width (collision) */
    f32         sprite_h;             /* World-space billboard height (collision) */
    f32         render_w;             /* World-space render width from NWX */
    f32         render_h;             /* World-space render height from NWX */
    bool        flat;                 /* Render as horizontal ground quad (not billboard) */

    /* Animation (cyclic frame sequence for animated decorations) */
    u32         anim_tex[8];         /* Texture IDs for each animation frame */
    u32         anim_count;          /* Number of animation frames (0 = not animated) */
    u32         anim_dt_ms;          /* Milliseconds per frame */
    f32         anim_timer;          /* Accumulated time (seconds) */
    u32         anim_frame;          /* Current animation frame index */

    /* Per-AI-state animation sequences (enemies) */
    EntityAnimSeq anim_seqs[ENTITY_NUM_ANIM_STATES];
    AnimState     cur_anim;
    u32           cur_anim_frame;
    f32           cur_anim_timer;
    bool          anim_loop;         /* false for die animation (play once) */
    bool          has_anim_seqs;     /* true if any anim_seqs were loaded */

    /* Scenery chor state machine (Inv_Object decorations) */
    bool        is_scenery;       /* Has ITM FUNC Inv_Object + chor data */
    u8          scenery_type;     /* SceneryType */
    ScnChor     scn[SCN_MAX_CHORS];
    u32         scn_count;
    u32         scn_state;        /* Current chor (0 = spawn state) */
    u32         scn_frame;
    f32         scn_timer;
    bool        scn_playing;      /* Frames advancing (STOP clears) */

    /* Boss (SANCHEZ): dormant spawn candidate until the mission triggers it.
     * Boss candidates load inactive; one is activated when the kill quota is met. */
    bool        is_boss;

    /* Item pickup value */
    i32         pickup_value;

    /* Loot drop: enemies with a non-null ITM DROP_ITEM pre-spawn an inactive
     * pickup (an ammo belt) at load; `drop_entity` is its index (-1 = none),
     * activated at the corpse when this enemy dies. */
    i32         drop_entity;

    /* Cached line-of-sight to the player (recomputed a few times/sec, not every
     * frame — LOS is a full 3D sector trace). */
    f32         los_timer;
    bool        los_cached;

    /* Sound effects (set by world_load from olsfx.lab; 0 = none) */
    u32         sfx_hit;   /* Played when entity takes damage */
    u32         sfx_die;   /* Played when entity dies */

    char        type_name[ENTITY_NAME_LEN];
} Entity;

/* -------------------------------------------------------------------------
 * Entity list for a level
 * ---------------------------------------------------------------------- */
#define MAX_ENTITIES 2048

typedef struct {
    Entity entities[MAX_ENTITIES];
    u32    count;
} EntityList;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/* Classify an OBT type string into an EntityKind */
EntityKind entity_classify(const char *type_name);

/* Initialize entity list */
void entity_list_init(EntityList *list);

/* Clear all entities */
void entity_list_clear(EntityList *list);

/* Add an entity from OBT object (returns index, or -1 on failure) */
int entity_add(EntityList *list, const ObtObject *obj);

/*
 * Update all entities (AI, timers, floor snapping).
 * player_pos: player world position (for alert/attack decisions)
 * dt: delta time in seconds
 * lvt: level geometry for floor-height snapping (may be NULL)
 * player_hurt_cb: called with damage amount when player is shot
 */
void entity_update_all(EntityList *list, Vec3 player_pos, f32 dt,
                       const LvtLevel *lvt,
                       void (*player_hurt_cb)(i32 damage));

/*
 * Try to pick up an item entity that the player is standing near.
 * Returns pickup_value (positive) or 0 if nothing picked up.
 * Deactivates the entity on pickup.
 */
i32 entity_try_pickup(EntityList *list, Vec3 player_pos);

/* Result of a pickup, for the caller to dispatch (heal/ammo/weapon/key). */
typedef struct {
    bool       got;
    EntityKind kind;
    i32        value;
    char       type_name[ENTITY_NAME_LEN];
} PickupResult;

/*
 * Like entity_try_pickup but reports the picked item's kind and OBT type name
 * so the caller can apply the correct effect (health, ammo type, weapon, key).
 * Deactivates the entity on pickup. Returns got=false if nothing was near.
 * Health pickups are SKIPPED (left on the ground) when hp >= max_hp — the
 * original doesn't consume a medkit you can't use.
 */
PickupResult entity_try_pickup_ex(EntityList *list, Vec3 player_pos,
                                  i32 hp, i32 max_hp);

/*
 * Cast a ray from origin in direction dir.
 * Returns the index of the first entity hit, or -1 if none.
 * Sets *dist to the hit distance.
 */
int entity_raycast(const EntityList *list, const LvtLevel *level,
                   Vec3 origin, Vec3 dir,
                   f32 max_dist, f32 *dist);

/* Deal damage to entity at index. Returns true if it died.
 * SHOOT scenery advances its chor state instead of losing health. */
bool entity_damage(EntityList *list, int idx, i32 amount);

/* Sound hook for scenery chor PLAYSOUND events: receives the sounds.lst
 * index and the object position. Set once at startup (main.c). */
void entity_set_sfx_callback(void (*play)(i32 sounds_lst_idx, Vec3 pos));

/* USE/nudge/concussion delivery (message 0x7D3 semantics): advances the chor
 * of SHOOT and NUDGE scenery within `reach` of origin along dir (or, for
 * explosions, within radius when dir is zero). Returns hits. */
int entity_nudge(EntityList *list, Vec3 origin, Vec3 dir, f32 reach);
