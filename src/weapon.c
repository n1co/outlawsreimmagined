/*
 * weapon.c - Weapon system (data from ITM files via Ghidra RE of olwin.exe)
 *
 * All values extracted from ij*.itm files in olweap.lab (James Anderson variant).
 * Generic i*.itm files differ slightly for other playable characters.
 *
 * Weapon struct offsets confirmed by Ghidra decompilation of FUN_0046fda0
 * (weapon ITM loader) and FUN_00471090 (weapon fire/ammo tick).
 *
 * Ammo types (from outlaws.itm):
 *   ibullit = pistol bullets (slot 2)
 *   icart   = rifle cartridges (slot 3)
 *   ishells = shotgun shells (slots 4, 5, 6 - shared!)
 *   idynam  = dynamite sticks (slot 7, self-ammo)
 *   iknife  = throwing knives (slot 8, self-ammo)
 *   iclip   = gatling ammo belt (slot 9)
 */
#include "weapon.h"
#include <string.h>

/* Weapon definitions from ij*.itm files in olweap.lab (James variant)
 *
 * Fire rate: NOT stored in ITM files. Fire rate is controlled by animation
 * choreography duration (WAX chor system). The fire_rate field below is
 * estimated from gameplay for our reimplementation's timer-based system.
 *
 * max_ammo: NOT stored in weapon ITM (stored in ammo item ITM). Values below
 * are gameplay-reasonable defaults.
 */
