/*
 * entity.c - Game entity system (enemies, items, decorations)
 */
#include "entity.h"
#include "collision.h"
#include <math.h>
#include <string.h>

static void scenery_update(Entity *e, f32 dt);

/* -------------------------------------------------------------------------
 * Sector point-in-polygon test (duplicated from collision.c for floor snap)
 * ---------------------------------------------------------------------- */
static bool entity_point_in_sector(const LvtSector *sec, f32 x, f32 z) {
    int crossings = 0;
    for (u32 i = 0; i < sec->wall_count; i++) {
        const LvtWall *w = &sec->walls[i];
        if (w->v1 < 0 || w->v1 >= (i32)sec->vertex_count) continue;
        if (w->v2 < 0 || w->v2 >= (i32)sec->vertex_count) continue;
        f32 x0 = sec->vertices[w->v1].x, z0 = sec->vertices[w->v1].y;
        f32 x1 = sec->vertices[w->v2].x, z1 = sec->vertices[w->v2].y;
        if ((z0 <= z && z < z1) || (z1 <= z && z < z0)) {
            f32 t = (z - z0) / (z1 - z0);
            if (x < x0 + t * (x1 - x0))
                crossings++;
        }
    }
    return (crossings & 1) != 0;
}

/* Y-aware floor lookup: among the (possibly stacked) sectors containing (x,z),
 * pick the one whose [floor,ceil] span contains the reference Y `refy`; failing
 * that, the sector whose floor is the closest at or below refy. Falls back to
 * the first 2D match. Without this a building's stacked sectors (a room under a
 * roof) resolve to the FIRST match — often the roof — so enemies snap to the
 * rooftop instead of the floor they were placed on. */
static f32 entity_floor_at_y(const LvtLevel *lvt, f32 x, f32 z, f32 refy) {
    if (!lvt) return 0.0f;
    f32 best = 0.0f; bool found = false;
    f32 best_below = -1e30f; bool below = false;
    f32 first = 0.0f; bool have_first = false;
    for (u32 si = 0; si < lvt->sector_count; si++) {
        const LvtSector *s = &lvt->sectors[si];
        if (!entity_point_in_sector(s, x, z)) continue;
        if (!have_first) { first = s->floor_y; have_first = true; }
        /* refy inside this sector's vertical span → this is the level we're on */
        if (refy >= s->floor_y - 4.0f && refy <= s->ceil_y + 0.5f) {
            if (!found || fabsf(s->floor_y - refy) < fabsf(best - refy)) {
                best = s->floor_y; found = true;
            }
        }
        /* else track the highest floor at/below refy */
        if (s->floor_y <= refy + 0.5f && s->floor_y > best_below) {
            best_below = s->floor_y; below = true;
        }
    }
    if (found)      return best;
    if (below)      return best_below;
    if (have_first) return first;
    return refy;
}

/* -------------------------------------------------------------------------
 * Entity classification table
 * ---------------------------------------------------------------------- */
typedef struct {
    const char *type_name;  /* OBT type string (case-insensitive prefix match) */
    EntityKind  kind;
    i32         health;
    i32         pickup_value;
    f32         sprite_w, sprite_h;  /* World units */
    f32         alert_range;
} EntityDef;

