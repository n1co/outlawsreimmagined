/*
 * entity.c - Game entity system (enemies, items, decorations)
 */
#include "entity.h"
#include "collision.h"
#include <math.h>
#include <string.h>

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

static f32 entity_floor_at(const LvtLevel *lvt, f32 x, f32 z) {
    if (!lvt) return 0.0f;
    for (u32 si = 0; si < lvt->sector_count; si++) {
        if (entity_point_in_sector(&lvt->sectors[si], x, z))
            return lvt->sectors[si].floor_y;
    }
    return 0.0f;
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

    /* Health pickups */
    { "GDOCBAG",      ENTITY_ITEM_HEALTH, 0,  50,  8,  8,   0},
    { "GELIXIR",      ENTITY_ITEM_HEALTH, 0,  25,  6,  8,   0},
    { "GCANTEEN",     ENTITY_ITEM_HEALTH, 0,  20,  6,  8,   0},

    /* Weapon pickups */
    { "GSGUN",        ENTITY_ITEM_WEAPON, 0,   1,  8,  8,   0},
    { "GSAWGUN",      ENTITY_ITEM_WEAPON, 0,   2,  8,  8,   0},
    { "GSCOPE",       ENTITY_ITEM_WEAPON, 0,   3,  8,  8,   0},

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

    /* Triggers */
    { "TRIGGER",      ENTITY_TRIGGER,     0,   0,  0,  0,   0},
    { "DOOR",         ENTITY_TRIGGER,     0,   0,  0,  0,   0},
};
static const int s_def_count = (int)(sizeof(s_defs)/sizeof(s_defs[0]));

EntityKind entity_classify(const char *type_name) {
    if (!type_name || !type_name[0]) return ENTITY_DECORATION;
    for (int i = 0; i < s_def_count; i++) {
        if (strcasecmp(s_defs[i].type_name, type_name) == 0)
            return s_defs[i].kind;
    }
    /* Prefix match for variants */
    for (int i = 0; i < s_def_count; i++) {
        size_t plen = strlen(s_defs[i].type_name);
        if (strncasecmp(s_defs[i].type_name, type_name, plen) == 0)
            return s_defs[i].kind;
    }
    return ENTITY_DECORATION;
}

static const EntityDef *find_def(const char *type_name) {
    if (!type_name) return NULL;
    for (int i = 0; i < s_def_count; i++) {
        if (strcasecmp(s_defs[i].type_name, type_name) == 0)
            return &s_defs[i];
    }
    for (int i = 0; i < s_def_count; i++) {
        size_t plen = strlen(s_defs[i].type_name);
        if (strncasecmp(s_defs[i].type_name, type_name, plen) == 0)
            return &s_defs[i];
    }
    return NULL;
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
               strncasecmp(obj->type, "ESLIM", 5) == 0) {
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

    /* Line-of-sight check: can enemy see the player through sector portals? */
    f32 eye_y = e->pos.y + e->sprite_h * 0.7f; /* enemy eye height */
    f32 player_eye_y = player_pos.y + 5.0f;     /* player eye height */
    bool can_see = (lvt != NULL)
        ? collision_has_los(lvt, e->pos.x, e->pos.z, eye_y,
                            player_pos.x, player_pos.z, player_eye_y)
        : true;

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
            /* Snap enemy Y to floor of the sector they occupy */
            if (e->ai_state != AI_DEAD && lvt) {
                f32 floor_y = entity_floor_at(lvt, e->pos.x, e->pos.z);
                e->pos.y = floor_y;
            }
            /* Drive per-AI-state animation (runs for all states incl. dead) */
            update_enemy_animation(e, dt);
        }

        /* Advance sprite animation timer for animated entities */
        if (e->anim_count > 1) {
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
 * Item pickup
 * ---------------------------------------------------------------------- */
i32 entity_try_pickup(EntityList *list, Vec3 player_pos) {
    PickupResult r = entity_try_pickup_ex(list, player_pos);
    return r.got ? r.value : 0;
}

PickupResult entity_try_pickup_ex(EntityList *list, Vec3 player_pos) {
    PickupResult r = {0};
    const f32 PICKUP_RANGE = 12.0f;
    for (u32 i = 0; i < list->count; i++) {
        Entity *e = &list->entities[i];
        if (!e->active) continue;
        if (e->kind != ENTITY_ITEM_HEALTH &&
            e->kind != ENTITY_ITEM_AMMO &&
            e->kind != ENTITY_ITEM_KEY &&
            e->kind != ENTITY_ITEM_WEAPON) continue;
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
int entity_raycast(const EntityList *list,
                   Vec3 origin, Vec3 dir,
                   f32 max_dist, f32 *hit_dist) {
    int   best_idx  = -1;
    f32   best_dist = max_dist;

    for (u32 i = 0; i < list->count; i++) {
        const Entity *e = &list->entities[i];
        if (!e->active || e->kind != ENTITY_ENEMY) continue;
        if (e->ai_state == AI_DEAD) continue;

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

    if (hit_dist) *hit_dist = best_dist;
    return best_idx;
}

/* -------------------------------------------------------------------------
 * Damage
 * ---------------------------------------------------------------------- */
bool entity_damage(EntityList *list, int idx, i32 amount) {
    if (idx < 0 || idx >= (i32)list->count) return false;
    Entity *e = &list->entities[idx];
    if (!e->active || e->kind != ENTITY_ENEMY) return false;
    if (e->ai_state == AI_DEAD) return false;

    e->health -= amount;
    if (e->health <= 0) {
        e->health    = 0;
        e->ai_state  = AI_DEAD;
        e->ai_timer  = 3.0f;  /* Stay as corpse for 3 seconds */
        /* After that, deactivate (handled in update, but simple: just mark) */
        return true;
    }
    e->ai_state = AI_PAIN;
    e->ai_timer = 0.3f;
    if (e->ai_state == AI_IDLE) e->ai_state = AI_ATTACK;
    return false;
}
