/*
 * weapon.h - Weapon system (data from Ghidra RE + ITM files in olweap.lab)
 *
 * Outlaws weapon slots (from outlaws.itm WEAPON_1..WEAPON_9):
 *   Slot 1 (idx 0): Fists       - melee, no ammo, no clip
 *   Slot 2 (idx 1): Pistol      - 6-round revolver, single/fan fire, ammo=ibullit
 *   Slot 3 (idx 2): Rifle       - 12-round, single shot, has scope, ammo=icart
 *   Slot 4 (idx 3): Shotgun     - 1-shell, 6 pellets, ammo=ishells
 *   Slot 5 (idx 4): Double-barrel shotgun - 2-shell, 5 pellets each, ammo=ishells
 *   Slot 6 (idx 5): Sawed-off   - 2-shell, 15 pellets, close range, ammo=ishells
 *   Slot 7 (idx 6): Dynamite    - throwable explosive, projectile=pdynam, ammo=idynam
 *   Slot 8 (idx 7): Knife       - throwable/melee stab, projectile=pknife, ammo=iknife
 *   Slot 9 (idx 8): Gatling gun - 100-round, full auto, AUTO_RELOAD=1, ammo=iclip
 *
 * Weapon struct layout from Ghidra (FUN_0046fda0, offset from WeaponBlock base):
 *   +0x000: magic 0x57454150 ("WEAP")
 *   +0x004: name[32]
 *   +0x044: handler_name[32]
 *   +0x088: ground_item[32]
 *   +0x0C8: nwx_path[32]
 *   +0x0E8: anim_path[32]
 *   +0x12C: scope_item[32]
 *   +0x14C: has_scope (byte)
 *   +0x150: scope_x (int)
 *   +0x154: scope_y (int)
 *   +0x158: rest_chor (short)
 *   +0x15A: fire_chor[3] (short each) - FIRE_CHOR_1..3
 *   +0x160: no_ammo_chor[3] (short each) - NO_AMMO_CHOR_1..3
 *   +0x166: reload_chor (short)
 *   +0x168: extra_chor (short)
 *   +0x16A: scope_rest_chor (short)
 *   +0x16C: scope_fire_chor[3] (short each)
 *   +0x172: scope_no_ammo_chor[3] (short each)
 *   +0x178: fire_sound[3] (ptr each)
 *   +0x184: no_ammo_sound[3] (ptr each)
 *   +0x190: action_sound[3] (ptr each)
 *   +0x19C: reload_sound (ptr)
 *   +0x1A0: extra_sound (ptr)
 *   +0x1A4: ballistics[3] (7 floats each: range, effrange, spread, variance, rate, mass, damage)
 *   +0x1F8: flash_intensity (int)
 *   +0x1FC: kick_amount (float)
 *   +0x200: is_projectile (byte) - set if PROJECTILE field present
 *   +0x201: auto_reload (byte) - AUTO_RELOAD field
 *   +0x204: loaded_ammo (int) - current loaded rounds (SHOT_CAPACITY at start)
 *   +0x208: shot_capacity (int) - max rounds per clip
 *   +0x20C: shot_multiples (int) - pellets per shot
 *   +0x210: projectile_name[32]
 *   +0x230: ammo_type_name[32]
 *
 * Animation state machine (Ghidra, managed by WAX choreography system):
 *   REST_CHOR (chor 0)        -> idle loop
 *   FIRE_CHOR_1 (chor 5)      -> primary fire, plays once, returns to REST
 *   FIRE_CHOR_2 (chor 6)      -> alternate fire, plays once, returns to REST
 *   RELOAD_CHOR (chor 8)      -> reload animation, plays once, returns to REST
 *   NO_AMMO_CHOR_1 (chor 9)   -> dry fire click (primary), plays once, returns to REST
 *   NO_AMMO_CHOR_2 (chor 9)   -> dry fire click (alternate), plays once, returns to REST
 *
 * Fire mechanism (FUN_00471090):
 *   - Each fire event decrements loaded_ammo (offset +0x204) by 1
 *   - If loaded_ammo == 0 and AUTO_RELOAD is set, calls reload (FUN_0046e040)
 *   - Reload transfers ammo from reserve pool to loaded_ammo (up to shot_capacity)
 *   - Fire rate is NOT in ITM; it's controlled by animation duration (chor length)
 *   - SHOT_MULTIPLES pellets fired per ammo consumed (shotgun pellet count)
 *
 * Ammo system (Ghidra FUN_0046e040):
 *   - Reserve ammo tracked per-ammo-type in player inventory (offset +0x7C in ammo item)
 *   - Reload: need = shot_capacity - loaded_ammo; take = min(reserve, need)
 *   - loaded_ammo += take; reserve -= take
 *   - If take == all reserve: remove ammo item from inventory
 *   - Plays RELOAD_SOUND on reload
 */
#pragma once

#include "engine.h"

typedef enum {
    WEAPON_FIST       = 0,   /* Slot 1 */
    WEAPON_PISTOL     = 1,   /* Slot 2 */
    WEAPON_RIFLE      = 2,   /* Slot 3 */
    WEAPON_SHOTGUN    = 3,   /* Slot 4 */
    WEAPON_DBL_SHOTGUN= 4,   /* Slot 5 */
    WEAPON_SAW_GUN    = 5,   /* Slot 6 */
    WEAPON_DYNAMITE   = 6,   /* Slot 7 */
    WEAPON_KNIFE      = 7,   /* Slot 8 */
    WEAPON_GATLING    = 8,   /* Slot 9 */
    WEAPON_COUNT      = 9,
} WeaponType;