static const EntityDef s_defs[] = {
    /*
     * Actual Outlaws OBT entity type names (from CANYON.OBT analysis).
     * Prefix matching handles variants like BGY1WA, BGY1S, BGY2BOOM etc.
     */

    /* Bad guys (standard enemies) - BGY1..BGY9.
     * Health from BGY*.ITM (game damage scale: weapon DAMAGE ~2.0, so enemies
     * die in 1-2 hits — matches the original). HEIGHT = 6.0 world units. */
    { "BGY1",         ENTITY_ENEMY,        2,  0,  6,  7, 90 },
    { "BGY2",         ENTITY_ENEMY,        2,  0,  6,  7, 90 },
    { "BGY3",         ENTITY_ENEMY,        2,  0,  6,  7, 90 },
    { "BGY4",         ENTITY_ENEMY,        2,  0,  6,  7, 90 },
    { "BGY5",         ENTITY_ENEMY,        4,  0,  6,  7, 90 },
    { "BGY6",         ENTITY_ENEMY,        4,  0,  6,  7, 90 },
    { "BGY7",         ENTITY_ENEMY,        2,  0,  6,  7, 90 },
    { "BGY8",         ENTITY_ENEMY,        2,  0,  6,  7, 90 },
    { "BGY9",         ENTITY_ENEMY,        3,  0,  6,  7, 90 },
    /* Named enemies / bosses (tougher) */
    { "EDICKFAR",     ENTITY_ENEMY,       12,  0,  7,  8, 130},
    { "DICKFARM",     ENTITY_ENEMY,       12,  0,  7,  8, 130},
    { "MAN1",         ENTITY_ENEMY,        2,  0,  6,  7, 90 },
    { "MARSHAL",      ENTITY_ENEMY,        8,  0,  7,  8, 110},
    { "JAMES",        ENTITY_ENEMY,       10,  0,  7,  8, 120},
    { "SANCHEZ",      ENTITY_ENEMY,        8,  0,  7,  8, 100},
    { "TWOFEATH",     ENTITY_ENEMY,        6,  0,  6,  7, 90 },
    { "MARY",         ENTITY_ENEMY,        4,  0,  6,  7, 80 },
    /* Story gang bosses. ESLIM before SLIM so the exact/prefix match resolves
     * the difficulty variant first. Slim is HIDEOUT's boss (upstairs room). */
    { "ESLIM",        ENTITY_ENEMY,       12,  0,  7,  8, 130},
    { "SLIM",         ENTITY_ENEMY,       12,  0,  7,  8, 130},
    /* RANCH's two gang bosses: Bob Graham (Bob_New) & Bill Morgan (Buckshot_New).
     * Health from their ITMs (EBobGrah=16, EBillMor=8; base variants 18/14). E-
     * variants first so the exact match wins before the base-name prefix. Sprites
     * resolve via the ITM ANIM field (Bobgrahm.NWX / BillMorg.NWX). */
    { "EBOBGRAH",     ENTITY_ENEMY,       16,  0,  7,  8, 140},
    { "BOBGRAH",      ENTITY_ENEMY,       18,  0,  7,  8, 140},
    { "EBILLMOR",     ENTITY_ENEMY,        8,  0,  7,  8, 140},
    { "BILLMOR",      ENTITY_ENEMY,       14,  0,  7,  8, 140},
    /* TRAIN boss "Timber Bloodeye" (Bloodeye_New): ETIMBLOO (easy variant, hp7)
     * and TIMBLOOD (default/hard variant, hp14) spawn by difficulty at the same
     * spot; sprite = Timblood.NWX via the ITM ANIM field. */
    { "ETIMBLOO",     ENTITY_ENEMY,        7,  0,  7,  8, 140},
    { "TIMBLOOD",     ENTITY_ENEMY,       14,  0,  7,  8, 140},

    /* Health pickups */
    { "GDOCBAG",      ENTITY_ITEM_HEALTH, 0,  50,  8,  8,   0},
    { "GELIXIR",      ENTITY_ITEM_HEALTH, 0,  25,  6,  8,   0},
    { "GCANTEEN",     ENTITY_ITEM_HEALTH, 0,  20,  6,  8,   0},

    /* Weapon pickups (OBT ground-item names = each weapon ITM's GROUND_ITEM
     * field: gpistol/grifle/gsgun/gsawgun/gdbsgun/gknife/ggatgun). The base
     * game scatters GPISTOL/GRIFLE/GKNIFE too — they MUST be pickable, not
     * treated as decoration. The pickup_value carries the WeaponType index. */
    { "GPISTOL",      ENTITY_ITEM_WEAPON, 0,   1,  8,  8,   0},  /* WEAPON_PISTOL */
    { "GRIFLE",       ENTITY_ITEM_WEAPON, 0,   2,  8,  8,   0},  /* WEAPON_RIFLE */
    { "GSCOPE",       ENTITY_ITEM_WEAPON, 0,   2,  8,  8,   0},  /* scoped rifle */
    { "GSAWGUN",      ENTITY_ITEM_WEAPON, 0,   5,  8,  8,   0},  /* WEAPON_SAW_GUN */
    { "GDBSGUN",      ENTITY_ITEM_WEAPON, 0,   4,  8,  8,   0},  /* WEAPON_DBL_SHOTGUN */
    { "GSGUN",        ENTITY_ITEM_WEAPON, 0,   3,  8,  8,   0},  /* WEAPON_SHOTGUN */
    { "GGATGUN",      ENTITY_ITEM_WEAPON, 0,   8,  8,  8,   0},  /* WEAPON_GATLING */
    { "GKNIFE",       ENTITY_ITEM_WEAPON, 0,   7,  8,  8,   0},  /* WEAPON_KNIFE */

    /* Ammo/supply pickups */
    { "GDYNAM",       ENTITY_ITEM_AMMO,   0,   5,  6,  6,   0},
    { "GAMBELT",      ENTITY_ITEM_AMMO,   0,  20,  8,  6,   0},
    { "GAMBOXB",      ENTITY_ITEM_AMMO,   0,  20,  8,  6,   0},
    { "GAMBOXC",      ENTITY_ITEM_AMMO,   0,  20,  8,  6,   0},
    { "GAMBOXS",      ENTITY_ITEM_AMMO,   0,  10,  8,  6,   0},
    { "GOIL",         ENTITY_ITEM_AMMO,   0,   5,  6,  8,   0},

    /* Keys / badges / crowbar (door tools). Prefix-matched, so GSTEEKEY etc. */
    { "GBADGE",       ENTITY_ITEM_KEY,    0,   1,  6,  8,   0},
    { "GSTEEKEY",     ENTITY_ITEM_KEY,    0,   1,  6,  8,   0},
    { "GIRONKEY",     ENTITY_ITEM_KEY,    0,   1,  6,  8,   0},
    { "GBRSSKEY",     ENTITY_ITEM_KEY,    0,   1,  6,  8,   0},
    { "GRKEY",        ENTITY_ITEM_KEY,    0,   1,  6,  8,   0},
    { "GSQRKEY",      ENTITY_ITEM_KEY,    0,   1,  6,  8,   0},
    { "GCROWBAR",     ENTITY_ITEM_KEY,    0,   1,  6,  8,   0},
    { "GSHOVEL",      ENTITY_ITEM_KEY,    0,   1,  6,  8,   0},

    /* Decorations - rocks, vegetation, props */
    { "ROCK",         ENTITY_DECORATION,  0,   0,  8,  8,   0},
    { "TREE",         ENTITY_DECORATION,  0,   0, 12, 32,   0},
    { "SHRUB",        ENTITY_DECORATION,  0,   0,  8, 10,   0},
    { "PONDROSA",     ENTITY_DECORATION,  0,   0, 12, 28,   0},
    { "STUMP",        ENTITY_DECORATION,  0,   0,  8, 10,   0},
    { "LANTERN",      ENTITY_DECORATION,  0,   0,  4, 12,   0},
    { "CAMFIRE",      ENTITY_DECORATION,  0,   0,  8,  6,   0},
    { "FIRECIRC",     ENTITY_DECORATION,  0,   0,  8,  4,   0},
    { "BOTTLE",       ENTITY_DECORATION,  0,   0,  4,  8,   0},
    { "HOR1",         ENTITY_DECORATION,  0,   0, 10, 10,   0},
    { "POODLE",       ENTITY_DECORATION,  0,   0,  8, 10,   0},
    { "BARREL",       ENTITY_DECORATION,  0,   0,  8, 12,   0},
    { "CRATE",        ENTITY_DECORATION,  0,   0,  8, 10,   0},
    { "WAGON",        ENTITY_DECORATION,  0,   0, 16, 12,   0},
    { "COFFIN",       ENTITY_DECORATION,  0,   0, 10,  6,   0},
    { "GRAVE",        ENTITY_DECORATION,  0,   0,  8, 12,   0},
    { "TABLE",        ENTITY_DECORATION,  0,   0, 10,  8,   0},
    { "CHAIR",        ENTITY_DECORATION,  0,   0,  6,  8,   0},
    { "SIGN",         ENTITY_DECORATION,  0,   0, 10,  8,   0},
    { "BUBBL",        ENTITY_DECORATION,  0,   0,  6,  6,   0},

    /* Sound/effect emitters (ambient sounds, invisible) */
    { "SFIRE",        ENTITY_DECORATION,  0,   0,  0,  0,   0},
    { "SDRIP",        ENTITY_DECORATION,  0,   0,  0,  0,   0},
    { "SWATER",       ENTITY_DECORATION,  0,   0,  0,  0,   0},
    { "SUWATER",      ENTITY_DECORATION,  0,   0,  0,  0,   0},
    { "SWTUNL",       ENTITY_DECORATION,  0,   0,  0,  0,   0},
    { "SWIND",        ENTITY_DECORATION,  0,   0,  0,  0,   0},
    { "SVULTURE",     ENTITY_DECORATION,  0,   0,  0,  0,   0},

    /* Historical-mission named enemies (Civil War / Villa / Wharf rosters, from
     * their .obb object lists). Health approximate. E-prefixed variants and the
     * EBGY* soldiers are handled by the E-prefix fallback in find_def(). */
    { "CONFED",       ENTITY_ENEMY,        3,  0,  6,  7, 100},  /* confederate soldier */
    { "GATGUY",       ENTITY_ENEMY,        6,  0,  6,  8, 120},  /* gatling gunner */
    { "CROWE",        ENTITY_ENEMY,       10,  0,  7,  8, 130},
    { "DORSEY",       ENTITY_ENEMY,       10,  0,  7,  8, 130},
    { "KENNY",        ENTITY_ENEMY,        4,  0,  6,  7,  90},
    { "HARGROVE",     ENTITY_ENEMY,       12,  0,  7,  8, 130},

    /* Triggers */
    { "TRIGGER",      ENTITY_TRIGGER,     0,   0,  0,  0,   0},
    { "DOOR",         ENTITY_TRIGGER,     0,   0,  0,  0,   0},
};
static const int s_def_count = (int)(sizeof(s_defs)/sizeof(s_defs[0]));