const WeaponDef g_weapon_defs[WEAPON_COUNT] = {
    [WEAPON_FIST] = {
        .name = "Fists", .itm_file = "ijfist.itm", .ammo_type = NULL,
        .damage_1 = 0.80f, .damage_2 = 0.30f,
        .clip_size = 0, .pellets = 1,
        .range_1 = 8.0f, .range_2 = 8.0f,
        .effrange_1 = 8.0f, .effrange_2 = 8.0f,
        .spread_1 = 30.0f, .spread_2 = 30.0f,
        .variance_1 = 5.0f, .variance_2 = 5.0f,
        .rate_1 = 60.0f, .rate_2 = 30.0f,
        .mass_1 = 3.0f, .mass_2 = 3.0f,
        .kick_amount = 0.0f, .flash = 0, .max_count = 0,
        .auto_reload = false, .has_scope = false, .is_projectile = false, .melee = true,
        .rest_chor = 0, .fire_chor_1 = 6, .fire_chor_2 = 5, .reload_chor = 0, .no_ammo_chor = 0,
    },
    [WEAPON_PISTOL] = {
        .name = "Pistol", .itm_file = "ijpistol.itm", .ammo_type = "ibullit",
        .damage_1 = 2.0f, .damage_2 = 2.0f,
        .clip_size = 6, .pellets = 1,
        .range_1 = 300.0f, .range_2 = 300.0f,
        .effrange_1 = 120.0f, .effrange_2 = 90.0f,
        .spread_1 = 0.0f, .spread_2 = 0.0f,
        .variance_1 = 1.5f, .variance_2 = 2.5f,
        .rate_1 = 900.0f, .rate_2 = 900.0f,
        .mass_1 = 0.3f, .mass_2 = 0.3f,
        .kick_amount = 0.08f, .flash = 2, .max_count = 0,
        .auto_reload = false, .has_scope = false, .is_projectile = false, .melee = false,
        .rest_chor = 0, .fire_chor_1 = 5, .fire_chor_2 = 6, .reload_chor = 8, .no_ammo_chor = 9,
    },
    [WEAPON_RIFLE] = {
        .name = "Rifle", .itm_file = "ijrifle.itm", .ammo_type = "icart",
        .damage_1 = 2.0f, .damage_2 = 2.0f,
        .clip_size = 12, .pellets = 1,
        .range_1 = 900.0f, .range_2 = 900.0f,
        .effrange_1 = 600.0f, .effrange_2 = 600.0f,
        .spread_1 = 0.0f, .spread_2 = 0.0f,
        .variance_1 = 0.2f, .variance_2 = 0.2f,
        .rate_1 = 1200.0f, .rate_2 = 1200.0f,
        .mass_1 = 0.3f, .mass_2 = 0.3f,
        .kick_amount = 0.1f, .flash = 2, .max_count = 0,
        .auto_reload = false, .has_scope = true, .is_projectile = false, .melee = false,
        .rest_chor = 0, .fire_chor_1 = 5, .fire_chor_2 = 6, .reload_chor = 8, .no_ammo_chor = 9,
        /* Scope: SCOPE_ITEM=iscope, SCOPE_REST_CHOR=20, SCOPE_FIRE_CHOR_1=21,
         * SCOPE_NO_AMMO_CHOR_1=22, SCOPE_X=0, SCOPE_Y=-186 */
    },
    [WEAPON_SHOTGUN] = {
        .name = "Shotgun", .itm_file = "ijsgun.itm", .ammo_type = "ishells",
        .damage_1 = 1.5f, .damage_2 = 1.5f,
        .clip_size = 1, .pellets = 6,
        .range_1 = 200.0f, .range_2 = 200.0f,
        .effrange_1 = 120.0f, .effrange_2 = 120.0f,
        .spread_1 = 4.0f, .spread_2 = 4.0f,
        .variance_1 = 1.0f, .variance_2 = 1.0f,
        .rate_1 = 1200.0f, .rate_2 = 1200.0f,
        .mass_1 = 0.15f, .mass_2 = 0.15f,
        .kick_amount = 0.21f, .flash = 2, .max_count = 0,
        .auto_reload = false, .has_scope = false, .is_projectile = false, .melee = false,
        .rest_chor = 0, .fire_chor_1 = 5, .fire_chor_2 = 5, .reload_chor = 8, .no_ammo_chor = 9,
        /* Note: FIRE_CHOR_2 == FIRE_CHOR_1 (same animation for alt fire) */
    },
    [WEAPON_DBL_SHOTGUN] = {
        .name = "Dbl.Shotgun", .itm_file = "ijdbsgun.itm", .ammo_type = "ishells",
        .damage_1 = 1.0f, .damage_2 = 1.0f,
        .clip_size = 2, .pellets = 5,
        .range_1 = 150.0f, .range_2 = 150.0f,
        .effrange_1 = 100.0f, .effrange_2 = 100.0f,
        .spread_1 = 2.0f, .spread_2 = 2.0f,
        .variance_1 = 1.0f, .variance_2 = 1.0f,
        .rate_1 = 1200.0f, .rate_2 = 1200.0f,
        .mass_1 = 0.06f, .mass_2 = 0.06f,
        .kick_amount = 0.24f, .flash = 2, .max_count = 0,
        .auto_reload = false, .has_scope = false, .is_projectile = false, .melee = false,
        .rest_chor = 0, .fire_chor_1 = 5, .fire_chor_2 = 6, .reload_chor = 8, .no_ammo_chor = 9,
    },
    [WEAPON_SAW_GUN] = {
        .name = "Sawed-off", .itm_file = "ijsawgun.itm", .ammo_type = "ishells",
        .damage_1 = 2.5f, .damage_2 = 1.0f,
        .clip_size = 2, .pellets = 15,
        .range_1 = 30.0f, .range_2 = 30.0f,
        .effrange_1 = 15.0f, .effrange_2 = 15.0f,
        .spread_1 = 18.0f, .spread_2 = 10.0f,
        .variance_1 = 2.0f, .variance_2 = 2.0f,
        .rate_1 = 1200.0f, .rate_2 = 1200.0f,
        .mass_1 = 0.022f, .mass_2 = 0.022f,
        .kick_amount = 0.22f, .flash = 2, .max_count = 0,
        .auto_reload = false, .has_scope = false, .is_projectile = false, .melee = false,
        .rest_chor = 0, .fire_chor_1 = 5, .fire_chor_2 = 6, .reload_chor = 8, .no_ammo_chor = 9,
    },
    [WEAPON_DYNAMITE] = {
        .name = "Dynamite", .itm_file = "ijdynam.itm", .ammo_type = "idynam",
        .damage_1 = 0.0f, .damage_2 = 0.0f,  /* Direct damage 0; explosion via pdynam.itm = 12.0 */
        .clip_size = 1, .pellets = 1,
        .range_1 = 50.0f, .range_2 = 0.0f,
        .effrange_1 = 50.0f, .effrange_2 = 0.0f,
        .spread_1 = 0.0f, .spread_2 = 0.0f,
        .variance_1 = 10.0f, .variance_2 = 0.0f,
        .rate_1 = 60.0f, .rate_2 = 0.0f,
        .mass_1 = 2.0f, .mass_2 = 0.0f,
        .kick_amount = 0.0f, .flash = 0, .max_count = 50,
        .auto_reload = false, .has_scope = false, .is_projectile = true, .melee = false,
        .rest_chor = 0, .fire_chor_1 = 5, .fire_chor_2 = 6, .reload_chor = 0, .no_ammo_chor = 0,
        /* pdynam.itm: EXPLOSION=tntboom.nwx, RADIUS=50, WALL_RADIUS=15,
         * DAMAGE=12.0, LIFE=4500ms, ALLOW_BOUNCE=1, THROW_PITCH=35 */
    },
    [WEAPON_KNIFE] = {
        .name = "Knife", .itm_file = "ijknife.itm", .ammo_type = "iknife",
        .damage_1 = 4.0f, .damage_2 = 4.0f,
        .clip_size = 1, .pellets = 1,
        .range_1 = 30.0f, .range_2 = 8.0f,     /* Primary=throw(30), Alt=stab(8) */
        .effrange_1 = 30.0f, .effrange_2 = 8.0f,
        .spread_1 = 0.0f, .spread_2 = 40.0f,
        .variance_1 = 5.0f, .variance_2 = 5.0f,
        .rate_1 = 120.0f, .rate_2 = 80.0f,
        .mass_1 = 1.0f, .mass_2 = 2.0f,
        .kick_amount = 0.0f, .flash = 0, .max_count = 50,
        .auto_reload = false, .has_scope = false, .is_projectile = true, .melee = false,
        .rest_chor = 0, .fire_chor_1 = 5, .fire_chor_2 = 6, .reload_chor = 0, .no_ammo_chor = 0,
        /* pknife.itm: THROW_PITCH=5, ALLOW_BOUNCE=1, COLLIDE_RADIUS=2.0 */
    },
    [WEAPON_GATLING] = {
        .name = "Gatling", .itm_file = "igatgun.itm", .ammo_type = "iclip",
        .damage_1 = 2.0f, .damage_2 = 2.0f,
        .clip_size = 100, .pellets = 1,
        .range_1 = 300.0f, .range_2 = 300.0f,
        .effrange_1 = 120.0f, .effrange_2 = 120.0f,
        .spread_1 = 0.0f, .spread_2 = 0.0f,
        .variance_1 = 0.0f, .variance_2 = 0.0f,
        .rate_1 = 1200.0f, .rate_2 = 1200.0f,
        .mass_1 = 0.3f, .mass_2 = 0.3f,
        .kick_amount = 0.0f, .flash = 2, .max_count = 0,
        .auto_reload = true, .has_scope = false, .is_projectile = false, .melee = false,
        .rest_chor = 0, .fire_chor_1 = 5, .fire_chor_2 = 6, .reload_chor = 8, .no_ammo_chor = 9,
    },
};

