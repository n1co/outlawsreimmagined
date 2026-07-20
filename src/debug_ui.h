/*
 * debug_ui.h - ImGui debug overlay (C interface)
 *
 * Toggle with INSERT key. Provides:
 *   - Position/sector info
 *   - Wireframe toggle
 *   - Noclip mode
 *   - God mode
 *   - Give all weapons
 *   - Sector/entity inspector
 */
#pragma once

#include <SDL2/SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Debug state accessible from C code */
typedef struct {
    bool visible;       /* INSERT toggles */
    bool wireframe;
    bool noclip;
    bool godmode;
    bool give_all_weapons;  /* one-shot trigger */
    int  give_items;        /* one-shot: bitmask OR'd into player.keys (bit = 1<<InfKeyType) */
    bool show_sector_info;
    bool show_entity_info;
    bool show_perf;

    /* Read-only info filled by game loop */
    float player_x, player_y, player_z;
    float player_yaw, player_pitch;
    int   player_sector;
    int   player_health;
    int   player_keys;      /* inventory bitmask (read-only, for display) */
    int   sector_count;
    int   entity_count;
    int   draw_calls;
    float fps;
    float dt;

    /* Level / mission status (read-only, filled by the game loop) */
    char  map_name[64];        /* current level file name */
    int   campaign_active;     /* 1 = story campaign */
    int   campaign_mission;    /* 1-based mission number */
    int   campaign_total;      /* total missions */
    char  objective[96];       /* current objective text */
    int   has_boss;
    int   boss_spawned;
    int   boss_dead;
    int   mission_complete;
    int   enemies_total;       /* enemies at load */
    int   enemies_alive;       /* enemies still active */
    int   items_alive;         /* pickups still in the level */
    int   difficulty;          /* 1..4 */

    /* Player movement detail (read-only) */
    float player_vx, player_vy, player_vz;
    int   on_ground;
    int   crouching;
    float eye_height;
    float sector_floor;        /* current sector floor/ceil at player */
    float sector_ceil;

    /* Weapon (read-only) */
    int   weapon_idx;
    int   weapon_clip, weapon_reserve;

    /* "Looking at" — filled each frame by the game loop from a view raycast */
    char  look_desc[192];      /* human description of what the crosshair hits */
    float look_dist;           /* distance to the hit (world units) */
    int   look_sector;         /* sector the ray ends in (-1 none) */
    int   look_is_door;        /* 1 = a door/portal, 0 = solid/entity/none */
    int   look_enemy;          /* enemy entity index under the crosshair, -1 none */

    /* FPS history ring buffer for the graph */
    float fps_hist[128];
    int   fps_hist_count;      /* number of valid samples (grows to 128) */
    int   fps_hist_head;       /* next write index */

    /* Requests back to the game (one-shot) */
    int   req_reload_level;    /* debug: reload current level */
    int   req_set_difficulty;  /* 0 = none, else 1..4 to apply+reload */
} DebugState;

/* Push one FPS sample into the ring buffer (call each frame). */
void debug_ui_push_fps(float fps);

/* Global debug state */
extern DebugState g_debug;

/* Init ImGui context (call after SDL/GL init) */
void debug_ui_init(SDL_Window *window, SDL_GLContext gl_ctx);

/* Shutdown ImGui */
void debug_ui_shutdown(void);

/* Process SDL event (call for each SDL event) */
void debug_ui_process_event(SDL_Event *event);

/* True when the debug UI is capturing the mouse (cursor over an ImGui window). */
bool debug_ui_wants_mouse(void);

/* Render the debug UI (call after all game rendering, before swap) */
void debug_ui_render(void);

#ifdef __cplusplus
}
#endif