/* Core lookup: exact match, then prefix match. Returns the def or NULL. */
static const EntityDef *lookup_def(const char *type_name) {
    if (!type_name || !type_name[0]) return NULL;
    for (int i = 0; i < s_def_count; i++)
        if (strcasecmp(s_defs[i].type_name, type_name) == 0) return &s_defs[i];
    for (int i = 0; i < s_def_count; i++) {
        size_t plen = strlen(s_defs[i].type_name);
        if (strncasecmp(s_defs[i].type_name, type_name, plen) == 0) return &s_defs[i];
    }
    return NULL;
}

static const EntityDef *find_def(const char *type_name) {
    const EntityDef *d = lookup_def(type_name);
    /* Historical missions prefix enemy types with 'E' (EBGY8, EBGY9WA2, ...).
     * If nothing matched, retry without the leading E so they resolve to their
     * base enemy def (only enemy types carry the E prefix; items are G-prefixed). */
    if (!d && type_name && (type_name[0] == 'E' || type_name[0] == 'e') && type_name[1])
        d = lookup_def(type_name + 1);
    return d;
}

EntityKind entity_classify(const char *type_name) {
    const EntityDef *d = find_def(type_name);
    return d ? d->kind : ENTITY_DECORATION;
}

/* -------------------------------------------------------------------------
 * Entity list API
 * ---------------------------------------------------------------------- */
