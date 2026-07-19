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
} DebugState;

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