void weapon_init(WeaponState *ws) {
    memset(ws, 0, sizeof(*ws));
    ws->has_weapon[WEAPON_FIST]    = true;
    ws->has_weapon[WEAPON_PISTOL]  = true;
    ws->ammo[WEAPON_PISTOL]        = 20;
    ws->clip[WEAPON_PISTOL]        = 6;
    ws->current                    = WEAPON_PISTOL;
}

bool weapon_switch(WeaponState *ws, WeaponType type) {
    if (type < 0 || type >= WEAPON_COUNT) return false;
    if (!ws->has_weapon[type]) return false;
    ws->current = type;
    ws->fire_cooldown = 0.0f;
    ws->reloading = false;
    return true;
}

void weapon_cycle_next(WeaponState *ws) {
    for (int i = 1; i < WEAPON_COUNT; i++) {
        WeaponType t = (WeaponType)((ws->current + i) % WEAPON_COUNT);
        if (ws->has_weapon[t]) { weapon_switch(ws, t); return; }
    }
}

void weapon_cycle_prev(WeaponState *ws) {
    for (int i = WEAPON_COUNT - 1; i >= 1; i--) {
        WeaponType t = (WeaponType)((ws->current + i) % WEAPON_COUNT);
        if (ws->has_weapon[t]) { weapon_switch(ws, t); return; }
    }
}

void weapon_update(WeaponState *ws, f32 dt) {
    if (ws->fire_cooldown > 0.0f)
        ws->fire_cooldown -= dt;

    /* Reload timer */
    if (ws->reloading) {
        ws->reload_timer -= dt;
        if (ws->reload_timer <= 0.0f) {
            ws->reloading = false;
            WeaponType cur = ws->current;
            const WeaponDef *def = &g_weapon_defs[cur];
            if (def->clip_size > 0) {
                i32 need = def->clip_size - ws->clip[cur];
                i32 take = (ws->ammo[cur] < need) ? ws->ammo[cur] : need;
                ws->clip[cur] += take;
                ws->ammo[cur] -= take;
            }
        }
    }

    /* Auto-reload when clip empty */
    WeaponType cur = ws->current;
    const WeaponDef *def = &g_weapon_defs[cur];
    if (def->clip_size > 0 && ws->clip[cur] <= 0 && ws->ammo[cur] > 0 && !ws->reloading) {
        weapon_reload(ws);
    }
}

