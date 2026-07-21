/*
 * weapon.c - Weapon system
 *
 * All values are the game's own data (ij*.itm / p*.itm in olweap.lab,
 * verified by extraction) and the decompiled handlers of olwin.exe
 * (Weapon_FireTick @0x471090, Weapon_ReloadStep @0x4709e0, per-weapon
 * handlers registered @0x46c5a0 — see the weapon RE report):
 *   - Fire-mode button tables (.data 0x513c28..): TNT is INVERTED
 *     (LMB=throw, RMB=light); rifle/shotgun have identical buttons.
 *   - DB/sawed-off primary fires BOTH barrels (2 rounds, 2×pellets).
 *   - Reload is PER-ROUND (one round per RELOAD_CHOR loop), interruptible.
 *   - Gatling AUTO_RELOAD is an instant bulk refill.
 *
 * Ammo types (OUTLAWS.ITM): ibullit(100) icart(100) ishells(50, shared by
 * slots 4/5/6) idynam(50) iknife(50) iclip(500).
 */
#include "weapon.h"
#include <string.h>

const WeaponDef g_weapon_defs[WEAPON_COUNT] = {
    [WEAPON_FIST] = {
        .name = "Fists", .itm_file = "ijfist.itm", .ammo_type = NULL,
        .button_mode = {0, 1},
        .damage_1 = 1.0f, .damage_2 = 0.3f,
        .clip_size = 0, .pellets = 1,
        .range_1 = 8.0f, .range_2 = 8.0f,
        .effrange_1 = 8.0f, .effrange_2 = 8.0f,
        .spread_1 = 30.0f, .spread_2 = 30.0f,   /* melee 3-trace arc ±30° */
        .variance_1 = 5.0f, .variance_2 = 5.0f,
        .rate_1 = 60.0f, .rate_2 = 30.0f,
        .mass_1 = 3.0f, .mass_2 = 3.0f,
        .kick_amount = 0.0f, .flash = 0, .max_count = 0,
        .auto_reload = false, .has_scope = false, .is_projectile = false, .melee = true,
        .rest_chor = 0, .fire_chor_1 = 6, .fire_chor_2 = 5, .reload_chor = 0, .no_ammo_chor = 0,
    },
    [WEAPON_PISTOL] = {
        .name = "Pistol", .itm_file = "ijpistol.itm", .ammo_type = "ibullit",
        .button_mode = {0, 1},   /* RMB = fan-fire: faster anim, VAR 2°, EFF 90 */
        .damage_1 = 2.0f, .damage_2 = 2.0f,
        .clip_size = 6, .pellets = 1,
        .range_1 = 300.0f, .range_2 = 300.0f,
        .effrange_1 = 120.0f, .effrange_2 = 90.0f,
        .spread_1 = 0.0f, .spread_2 = 0.0f,
        .variance_1 = 1.0f, .variance_2 = 2.0f,
        .rate_1 = 900.0f, .rate_2 = 900.0f,
        .mass_1 = 0.0f, .mass_2 = 0.0f,
        .kick_amount = 0.08f, .flash = 2, .max_count = 0,
        .auto_reload = false, .has_scope = false, .is_projectile = false, .melee = false,
        .rest_chor = 0, .fire_chor_1 = 5, .fire_chor_2 = 6, .reload_chor = 8, .no_ammo_chor = 9,
    },
    [WEAPON_RIFLE] = {
        .name = "Rifle", .itm_file = "ijrifle.itm", .ammo_type = "icart",
        .button_mode = {0, 0},   /* both buttons fire; scope is a toggle key */
        .damage_1 = 2.0f, .damage_2 = 2.0f,
        .clip_size = 12, .pellets = 1,
        .range_1 = 600.0f, .range_2 = 600.0f,
        .effrange_1 = 250.0f, .effrange_2 = 250.0f,
        .spread_1 = 0.0f, .spread_2 = 0.0f,
        .variance_1 = 0.5f, .variance_2 = 0.5f,
        .rate_1 = 1200.0f, .rate_2 = 1200.0f,
        .mass_1 = 0.0f, .mass_2 = 0.0f,
        .kick_amount = 0.1f, .flash = 2, .max_count = 0,
        .auto_reload = false, .has_scope = true, .is_projectile = false, .melee = false,
        .rest_chor = 0, .fire_chor_1 = 5, .fire_chor_2 = 6, .reload_chor = 8, .no_ammo_chor = 9,
        /* Scoped: no auto-aim, no variance, max trace = full RANGE (600).
         * SCOPE_REST_CHOR 20 / FIRE 21 / NO_AMMO 22, overlay at (0,-186). */
    },
    [WEAPON_SHOTGUN] = {
        .name = "Shotgun", .itm_file = "ijsgun.itm", .ammo_type = "ishells",
        .button_mode = {0, 0},
        .damage_1 = 1.5f, .damage_2 = 1.5f,
        .clip_size = 1, .pellets = 6,     /* single chamber: pump between shots */
        .range_1 = 200.0f, .range_2 = 200.0f,
        .effrange_1 = 120.0f, .effrange_2 = 120.0f,
        .spread_1 = 4.0f, .spread_2 = 4.0f,
        .variance_1 = 1.0f, .variance_2 = 1.0f,
        .rate_1 = 1200.0f, .rate_2 = 1200.0f,
        .mass_1 = 0.15f, .mass_2 = 0.15f,
        .kick_amount = 0.21f, .flash = 2, .max_count = 0,
        .auto_reload = false, .has_scope = false, .is_projectile = false, .melee = false,
        .rest_chor = 0, .fire_chor_1 = 5, .fire_chor_2 = 5, .reload_chor = 8, .no_ammo_chor = 9,
    },
    [WEAPON_DBL_SHOTGUN] = {
        .name = "Dbl.Shotgun", .itm_file = "ijdbsgun.itm", .ammo_type = "ishells",
        .button_mode = {0, 1},   /* LMB = BOTH barrels (2 rounds), RMB = one */
        .damage_1 = 0.75f, .damage_2 = 0.75f,
        .clip_size = 2, .pellets = 5,
        .range_1 = 200.0f, .range_2 = 200.0f,
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
        .button_mode = {0, 1},   /* LMB = BOTH barrels, RMB = one */
        .damage_1 = 2.5f, .damage_2 = 2.5f,
        .clip_size = 2, .pellets = 5,
        .range_1 = 80.0f, .range_2 = 80.0f,
        .effrange_1 = 30.0f, .effrange_2 = 30.0f,
        .spread_1 = 10.0f, .spread_2 = 10.0f,
        .variance_1 = 2.0f, .variance_2 = 2.0f,
        .rate_1 = 1200.0f, .rate_2 = 1200.0f,
        .mass_1 = 0.022f, .mass_2 = 0.022f,
        .kick_amount = 0.22f, .flash = 2, .max_count = 0,
        .auto_reload = false, .has_scope = false, .is_projectile = false, .melee = false,
        .rest_chor = 0, .fire_chor_1 = 5, .fire_chor_2 = 6, .reload_chor = 8, .no_ammo_chor = 9,
    },
    [WEAPON_DYNAMITE] = {
        .name = "Dynamite", .itm_file = "ijdynam.itm", .ammo_type = "idynam",
        .button_mode = {1, 0},   /* INVERTED: LMB=throw(1), RMB=light fuse(0) */
        .damage_1 = 0.0f, .damage_2 = 0.0f,  /* damage comes from pdynam */
        .clip_size = 1, .pellets = 1,
        .range_1 = 50.0f, .range_2 = 0.0f,
        .effrange_1 = 50.0f, .effrange_2 = 0.0f,
        .spread_1 = 0.0f, .spread_2 = 0.0f,
        .variance_1 = 10.0f, .variance_2 = 0.0f,
        .rate_1 = 60.0f, .rate_2 = 0.0f,     /* throw speed 60 × power(.5-1) */
        .mass_1 = 2.0f, .mass_2 = 0.0f,
        .kick_amount = 0.0f, .flash = 0, .max_count = 50,
        .auto_reload = false, .has_scope = false, .is_projectile = true, .melee = false,
        .rest_chor = 0, .fire_chor_1 = 5, .fire_chor_2 = 6, .reload_chor = 0, .no_ammo_chor = 0,
        /* pdynam.itm: RADIUS 50 (people), WALL_RADIUS 15, DAMAGE 12,
         * THROW_PITCH 35°, LIFE 4500ms fuse, ALLOW_BOUNCE, COLLIDE_RADIUS 1 */
    },
    [WEAPON_KNIFE] = {
        .name = "Knife", .itm_file = "ijknife.itm", .ammo_type = "iknife",
        .button_mode = {0, 1},   /* LMB=throw (cooked), RMB=jab (melee) */
        .damage_1 = 4.0f, .damage_2 = 4.0f,
        .clip_size = 1, .pellets = 1,
        .range_1 = 30.0f, .range_2 = 8.0f,
        .effrange_1 = 30.0f, .effrange_2 = 8.0f,
        .spread_1 = 0.0f, .spread_2 = 40.0f,  /* jab: melee 3-trace arc ±40° */
        .variance_1 = 5.0f, .variance_2 = 5.0f,
        .rate_1 = 120.0f, .rate_2 = 80.0f,    /* throw speed 120 × power */
        .mass_1 = 1.0f, .mass_2 = 2.0f,
        .kick_amount = 0.0f, .flash = 0, .max_count = 50,
        .auto_reload = false, .has_scope = false, .is_projectile = true, .melee = false,
        .rest_chor = 0, .fire_chor_1 = 5, .fire_chor_2 = 6, .reload_chor = 0, .no_ammo_chor = 0,
        /* pknife.itm: THROW_PITCH 5°, ALLOW_BOUNCE, COLLIDE_RADIUS 2,
         * DEAD_ITEM gknife = thrown knives are picked back up */
    },
    [WEAPON_GATLING] = {
        .name = "Gatling", .itm_file = "igatgun.itm", .ammo_type = "iclip",
        .button_mode = {0, 0},
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

/* Fire rate = FIRE_CHOR animation length in the original. Estimated chor
 * durations (until 1:1 NWX chor timing is wired to the view model). */
static const f32 fire_cooldowns[WEAPON_COUNT] = {
    0.40f,  /* Fist */
    0.33f,  /* Pistol (fast retrigger class) */
    0.67f,  /* Rifle (lever action) */
    1.00f,  /* Shotgun (pump) */
    0.80f,  /* Dbl.Shotgun */
    0.80f,  /* Sawed-off */
    0.60f,  /* Dynamite release */
    0.50f,  /* Knife */
    0.10f,  /* Gatling (held = continuous) */
};
/* Fan-fire (pistol FIRE_CHOR_2) is the faster secondary animation. */
static const f32 pistol_fan_cooldown = 0.18f;
/* Per-round reload step time (one RELOAD_CHOR loop). */
static const f32 reload_step_time = 0.55f;

/* Max reserve ammo (OUTLAWS.ITM ammo item MAX values). */
static const i32 max_reserve_ammo[WEAPON_COUNT] = {
    0,    /* Fist */
    100,  /* ibullit */
    100,  /* icart */
    50,   /* ishells (shared 4/5/6) */
    50,
    50,
    50,   /* idynam */
    50,   /* iknife */
    500,  /* iclip */
};

void weapon_init(WeaponState *ws) {
    memset(ws, 0, sizeof(*ws));
    /* Player start (cidjames.itm): pistol + rifle, 24 bullets, 16 carts. */
    ws->has_weapon[WEAPON_FIST]    = true;
    ws->has_weapon[WEAPON_PISTOL]  = true;
    ws->ammo[WEAPON_PISTOL]        = 24;
    ws->clip[WEAPON_PISTOL]        = 6;
    ws->current                    = WEAPON_PISTOL;   /* START_WEAPON 2 */
}

bool weapon_switch(WeaponState *ws, WeaponType type) {
    if (type < 0 || type >= WEAPON_COUNT) return false;
    if (!ws->has_weapon[type]) return false;
    if (ws->current == type) return true;
    ws->current = type;
    ws->fire_cooldown = 0.0f;
    ws->reloading = false;
    ws->cooking = false;
    ws->scope_active = false;
    /* Switching away drops a held lit stick at the feet with power 0
     * (weapon-change reset @0x46c3fd) — main.c handles the drop. */
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

    /* Per-round reload (Weapon_ReloadStep @0x4709e0): while the reload key is
     * HELD, the RELOAD_CHOR loops and loads ONE round per loop; releasing the
     * key stops it after the current round (so you can chamber a single round
     * and fire it). Firing also interrupts. */
    ws->reload_click = false;
    {
        WeaponType cur = ws->current;
        const WeaponDef *def = &g_weapon_defs[cur];
        bool can_reload = !def->melee && def->clip_size > 0 && !def->auto_reload &&
                          ws->clip[cur] < def->clip_size && ws->ammo[cur] > 0;
        if (ws->reload_held && can_reload && !ws->reload_interrupt &&
            ws->fire_cooldown <= 0.0f) {
            if (!ws->reloading) { ws->reloading = true; ws->reload_timer = reload_step_time; }
            ws->reload_timer -= dt;
            if (ws->reload_timer <= 0.0f) {
                ws->clip[cur]++;
                ws->ammo[cur]--;
                ws->reload_click = true;              /* → play RELOAD_SOUND */
                ws->reload_timer = reload_step_time;  /* next round if still held */
            }
        } else {
            /* Key released, clip full, out of reserve, or fire pressed → stop. */
            ws->reloading = false;
            ws->reload_interrupt = false;
        }
    }

    /* Gatling AUTO_RELOAD: instant bulk refill when the clip empties. */
    WeaponType cur = ws->current;
    const WeaponDef *def = &g_weapon_defs[cur];
    if (def->auto_reload && def->clip_size > 0 &&
        ws->clip[cur] <= 0 && ws->ammo[cur] > 0) {
        i32 need = def->clip_size - ws->clip[cur];
        i32 take = (ws->ammo[cur] < need) ? ws->ammo[cur] : need;
        ws->clip[cur] += take;
        ws->ammo[cur] -= take;
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

/* Consume `rounds` loaded rounds (DB/sawed primary = 2 = both barrels).
 * Returns the number actually fired. */
i32 weapon_consume(WeaponState *ws, i32 rounds, i32 mode) {
    WeaponType cur = ws->current;
    const WeaponDef *def = &g_weapon_defs[cur];
    i32 fired = 0;
    if (def->melee) {
        fired = 1;
    } else if (def->clip_size > 0) {
        while (fired < rounds && ws->clip[cur] > 0) { ws->clip[cur]--; fired++; }
    } else {
        while (fired < rounds && ws->ammo[cur] > 0) { ws->ammo[cur]--; fired++; }
    }
    if (fired > 0) {
        f32 cd = fire_cooldowns[cur];
        if (cur == WEAPON_PISTOL && mode == 1) cd = pistol_fan_cooldown;
        ws->fire_cooldown = cd;
    }
    return fired;
}

bool weapon_fire(WeaponState *ws) {
    if (!weapon_can_fire(ws)) return false;
    return weapon_consume(ws, 1, 0) > 0;
}

bool weapon_fire_alt(WeaponState *ws) {
    if (!weapon_can_fire(ws)) return false;
    return weapon_consume(ws, 1, 1) > 0;
}

void weapon_reload(WeaponState *ws) {
    WeaponType cur = ws->current;
    const WeaponDef *def = &g_weapon_defs[cur];
    if (def->clip_size <= 0 || def->auto_reload) return;
    if (ws->clip[cur] >= def->clip_size) return;
    if (ws->ammo[cur] <= 0) return;
    if (ws->reloading) return;
    ws->reloading = true;
    ws->reload_interrupt = false;
    ws->reload_timer = reload_step_time;
}

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
