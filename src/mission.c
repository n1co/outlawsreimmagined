/*
 * mission.c - Boss / objective logic (see mission.h)
 */
#include "mission.h"
#include <string.h>
#include <stdlib.h>

/* Fraction of the regular enemies that must be killed before the boss appears
 * ("cleaned out most buildings — not necessary to clear every one"). */
#define MISSION_BOSS_QUOTA 0.60f
/* Don't spawn the boss right on top of the player. */
#define MISSION_BOSS_MIN_DIST 30.0f

static bool is_regular_enemy(const Entity *e) {
    return e->kind == ENTITY_ENEMY && !e->is_boss;
}

/* Nice display name from an OBT boss type (e.g. "ESLIM3" → "SLIM"). */
static void boss_display_name(const char *type, char *out, int cap) {
    if (strncasecmp(type, "SANCHEZ", 7) == 0) { snprintf(out, cap, "SANCHEZ"); return; }
    if (strncasecmp(type, "ESLIM", 5) == 0 || strncasecmp(type, "SLIM", 4) == 0) {
        snprintf(out, cap, "SLIM"); return;
    }
    if (strncasecmp(type, "JAMES", 5) == 0)   { snprintf(out, cap, "JAMES ANDERSON"); return; }
    if (strncasecmp(type, "TWOFEATH", 8) == 0){ snprintf(out, cap, "TWO FEATHERS"); return; }
    if (strncasecmp(type, "MARY", 4) == 0)    { snprintf(out, cap, "BLOODY MARY"); return; }
    if (strncasecmp(type, "EBOBGRAH", 8) == 0 || strncasecmp(type, "BOBGRAH", 7) == 0) {
        snprintf(out, cap, "BOB GRAHAM"); return;
    }
    if (strncasecmp(type, "EBILLMOR", 8) == 0 || strncasecmp(type, "BILLMOR", 7) == 0) {
        snprintf(out, cap, "BILL MORGAN"); return;
    }
    snprintf(out, cap, "THE OUTLAW");
}

void mission_init(MissionState *m, const EntityList *entities) {
    memset(m, 0, sizeof(*m));
    m->boss_idx = -1;
    snprintf(m->boss_name, sizeof(m->boss_name), "THE OUTLAW");
    i32 candidates = 0, active_boss_idx = -1;
    char first_boss[32] = {0};
    bool multiple_names = false;
    for (u32 i = 0; i < entities->count; i++) {
        const Entity *e = &entities->entities[i];
        if (e->is_boss) {
            candidates++;
            char nm[32];
            boss_display_name(e->type_name, nm, sizeof(nm));
            if (!first_boss[0]) snprintf(first_boss, sizeof(first_boss), "%s", nm);
            else if (strcmp(first_boss, nm) != 0) multiple_names = true;
            snprintf(m->boss_name, sizeof(m->boss_name), "%s", nm);
            if (e->active && active_boss_idx < 0) active_boss_idx = (i32)i;
        } else if (is_regular_enemy(e) && e->active) {
            m->total_enemies++;
        }
    }
    /* Several distinct bosses in one level (e.g. RANCH: Bob Graham + Bill
     * Morgan) → a collective objective rather than naming just one. */
    if (multiple_names)
        snprintf(m->boss_name, sizeof(m->boss_name), "THE GANG");
    m->has_boss = (candidates > 0);
    if (!m->has_boss) return;

    if (active_boss_idx >= 0) {
        /* Boss present + active from the start (Slim). Track it directly. */
        m->active_boss  = true;
        m->boss_spawned = true;
        m->boss_idx     = active_boss_idx;
        snprintf(m->objective, sizeof(m->objective), "DEFEAT %s", m->boss_name);
    } else {
        /* Dormant-spawn boss (Sanchez) — appears after the kill quota. */
        snprintf(m->objective, sizeof(m->objective), "CLEAR OUT THE TOWN");
    }
}

