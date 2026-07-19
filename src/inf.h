/*
 * inf.h - Simplified INF interactive scripting for Outlaws
 *
 * INF files define interactive elements: doors, elevators, triggers.
 * This is a simplified implementation supporting the most common cases.
 *
 * Class types we support:
 *   CLASS: DOOR        - Standard sliding door (moves sector floor/ceiling)
 *   CLASS: ELEV_MOVE   - Moving elevator (raises/lowers floor Y)
 *   CLASS: TRIGGER     - Activates another elevator/door when player enters
 */
#pragma once

#include "engine.h"
#include "lvt.h"

#define INF_MAX_ELEVS   256
#define INF_MAX_STOPS   16
#define INF_MORPH_MAX_VERTS 128  /* Max sector vertices a morph door can move */

typedef enum {
    ELEV_TYPE_FLOOR,        /* Moves floor Y */
    ELEV_TYPE_CEILING,      /* Moves ceiling Y */
    ELEV_TYPE_MOVE_FLOOR,   /* Moves both floor and ceiling (standard elevator) */
    ELEV_TYPE_DOOR,         /* Standard door: floor goes to ceiling height */
    ELEV_TYPE_INV_DOOR,     /* Inverse door: ceiling goes to floor height */
    ELEV_TYPE_SCROLL_FLOOR, /* Scrolls sector floor texture UV (no geometry change) */
    ELEV_TYPE_SCROLL_WALL,  /* Scrolls a specific wall texture UV (no geometry change) */
    ELEV_TYPE_VELOCITY_Z,   /* Pushes player (physics, no geometry change) */
    ELEV_TYPE_MORPH_MOVE,   /* Translates the sector's vertices along ANGLE (sliding door) */
    ELEV_TYPE_MORPH_SPIN,   /* Rotates the sector's vertices around CENTER (swinging door) */
    ELEV_TYPE_CHANGE_LIGHT, /* Animates the sector's ambient light between stops */
    ELEV_TYPE_EXPLODE,      /* One-shot: detonates (damage + sound) when triggered */
} ElevType;

/* A message fired when an elevator reaches a stop:
 *   MESSAGE: <n> <client_sector> <TYPE> <param>
 * Drives scripted sequences (one elevator advancing another). */
typedef struct {
    char client[64];
    i32  client_sector;   /* resolved (-1 = unresolved/SYSTEM) */
    bool to_system;
    int  type;            /* InfMsgType */
    i32  param;
} StopMsg;

#define INF_MAX_STOP_MSGS 8

typedef struct {
    f32 y;           /* Target Y position for this stop */
    f32 delay;       /* Delay in seconds before moving to next stop */
    u32 sound_id;    /* Not used yet */
    StopMsg msgs[INF_MAX_STOP_MSGS];
    u32 msg_count;
} ElevStop;

typedef struct {
    bool      active;
    u32       sector_idx;          /* Which level sector this controls (0xFFFFFFFF = unresolved) */
    char      sector_name[64];     /* Sector name from INF file (resolved to sector_idx on first use) */
    ElevType  type;
    f32       speed;               /* Units per second */

    ElevStop  stops[INF_MAX_STOPS];
    u32       stop_count;
    u32       current_stop;
    u32       next_stop;

    f32       current_y;           /* Current Y position */
    f32       target_y;            /* Target Y (from next_stop) */
    f32       delay_timer;         /* Time left at current stop */
    bool      moving;

    bool      key_trigger;         /* Requires key to activate */
    bool      trigger_activated;   /* Was triggered by a trigger sector? */

    /* Scroll elevators (SCROLL_FLOOR/SCROLL_WALL): direction and accumulated UV offset */
    f32       angle_deg;           /* Scroll/morph-move direction in degrees (0=+X, 90=+Z) */
    f32       scroll_u;            /* Accumulated UV scroll offset U */
    f32       scroll_v;            /* Accumulated UV scroll offset V */

    /* Morph doors (MORPH_MOVE / MORPH_SPIN): baseline (closed) vertex snapshot of
     * the door sector, and the rotation pivot for spin doors. current_y holds the
     * current morph amount (distance for MOVE, degrees for SPIN). */
    f32       center_x, center_z;  /* CENTER: pivot for MORPH_SPIN */
    Vec2      base_verts[INF_MORPH_MAX_VERTS];
    u32       base_vert_count;     /* 0 = not snapshotted / too many verts */
    bool      base_captured;

    /* Sound: played when elevator starts moving */
    char      sound_file[64];      /* WAV filename from INF (0-terminated, may be empty) */
    u32       sound_id;            /* Audio ID (loaded by world_load/main; 0 = none) */

    /* Set by inf_trigger() when elevator actually starts; cleared at start of next inf_update() */
    bool      just_triggered;

    bool      master;              /* MASTER on/off: disabled elevators ignore triggers */

    /* Lock: a door whose sector name encodes a required key (RHSTEEL, BIRON,
     * MHBRASS, IRONDOOR, MHLOCKED, ...) or crowbar. Nudging it without the item
     * shows "you need the X" and plays the locked sound. */
    int       required_key;        /* InfKeyType (0 = unlocked) */
    bool      unlocked;            /* Set once opened with the correct key */
    i32       lock_msg_id;         /* LOCAL.MSG id shown when locked (0 = none) */
    bool      perm_locked;         /* Stuck / locked-from-inside: never opens */

    /* SLAVE: sectors that move together with this (master) elevator. Resolved
     * from names at inf_resolve_sectors. When the master is triggered/nudged,
     * each slave is driven to the same stop. */
    char      slave_names[8][64];
    i32       slave_sectors[8];    /* resolved slave sector indices (-1 unused) */
    u32       slave_count;

    /* Elevator-as-trigger: an ELEVATOR SEQ may carry EVENT_MASK/CLIENT/MESSAGE,
     * making the elevator's own sector a trigger that routes a message to a
     * target when the matching event fires. -1/none if absent. */
    u32       self_event_mask;     /* 0 = none */
    int       self_msg;            /* InfMsgType (0 = none) */
    i32       self_msg_param;
    char      self_client[64];
    i32       self_client_sector;  /* -1 = SYSTEM/unresolved */
    bool      self_to_system;
} Elevator;