void entity_list_init(EntityList *list) {
    memset(list, 0, sizeof(*list));
}

void entity_list_clear(EntityList *list) {
    memset(list, 0, sizeof(*list));
}

int entity_add(EntityList *list, const ObtObject *obj) {
    if (!list || !obj || list->count >= MAX_ENTITIES) return -1;

    /* Skip PLAYER (handled separately) */
    if (strcasecmp(obj->type, "PLAYER") == 0) return -1;

    const EntityDef *def = find_def(obj->type);
    EntityKind kind = def ? def->kind : ENTITY_DECORATION;

    Entity *e = &list->entities[list->count];
    memset(e, 0, sizeof(*e));

    e->active     = true;
    e->kind       = kind;
    e->pos        = obj->pos;
    e->yaw        = obj->yaw * OL_DEG2RAD;
    e->sprite_tex = 0;  /* Filled in later when WAX is loaded */
    e->drop_entity = -1; /* set by world_load for enemies with a DROP_ITEM */
    snprintf(e->type_name, ENTITY_NAME_LEN, "%s", obj->type);

    /* Story bosses. SANCHEZ (TOWN) is a special case: he appears at one of ~14
     * marked points once most of the town is cleared, so his candidates load
     * DORMANT and the mission system activates one at the kill quota. Other
     * bosses (SLIM = HIDEOUT, upstairs room) are present and ACTIVE from the
     * start; the mission system completes the level when they die. */
    if (strstr(obj->type, "SANCHEZ") || strstr(obj->type, "sanchez")) {
        e->is_boss = true;
        e->active  = false;   /* dormant until the mission triggers it */
    } else if (strncasecmp(obj->type, "SLIM", 4) == 0 ||
               strncasecmp(obj->type, "ESLIM", 5) == 0 ||
               strncasecmp(obj->type, "BOBGRAH", 7) == 0 ||
               strncasecmp(obj->type, "EBOBGRAH", 8) == 0 ||
               strncasecmp(obj->type, "BILLMOR", 7) == 0 ||
               strncasecmp(obj->type, "EBILLMOR", 8) == 0 ||
               strncasecmp(obj->type, "TIMBLOOD", 8) == 0 ||
               strncasecmp(obj->type, "ETIMBLOO", 8) == 0) {
        e->is_boss = true;    /* active from the start (kept active below) */
    }

    if (def) {
        e->health      = def->health;
        e->max_health  = def->health;
        e->pickup_value = def->pickup_value;
        e->sprite_w    = (def->sprite_w > 0) ? def->sprite_w : 8.0f;
        e->sprite_h    = (def->sprite_h > 0) ? def->sprite_h : 12.0f;
        e->alert_range = def->alert_range;
        /* Ground-lying flat sprites: render as horizontal quad on the floor */
        e->flat = (strncasecmp(obj->type, "FIRECIRC", 8) == 0 ||
                   strncasecmp(obj->type, "CAMFIRE",  7) == 0);
    } else {
        e->health     = 0;
        e->max_health = 0;
        e->sprite_w   = 8.0f;
        e->sprite_h   = 12.0f;
        e->alert_range = 0;
    }

    return (int)(list->count++);
}

