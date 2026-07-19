/*
 * mission.h - Per-level mission / objective logic
 *
 * Outlaws story levels are gated by a boss objective. In TOWN (Sanctuary) the
 * outlaw "Spittin' Jack Sanchez" is placed as ~14 dormant spawn candidates; he
 * appears at ONE of them once the player has cleared out most of the town, and
 * killing him completes the level.
 */
#pragma once

#include "engine.h"
#include "entity.h"

typedef struct {
    bool  has_boss;        /* level has a boss objective */
    bool  active_boss;     /* boss is present+active from the start (e.g. Slim in
                            * HIDEOUT); false = dormant-spawn boss (Sanchez/TOWN) */
    i32   total_enemies;   /* regular (non-boss) enemies present at load */
    bool  boss_spawned;    /* the boss has been activated */
    i32   boss_idx;        /* entity index of the active boss (-1 = none) */
    bool  boss_dead;
    bool  complete;        /* level objective met */
    char  boss_name[32];   /* display name for the objective/banner */
    char  objective[96];   /* current objective text for the HUD */
} MissionState;

/* Scan the loaded entities: count regular enemies, note boss candidates. */
void mission_init(MissionState *m, const EntityList *entities);

/*
 * Advance the mission one frame. May activate the boss (spawning it at the
 * candidate nearest the player) and detect its death. When a notable event
 * happens this frame, *out_message is set to a banner string (else left alone);
 * *out_complete is set true the frame the level is completed.
 */
void mission_update(MissionState *m, EntityList *entities, Vec3 player_pos,
                    const char **out_message, bool *out_complete);