/* Count regular enemies still alive (a corpse has ai_state == AI_DEAD). */
static i32 alive_regular_enemies(const EntityList *entities) {
    i32 n = 0;
    for (u32 i = 0; i < entities->count; i++) {
        const Entity *e = &entities->entities[i];
        if (is_regular_enemy(e) && e->active && e->ai_state != AI_DEAD) n++;
    }
    return n;
}

/* Activate the dormant boss candidate nearest the player (but not closer than
 * MISSION_BOSS_MIN_DIST); returns its index or -1. */
static i32 spawn_boss(EntityList *entities, Vec3 player_pos) {
    i32 best = -1, nearest = -1;
    f32 best_d2 = 1e30f, near_d2 = 1e30f;
    f32 minr2 = MISSION_BOSS_MIN_DIST * MISSION_BOSS_MIN_DIST;
    for (u32 i = 0; i < entities->count; i++) {
        Entity *e = &entities->entities[i];
        if (!e->is_boss || e->active) continue;
        f32 dx = e->pos.x - player_pos.x, dz = e->pos.z - player_pos.z;
        f32 d2 = dx*dx + dz*dz;
        if (d2 < near_d2) { near_d2 = d2; nearest = (i32)i; }
        if (d2 >= minr2 && d2 < best_d2) { best_d2 = d2; best = (i32)i; }
    }
    i32 idx = (best >= 0) ? best : nearest;
    if (idx < 0) return -1;
    Entity *b = &entities->entities[idx];
    b->active   = true;
    b->health   = b->max_health > 0 ? b->max_health : 8;
    b->ai_state = AI_ALERT;      /* immediately hunts the player */
    b->ai_timer = 0.0f;
    return idx;
}

void mission_update(MissionState *m, EntityList *entities, Vec3 player_pos,
                    const char **out_message, bool *out_complete) {
    if (!m->has_boss || m->complete) return;

    /* Active-boss levels (Slim/HIDEOUT): the boss is already out; the level
     * completes when every boss entity is dead. */
    if (m->active_boss) {
        bool any_alive = false;
        for (u32 i = 0; i < entities->count; i++) {
            const Entity *e = &entities->entities[i];
            if (e->is_boss && e->active && e->ai_state != AI_DEAD) any_alive = true;
        }
        if (!any_alive) {
            char msg[96];
            m->boss_dead = m->complete = true;
            snprintf(m->objective, sizeof(m->objective), "%s DEFEATED", m->boss_name);
            snprintf(msg, sizeof(msg), "%s DEFEATED - LEVEL COMPLETE", m->boss_name);
            if (out_message)  *out_message  = "BOSS DEFEATED - LEVEL COMPLETE";
            if (out_complete) *out_complete = true;
        }
        return;
    }

    if (!m->boss_spawned) {
        i32 alive = alive_regular_enemies(entities);
        i32 killed = m->total_enemies - alive;
        i32 quota  = (i32)(m->total_enemies * MISSION_BOSS_QUOTA);
        if (getenv("OL_BOSS_NOW")) quota = 0;   /* debug: spawn boss immediately */
        if (m->total_enemies > 0 && killed >= quota) {
            m->boss_idx = spawn_boss(entities, player_pos);
            if (m->boss_idx >= 0) {
                m->boss_spawned = true;
                snprintf(m->objective, sizeof(m->objective), "KILL SANCHEZ");
                if (out_message) *out_message = "SPITTIN' JACK SANCHEZ APPEARS!";
            }
        }
        return;
    }

    /* Boss is out — has he been killed? */
    if (m->boss_idx >= 0 && m->boss_idx < (i32)entities->count) {
        const Entity *b = &entities->entities[m->boss_idx];
        if (!b->active || b->ai_state == AI_DEAD) {
            m->boss_dead = true;
            m->complete  = true;
            snprintf(m->objective, sizeof(m->objective), "SANCHEZ DEFEATED");
            if (out_message)  *out_message  = "SANCHEZ DEFEATED - LEVEL COMPLETE";
            if (out_complete) *out_complete = true;
        }
    }
}