bool weapon_can_fire(const WeaponState *ws) {
    if (ws->fire_cooldown > 0.0f) return false;
    if (ws->reloading) return false;
    WeaponType cur = ws->current;
    const WeaponDef *def = &g_weapon_defs[cur];
    if (def->melee) return true;
    if (def->clip_size > 0) return ws->clip[cur] > 0;
    return ws->ammo[cur] > 0;
}

static bool fire_internal(WeaponState *ws) {
    if (!weapon_can_fire(ws)) return false;
    WeaponType cur = ws->current;
    const WeaponDef *def = &g_weapon_defs[cur];

    if (!def->melee) {
        if (def->clip_size > 0) {
            ws->clip[cur]--;
        } else {
            ws->ammo[cur]--;
        }
    }
    /* Fire rate is animation-driven in original (choreography duration).
     * We use estimated cooldowns: melee ~0.4s, pistol ~0.33s, rifle ~0.67s,
     * shotgun ~1.0s, gatling ~0.1s. Approximated from gameplay. */
    static const f32 fire_cooldowns[WEAPON_COUNT] = {
        0.40f,  /* Fist */
        0.33f,  /* Pistol */
        0.67f,  /* Rifle */
        1.00f,  /* Shotgun */
        0.80f,  /* Dbl.Shotgun */
        0.60f,  /* Sawed-off */
        1.50f,  /* Dynamite */
        0.50f,  /* Knife */
        0.10f,  /* Gatling */
    };
    ws->fire_cooldown = fire_cooldowns[cur];
    return true;
}

bool weapon_fire(WeaponState *ws) {
    return fire_internal(ws);
}

bool weapon_fire_alt(WeaponState *ws) {
    return fire_internal(ws);
}

void weapon_reload(WeaponState *ws) {
    WeaponType cur = ws->current;
    const WeaponDef *def = &g_weapon_defs[cur];
    if (def->clip_size <= 0) return;
    if (ws->clip[cur] >= def->clip_size) return;
    if (ws->ammo[cur] <= 0) return;
    if (ws->reloading) return;

    ws->reloading = true;
    ws->reload_timer = 1.5f; /* Reload duration */
}

/* Max reserve ammo per weapon type (from ammo item definitions, not weapon ITM).
 * These are gameplay-reasonable defaults; original values stored in ammo ITM items. */
static const i32 max_reserve_ammo[WEAPON_COUNT] = {
    0,    /* Fist - no ammo */
    100,  /* Pistol (ibullit) */
    60,   /* Rifle (icart) */
    40,   /* Shotgun (ishells) */
    40,   /* Dbl.Shotgun (ishells - shared with shotgun) */
    20,   /* Sawed-off (ishells - shared) */
    50,   /* Dynamite (idynam, MAX_COUNT=50) */
    50,   /* Knife (iknife, MAX_COUNT=50) */
    200,  /* Gatling (iclip) */
};

i32 weapon_max_ammo(WeaponType type) {
    if (type < 0 || type >= WEAPON_COUNT) return 0;
    return max_reserve_ammo[type];
}

void weapon_pickup(WeaponState *ws, WeaponType type, i32 ammo) {
    if (type < 0 || type >= WEAPON_COUNT) return;
    ws->has_weapon[type] = true;
    const WeaponDef *def = &g_weapon_defs[type];
    ws->ammo[type] += ammo;
    if (ws->ammo[type] > max_reserve_ammo[type])
        ws->ammo[type] = max_reserve_ammo[type];
    /* Fill clip if empty */
    if (def->clip_size > 0 && ws->clip[type] <= 0) {
        i32 fill = (ws->ammo[type] < def->clip_size) ? ws->ammo[type] : def->clip_size;
        ws->clip[type] = fill;
        ws->ammo[type] -= fill;
    }
}

void weapon_add_ammo(WeaponState *ws, WeaponType type, i32 amount) {
    if (type < 0 || type >= WEAPON_COUNT) return;
    ws->ammo[type] += amount;
    if (ws->ammo[type] > max_reserve_ammo[type])
        ws->ammo[type] = max_reserve_ammo[type];
}