/* -------------------------------------------------------------------------
 * AI update
 * ---------------------------------------------------------------------- */

/* Enemy walk/run speed — from ITM BGY1: WALK_SPEED=18, RUN_SPEED=22 */
#define ENEMY_WALK_SPEED  18.0f
#define ENEMY_RUN_SPEED   22.0f
/* Min distance to player before enemy stops advancing — from ITM KILL_DIST=45 */
#define ENEMY_STOP_DIST    4.0f
/* Attack range: must be within this to shoot */
#define ENEMY_ATTACK_DIST 120.0f

/* Simple enemy AI: idle → alert (walk toward player) → attack → pain → dead
 * Now with line-of-sight checks through sector portals so enemies don't
 * see/shoot through solid walls. */
static void update_enemy(Entity *e, Vec3 player_pos, f32 dt,
                          const LvtLevel *lvt,
                          void (*player_hurt_cb)(i32)) {
    if (!e->active || e->ai_state == AI_DEAD) return;

    f32 dx_to = player_pos.x - e->pos.x;
    f32 dz_to = player_pos.z - e->pos.z;
    f32 dist  = sqrtf(dx_to*dx_to + dz_to*dz_to);
    e->ai_timer -= dt;

    /* Line-of-sight check: can enemy see the player through sector portals?
     * Cached and rechecked ~7×/s (a full 3D sector trace is too costly to run
     * every frame for every enemy). */
    f32 eye_y = e->pos.y + e->sprite_h * 0.7f; /* enemy eye height */
    f32 player_eye_y = player_pos.y + 5.0f;     /* player eye height */
    e->los_timer -= dt;
    if (lvt == NULL) {
        e->los_cached = true;
    } else if (e->los_timer <= 0.0f) {
        e->los_cached = collision_has_los(lvt, e->pos.x, e->pos.z, eye_y,
                                          player_pos.x, player_pos.z, player_eye_y);
        e->los_timer = 0.15f;
    }
    bool can_see = e->los_cached;

    switch (e->ai_state) {
    case AI_IDLE:
        if (e->alert_range > 0 && dist < e->alert_range && can_see) {
            e->ai_state = AI_ALERT;
            e->ai_timer = 0.3f;
        }
        break;

    case AI_ALERT:
        /* Face and walk toward the player */
        if (can_see && dist > 0.1f) e->yaw = atan2f(dz_to, dx_to);
        if (can_see && dist > ENEMY_STOP_DIST) {
            f32 spd = ENEMY_WALK_SPEED * dt;
            e->pos.x += (dx_to / dist) * spd;
            e->pos.z += (dz_to / dist) * spd;
        }
        if (can_see && dist < ENEMY_ATTACK_DIST) {
            e->ai_state = AI_ATTACK;
            e->attack_timer = 0.5f + ((f32)(rand() % 100) / 100.0f);
        } else if (!can_see || dist > e->alert_range * 2.0f) {
            /* Lost sight or too far — go back to idle */
            if (!can_see) e->ai_state = AI_IDLE;
            else if (dist > e->alert_range * 2.0f) e->ai_state = AI_IDLE;
        }
        break;

    case AI_ATTACK:
        /* Face the player */
        if (can_see && dist > 0.1f) e->yaw = atan2f(dz_to, dx_to);

        /* Move to stay in attack range if player runs */
        if (can_see && dist > ENEMY_ATTACK_DIST && dist > ENEMY_STOP_DIST) {
            f32 spd = ENEMY_WALK_SPEED * dt;
            e->pos.x += (dx_to / dist) * spd;
            e->pos.z += (dz_to / dist) * spd;
        }

        e->attack_timer -= dt;
        if (e->attack_timer <= 0.0f) {
            /* Only deal damage if we can see the player */
            if (can_see && dist < ENEMY_ATTACK_DIST && player_hurt_cb) {
                i32 dmg = 5 + rand() % 10;
                player_hurt_cb(dmg);
            }
            e->attack_timer = 1.2f + ((f32)(rand() % 100) / 50.0f);
        }

        /* Lost sight → drop back to alert (search) */
        if (!can_see) {
            e->ai_state = AI_ALERT;
            e->ai_timer = 2.0f; /* search for 2 seconds before giving up */
        } else if (dist > e->alert_range * 3.0f) {
            e->ai_state = AI_IDLE;
        }
        break;

    case AI_PAIN:
        if (e->ai_timer <= 0.0f) {
            e->ai_state = (dist < ENEMY_ATTACK_DIST) ? AI_ATTACK : AI_ALERT;
        }
        break;

    case AI_DEAD:
        break;
    }
}