/* Key / tool types (Outlaws pickups: GSTEEKEY, GIRONKEY, GBRSSKEY, GRKEY,
 * GSQRKEY, GCROWBAR). Player inventory is a bitmask of (1 << InfKeyType). */
typedef enum {
    INF_KEY_NONE = 0,
    INF_KEY_STEEL,
    INF_KEY_IRON,
    INF_KEY_BRASS,
    INF_KEY_ROUND,
    INF_KEY_SQUARE,
    INF_KEY_CROWBAR,
    INF_KEY_GENERIC,   /* MHLOCKED etc. — any key/tool the level provides */
    INF_KEY_SHOVEL,    /* Pelle — digs at marked spots */
    INF_KEY_BADGE,     /* Sheriff's badge (quest item) */
    INF_KEY_COUNT,
} InfKeyType;

/* Result of trying to open a door by nudge. */
typedef enum {
    INF_DOOR_NONE = 0,     /* no door here */
    INF_DOOR_OPENED,       /* opened (was unlocked) */
    INF_DOOR_UNLOCKED,     /* opened using a key the player holds */
    INF_DOOR_LOCKED,       /* locked — player lacks the required key */
} InfDoorResult;

/* Human-readable name of a key type (for the "you need the X" message). */
const char *inf_key_name(int key);
/* Classify a door sector name into a required key (INF_KEY_NONE if unlocked). */
int inf_key_from_name(const char *sector_name);
/* Classify a door lock from its USER_MSG LOCAL.MSG id (authoritative). Sets
 * *perm true for permanently-stuck/locked doors. Returns InfKeyType. */
int inf_key_from_msg(i32 msg_id, bool *perm);

/* ---- Triggers (CLASS: TRIGGER) — send a message to a target when fired ---- */
typedef enum {
    INF_MSG_NONE = 0,
    INF_MSG_NEXT_STOP,
    INF_MSG_PREV_STOP,
    INF_MSG_GOTO_STOP,
    INF_MSG_MASTER_ON,
    INF_MSG_MASTER_OFF,
    INF_MSG_COMPLETE,
    INF_MSG_WAKEUP,
    INF_MSG_USER_MSG,
    INF_MSG_END_LEVEL,
    INF_MSG_DONE,
} InfMsgType;

/* Event mask bits (which action fires a trigger). Values from Outlaws INF. */
#define INF_EVENT_CROSS   0x01u   /* Player crosses/enters the sector */
#define INF_EVENT_ENTER   0x10u   /* Enter sector */
#define INF_EVENT_LEAVE   0x08u   /* Leave sector */
#define INF_EVENT_NUDGE   0x20u   /* Nudge (USE / walk into) */
#define INF_EVENT_SHOOT   0x40u   /* Shot */

#define INF_MAX_TRIGGERS 512

typedef struct {
    bool       active;
    u32        sector_idx;         /* Sector this trigger lives in (0xFFFFFFFF unresolved) */
    char       sector_name[64];
    u32        event_mask;         /* EVENT_MASK: which events fire it */
    InfMsgType msg;                /* MESSAGE type */
    i32        msg_param;          /* Message parameter (stop index / user msg id) */
    char       client_name[64];    /* CLIENT: target sector name, or "SYSTEM" */
    i32        client_sector;      /* Resolved client sector (-1 = SYSTEM/unresolved) */
    bool       to_system;          /* CLIENT: SYSTEM */
    bool       fired_once;         /* For SINGLE triggers */
    bool       single;            /* Fires only once */
} InfTrigger;