typedef struct {
    const char *name;          /* ITM NAME field */
    const char *itm_file;      /* Source ITM filename (ij*.itm = James variant) */
    const char *ammo_type;     /* Ammo item name (ibullit, icart, ishells, etc.) */
    /* Fire-mode button table (Ghidra 0x513c28..): result of LMB/RMB press.
     * Most weapons {0,1}; rifle/shotgun {0,0} (identical); TNT {1,0} —
     * INVERTED: LMB=throw(mode 1), RMB=light fuse(mode 0). */
    i32         button_mode[2];
    f32         damage_1;      /* DAMAGE_1: primary fire damage per pellet */
    f32         damage_2;      /* DAMAGE_2: alternate fire damage per pellet */
    i32         clip_size;     /* SHOT_CAPACITY: rounds per clip (0 = no clip/melee) */
    i32         pellets;       /* SHOT_MULTIPLES: pellets per shot (1 = single) */
    f32         range_1;       /* RANGE_1: primary fire max range (world units) */
    f32         range_2;       /* RANGE_2: alternate fire max range */
    f32         effrange_1;    /* EFFRANGE_1: effective range (full damage) */
    f32         effrange_2;    /* EFFRANGE_2: effective range alt */
    f32         spread_1;      /* SPREAD_1: pellet spread cone (degrees) */
    f32         spread_2;      /* SPREAD_2: alternate spread */
    f32         variance_1;    /* VARIANCE_1: aim variance (random scatter) */
    f32         variance_2;    /* VARIANCE_2: alternate variance */
    f32         rate_1;        /* RATE_1: projectile velocity (world units/sec) */
    f32         rate_2;        /* RATE_2: alternate velocity */
    f32         mass_1;        /* MASS_1: projectile mass (knockback) */
    f32         mass_2;        /* MASS_2: alternate mass */
    f32         kick_amount;   /* KICK_AMOUNT: view kick on fire */
    i32         flash;         /* FLASH_INTENSITY: muzzle flash intensity */
    i32         max_count;     /* MAX_COUNT: max stack for thrown weapons (0 = use ammo pool) */
    bool        auto_reload;   /* AUTO_RELOAD: auto-reload when clip empty */
    bool        has_scope;     /* SCOPE_ITEM present */
    bool        is_projectile; /* PROJECTILE field present (thrown/explosive) */
    bool        melee;         /* Melee weapon (no ammo consumed, short range) */
    /* Animation choreography indices (from ITM) */
    i32         rest_chor;     /* REST_CHOR: idle animation */
    i32         fire_chor_1;   /* FIRE_CHOR_1: primary fire animation */
    i32         fire_chor_2;   /* FIRE_CHOR_2: alternate fire animation */
    i32         reload_chor;   /* RELOAD_CHOR: reload animation */
    i32         no_ammo_chor;  /* NO_AMMO_CHOR_1: dry fire animation */
} WeaponDef;

/* Global weapon definitions (indexed by WeaponType) */
extern const WeaponDef g_weapon_defs[WEAPON_COUNT];

typedef struct {
    WeaponType  current;
    i32         ammo[WEAPON_COUNT];      /* Reserve ammo per weapon */
    i32         clip[WEAPON_COUNT];      /* Loaded rounds in current clip */
    f32         fire_cooldown;           /* Time until next shot */
    bool        has_weapon[WEAPON_COUNT];

    /* Per-round reload (Weapon_ReloadStep @0x4709e0): the RELOAD_CHOR loops,
     * adding ONE round per loop; interruptible by pressing fire. */
    bool        reloading;
    f32         reload_timer;            /* Time until next round loads */
    bool        reload_interrupt;        /* Fire pressed during reload */

    /* Cook state (knife LMB / TNT LMB wind-up): power = clamp(held, .5, 1) */
    bool        cooking;
    f32         cook_time;               /* Seconds the button has been held */
    i32         cook_mode;               /* Fire mode being cooked */

    /* Dynamite: a lit stick held in hand (fuse keeps running!) */
    bool        holding_lit;
    f32         lit_fuse;                /* Seconds until the held stick blows */

    /* Rifle scope (toggle; requires the weapon's SCOPE_ITEM) */
    bool        scope_active;
} WeaponState;

/* Initialize weapons (fist always available) */
void weapon_init(WeaponState *ws);

/* Switch to weapon type. Returns false if not owned. */
bool weapon_switch(WeaponState *ws, WeaponType type);

/* Try to switch to next available weapon */
void weapon_cycle_next(WeaponState *ws);
void weapon_cycle_prev(WeaponState *ws);

/* Update timers (call each frame with dt) */
void weapon_update(WeaponState *ws, f32 dt);

/* Can the weapon fire right now? */
bool weapon_can_fire(const WeaponState *ws);

/* Fire primary (mode=0) or alternate (mode=1). Returns true if fired. */
bool weapon_fire(WeaponState *ws);
bool weapon_fire_alt(WeaponState *ws);

/* Consume `rounds` loaded rounds and start the fire cooldown for `mode`
 * (both-barrels shots pass rounds=2). Returns the rounds actually fired. */
i32 weapon_consume(WeaponState *ws, i32 rounds, i32 mode);

/* Start reload if needed and possible */
void weapon_reload(WeaponState *ws);

/* Give player a weapon and ammo */
void weapon_pickup(WeaponState *ws, WeaponType type, i32 ammo);

/* Add ammo to a weapon type */
void weapon_add_ammo(WeaponState *ws, WeaponType type, i32 amount);

/* Get max reserve ammo for a weapon type */
i32 weapon_max_ammo(WeaponType type);