/* Drive per-AI-state animation: select sequence, advance timer, update frame */
static void update_enemy_animation(Entity *e, f32 dt) {
    if (!e->has_anim_seqs) return;

    AnimState target_anim;
    bool loop = true;

    switch (e->ai_state) {
    case AI_IDLE:   target_anim = ANIM_IDLE;   break;
    case AI_ALERT:  target_anim = ANIM_WALK;   break;
    case AI_ATTACK: target_anim = ANIM_ATTACK; break;
    case AI_PAIN:   target_anim = ANIM_PAIN;   loop = false; break;
    case AI_DEAD:
        /* Die animation plays once, then switches to dead (corpse) */
        if (e->cur_anim == ANIM_DIE) {
            EntityAnimSeq *die_seq = &e->anim_seqs[ANIM_DIE];
            if (die_seq->frame_count > 0 &&
                e->cur_anim_frame >= die_seq->frame_count - 1) {
                target_anim = ANIM_DEAD;
                loop = false;
            } else {
                target_anim = ANIM_DIE;
                loop = false;
            }
        } else if (e->cur_anim == ANIM_DEAD) {
            target_anim = ANIM_DEAD;
            loop = false;
        } else {
            target_anim = ANIM_DIE;
            loop = false;
        }
        break;
    default:
        target_anim = ANIM_IDLE;
        break;
    }

    /* If target sequence has no frames, fall back to idle */
    if (e->anim_seqs[target_anim].frame_count == 0) {
        target_anim = ANIM_IDLE;
        loop = true;
    }

    /* Switch animation state if changed */
    if (target_anim != e->cur_anim) {
        e->cur_anim = target_anim;
        e->cur_anim_frame = 0;
        e->cur_anim_timer = 0.0f;
        e->anim_loop = loop;
    }

    /* Advance animation timer */
    EntityAnimSeq *seq = &e->anim_seqs[e->cur_anim];
    if (seq->frame_count > 0) {
        e->cur_anim_timer += dt;
        f32 frame_sec = (f32)seq->dt_ms / 1000.0f;
        if (frame_sec < 0.033f) frame_sec = 0.1f;

        while (e->cur_anim_timer >= frame_sec) {
            e->cur_anim_timer -= frame_sec;
            if (e->cur_anim_frame + 1 < seq->frame_count) {
                e->cur_anim_frame++;
            } else if (e->anim_loop) {
                e->cur_anim_frame = 0;
            }
            /* else: stay on last frame (play-once finished) */
        }
    }
}