typedef struct {
    Elevator elevs[INF_MAX_ELEVS];
    u32      count;
    InfTrigger triggers[INF_MAX_TRIGGERS];
    u32      trigger_count;
    bool     dirty;   /* Set when any elevator moved this frame — renderer should rebuild */

    /* Output events for the game loop to consume (set by inf_fire_*): */
    i32      pending_user_msg;     /* USER_MSG id fired this frame, -1 = none */
    i32      pending_lock_msg;     /* LOCAL.MSG id for a locked door nudge, -1 = none */
    i32      pending_explode_sector;/* EXPLODE elevator fired this frame, -1 = none */
    bool     pending_end_level;    /* END_LEVEL fired */
} InfSystem;

/* Initialize INF system */
void inf_init(InfSystem *inf);

/* Load INF file for a level */
bool inf_load(InfSystem *inf, const char *text, u32 text_len);

/* Update all elevators (call each frame with dt) */
void inf_update(InfSystem *inf, f32 dt, LvtLevel *level);

/* Try to trigger an elevator by sector index.
 * Called when player presses USE near a door/elevator. */
void inf_trigger(InfSystem *inf, u32 sector_idx);

/*
 * Fire all triggers located in `sector_idx` whose EVENT_MASK includes any of
 * the given event bits (INF_EVENT_*). Routes each trigger's MESSAGE to its
 * CLIENT target (elevator NEXT_STOP/GOTO_STOP/etc.), or records SYSTEM events
 * (USER_MSG / END_LEVEL) in the InfSystem pending_* fields for the game loop.
 * Returns the number of triggers fired.
 */
u32 inf_fire_triggers(InfSystem *inf, u32 sector_idx, u32 event_bits);

/* Send a message directly to the elevator(s) controlling `target_sector`. */
void inf_send_message(InfSystem *inf, i32 target_sector,
                      InfMsgType msg, i32 param);

/*
 * Nudge (USE) the door in `sector_idx`. `have_keys` is the player's key bitmask
 * (bit (1<<InfKeyType)). Opens/toggles an unlocked door or one the player can
 * unlock; returns INF_DOOR_LOCKED (and *needed_key) if the required key/tool is
 * missing. Returns INF_DOOR_NONE if there is no door in this sector.
 */
InfDoorResult inf_nudge_door(InfSystem *inf, u32 sector_idx,
                             u32 have_keys, int *needed_key);

/*
 * Nudge (USE) the nearest door within `radius` of (px,pz). More robust than
 * per-sector nudging — the door does not have to be in an adjacent sector.
 * Returns the door result; fills *needed_key on INF_DOOR_LOCKED.
 */
InfDoorResult inf_nudge_door_near(InfSystem *inf, const LvtLevel *level,
                                  f32 px, f32 pz, f32 radius,
                                  u32 have_keys, int *needed_key);

/*
 * Automatic morph-door handling. An ENTER-mask door opens only while the player
 * is actually STANDING IN the door's own sector (the Outlaws EVENT_ENTER
 * semantics) — NOT merely near it, so building doors no longer swing open from
 * across the room. `player_sector` is the player's current sector. `radius`
 * >= 1e5 (the --open-doors debug) forces every morph door open regardless.
 * Nudge-only (NUDGE-mask) doors are untouched here — they wait for USE.
 */
void inf_auto_doors(InfSystem *inf, const LvtLevel *level,
                    int player_sector, f32 radius);

/* Resolve sector names to indices using a parsed level. Call after inf_load + lvt_parse. */
void inf_resolve_sectors(InfSystem *inf, const LvtLevel *level);

/* Get current floor_y for a sector (returns false if not controlled) */
bool inf_get_floor(const InfSystem *inf, u32 sector_idx, f32 *floor_y);
bool inf_get_ceil(const InfSystem *inf, u32 sector_idx, f32 *ceil_y);

/*
 * Get accumulated UV scroll offset for a scroll-floor sector.
 * Returns true and fills u/v if the sector has a SCROLL_FLOOR elevator.
 */
bool inf_get_scroll(const InfSystem *inf, u32 sector_idx, f32 *u, f32 *v);

/* Returns true if sector_idx is a scroll-floor sector (for mesh building). */
bool inf_is_scroll_floor(const InfSystem *inf, u32 sector_idx);

/* Returns true if sector_idx is a MORPH_SPIN/MORPH_MOVE door leaf. Used by the
 * renderer to draw the swinging door panel from the (moving) leaf sector rather
 * than the static neighbour, so the open/close animation is visible. */
bool inf_is_morph_door(const InfSystem *inf, u32 sector_idx);