void entity_update_all(EntityList *list, Vec3 player_pos, f32 dt,
                        const LvtLevel *lvt,
                        void (*player_hurt_cb)(i32)) {
    for (u32 i = 0; i < list->count; i++) {
        Entity *e = &list->entities[i];
        if (!e->active) continue;
        if (e->kind == ENTITY_ENEMY) {
            update_enemy(e, player_pos, dt, lvt, player_hurt_cb);
            /* Snap enemy Y to the floor of the sector they occupy — Y-aware so a
             * building's stacked sectors resolve to the level the enemy is on
             * (their OBT spawn height), not the first 2D match (often the roof). */
            if (e->ai_state != AI_DEAD && lvt) {
                e->pos.y = entity_floor_at_y(lvt, e->pos.x, e->pos.z, e->pos.y);
            }
            /* Drive per-AI-state animation (runs for all states incl. dead) */
            update_enemy_animation(e, dt);
        }

        /* Scenery chor playback (Inv_Object state machine) */
        if (e->is_scenery && e->scn_count > 0) {
            scenery_update(e, dt);
        }
        /* Advance sprite animation timer for animated entities (non-scenery) */
        else if (e->anim_count > 1) {
            e->anim_timer += dt;
            f32 frame_sec = (f32)e->anim_dt_ms / 1000.0f;
            if (frame_sec < 0.033f) frame_sec = 0.1f; /* clamp to min 3fps */
            while (e->anim_timer >= frame_sec) {
                e->anim_timer -= frame_sec;
                e->anim_frame = (e->anim_frame + 1) % e->anim_count;
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Scenery (Inv_Object) chor state machine
 * Ghidra: Inv_Object@0x418da0 msg 120: a qualifying hit advances the wax to
 * the NEXT chor (WaxInst_SetState(cur+1), bounds-checked, no hit points).
 * ---------------------------------------------------------------------- */
static void (*g_scn_play_sfx)(i32 sounds_lst_idx, Vec3 pos) = NULL;

void entity_set_sfx_callback(void (*play)(i32 sounds_lst_idx, Vec3 pos)) {
    g_scn_play_sfx = play;
}

static void scenery_set_state(Entity *e, u32 state) {
    /* WaxInst_SetState bounds-checks against nChors: past-the-end = no-op. */
    if (state >= e->scn_count) return;
    e->scn_state   = state;
    e->scn_frame   = 0;
    e->scn_timer   = 0.0f;
    e->scn_playing = true;
    const ScnChor *ch = &e->scn[state];
    if (ch->sound_idx >= 0 && g_scn_play_sfx)
        g_scn_play_sfx(ch->sound_idx, e->pos);
}

/* Advance the current chor. Frame words display at the chor rate; the
 * terminal opcode decides what happens at the end of the stream. */
static void scenery_update(Entity *e, f32 dt) {
    const ScnChor *ch = &e->scn[e->scn_state];
    if (!e->scn_playing || ch->nframes == 0) return;

    e->scn_timer += dt;
    f32 frame_sec = (ch->dt_ms > 0) ? (f32)ch->dt_ms / 1000.0f : 0.1f;
    while (e->scn_timer >= frame_sec && e->scn_playing) {
        e->scn_timer -= frame_sec;
        if (e->scn_frame + 1 < ch->nframes) {
            e->scn_frame++;
            continue;
        }
        /* End of stream: terminal opcode */
        switch (ch->end) {
        case SCN_END_LOOP:
            e->scn_frame = 0;
            break;
        case SCN_END_TERMINATE:
            /* 0xFFF8: logic frees the actor (bottle shatters away) */
            e->active = false;
            e->scn_playing = false;
            break;
        case SCN_END_SETSTATE:
            scenery_set_state(e, ch->end_state);
            return;
        case SCN_END_STOP:
        default:
            /* Hold the last frame forever (debris stays) */
            e->scn_playing = false;
            break;
        }
    }
}

/* Advance to the next chor (damage/nudge reaction). */
static void scenery_advance(Entity *e) {
    scenery_set_state(e, e->scn_state + 1);
}

int entity_nudge(EntityList *list, Vec3 origin, Vec3 dir, f32 reach) {
    /* Message 0x7D3 semantics (Actor_NudgeTrace@0x446cc0): USE/concussion
     * reaches nearby scenery; both SHOOT (0x2000) and NUDGE (0x1000) types
     * react. dir selects a frontal cone when non-zero. */
    int hits = 0;
    bool has_dir = (dir.x*dir.x + dir.y*dir.y + dir.z*dir.z) > 1e-6f;
    for (u32 i = 0; i < list->count; i++) {
        Entity *e = &list->entities[i];
        if (!e->active || !e->is_scenery) continue;
        if (e->scenery_type == SCENERY_PASS) continue;
        Vec3 to = vec3_sub(e->pos, origin);
        f32 d = vec3_len(to);
        if (d > reach) continue;
        if (has_dir && d > 1e-3f) {
            f32 dot = (to.x*dir.x + to.y*dir.y + to.z*dir.z) / d;
            if (dot < 0.5f) continue;  /* outside frontal cone */
        }
        scenery_advance(e);
        hits++;
    }
    return hits;
}

/* -------------------------------------------------------------------------
 * Item pickup
 * ---------------------------------------------------------------------- */
i32 entity_try_pickup(EntityList *list, Vec3 player_pos) {
    /* Legacy helper: no health-cap gate (max_hp huge so health always picks up). */
    PickupResult r = entity_try_pickup_ex(list, player_pos, 0, 0x7fffffff);
    return r.got ? r.value : 0;
}

PickupResult entity_try_pickup_ex(EntityList *list, Vec3 player_pos,
                                  i32 hp, i32 max_hp) {
    PickupResult r = {0};
    const f32 PICKUP_RANGE = 12.0f;
    for (u32 i = 0; i < list->count; i++) {
        Entity *e = &list->entities[i];
        if (!e->active) continue;
        if (e->kind != ENTITY_ITEM_HEALTH &&
            e->kind != ENTITY_ITEM_AMMO &&
            e->kind != ENTITY_ITEM_KEY &&
            e->kind != ENTITY_ITEM_WEAPON) continue;
        /* Don't consume a health pickup when already at full health. */
        if (e->kind == ENTITY_ITEM_HEALTH && hp >= max_hp) continue;
        if (vec3_dist(e->pos, player_pos) < PICKUP_RANGE) {
            e->active = false;
            r.got   = true;
            r.kind  = e->kind;
            r.value = e->pickup_value;
            snprintf(r.type_name, sizeof(r.type_name), "%s", e->type_name);
            return r;
        }
    }
    return r;
}

/* -------------------------------------------------------------------------
 * Raycast against entity bounding cylinders
 * ---------------------------------------------------------------------- */
int entity_raycast(const EntityList *list, const LvtLevel *level,
                   Vec3 origin, Vec3 dir,
                   f32 max_dist, f32 *hit_dist) {
    int   best_idx  = -1;
    f32   best_dist = max_dist;

    for (u32 i = 0; i < list->count; i++) {
        const Entity *e = &list->entities[i];
        if (!e->active) continue;
        /* Ray-solid targets: live enemies, and scenery with a TYPE field
         * (actor flag 0x1000/0x2000 in the original — flag 0x400 scenery
         * without TYPE lets rays pass, Ghidra filter @0x4e54c0). */
        if (e->kind == ENTITY_ENEMY) {
            if (e->ai_state == AI_DEAD) continue;
        } else if (e->is_scenery) {
            if (e->scenery_type == SCENERY_PASS) continue;
        } else {
            continue;
        }

        /* Ray vs vertical cylinder (radius = sprite_w/2, height = sprite_h) */
        f32 rx = origin.x - e->pos.x;
        f32 rz = origin.z - e->pos.z;
        f32 dx = dir.x, dz = dir.z;
        f32 r  = e->sprite_w * 0.5f;

        f32 a = dx*dx + dz*dz;
        f32 b = 2.0f*(rx*dx + rz*dz);
        f32 c = rx*rx + rz*rz - r*r;
        f32 disc = b*b - 4.0f*a*c;

        if (disc < 0.0f || a < 1e-6f) continue;
        f32 t = (-b - sqrtf(disc)) / (2.0f*a);
        if (t < 0.0f || t > best_dist) continue;

        /* Check Y range */
        f32 hit_y = origin.y + dir.y * t;
        if (hit_y < e->pos.y || hit_y > e->pos.y + e->sprite_h) continue;

        best_dist = t;
        best_idx  = (int)i;
    }

    /* Line-of-sight gate: the shot is blocked by any solid wall between the
     * shooter and the target — no more killing enemies through walls. LOS honors
     * portal openings (windows/doorways) so you CAN shoot through those. */
    if (best_idx >= 0 && level) {
        const Entity *e = &list->entities[best_idx];
        f32 ty = e->pos.y + e->sprite_h * 0.5f;
        if (!collision_has_los(level, origin.x, origin.z, origin.y,
                               e->pos.x, e->pos.z, ty)) {
            best_idx = -1;
            best_dist = max_dist;
        }
    }

    if (hit_dist) *hit_dist = best_dist;
    return best_idx;
}

/* -------------------------------------------------------------------------
 * Damage
 * ---------------------------------------------------------------------- */
bool entity_damage(EntityList *list, int idx, i32 amount) {
    if (idx < 0 || idx >= (i32)list->count) return false;
    Entity *e = &list->entities[idx];
    if (!e->active) return false;

    /* SHOOT scenery: a single qualifying hit advances the chor state — no
     * hit points (Ghidra Inv_Object@0x418da0 msg 0x7D1 path). NUDGE-type
     * scenery is ray-solid but does NOT react to bullets. */
    if (e->is_scenery) {
        if (e->scenery_type == SCENERY_SHOOT)
            scenery_advance(e);
        return false;
    }

    if (e->kind != ENTITY_ENEMY) return false;
    if (e->ai_state == AI_DEAD) return false;

    e->health -= amount;
    if (e->health <= 0) {
        e->health    = 0;
        e->ai_state  = AI_DEAD;
        e->ai_timer  = 3.0f;  /* Stay as corpse for 3 seconds */
        /* Loot drop (Outlaws ITM DROP_ITEM → Inv_GroundObject): activate the
         * pre-spawned ammo-belt pickup at the corpse. */
        if (e->drop_entity >= 0 && e->drop_entity < (i32)list->count) {
            Entity *d = &list->entities[e->drop_entity];
            d->pos    = e->pos;
            d->active = true;
        }
        return true;
    }
    /* Non-fatal hit: play the full HIT flinch (chor 1). Hold the pain state for
     * the WHOLE hit-chor duration (frame_count × dt_ms) so every flinch frame is
     * seen — a fixed 0.3s cut the 4-frame/800ms reaction off after one frame,
     * which read as "no hit animation". Restart the flinch from frame 0 on each
     * new hit so rapid fire keeps staggering the enemy. */
    e->ai_state = AI_PAIN;
    f32 pain_dur = 0.3f;
    const EntityAnimSeq *ps = &e->anim_seqs[ANIM_PAIN];
    if (ps->frame_count > 0 && ps->dt_ms > 0)
        pain_dur = (f32)ps->frame_count * (f32)ps->dt_ms / 1000.0f;
    e->ai_timer = pain_dur;
    e->cur_anim = ANIM_PAIN;
    e->cur_anim_frame = 0;
    e->cur_anim_timer = 0.0f;
    e->anim_loop = false;
    return false;
}
