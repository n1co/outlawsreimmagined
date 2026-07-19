/*
 * menu.h - Front-end menu system (main menu, mission-select posters, options)
 *
 * Reproduces the Outlaws front end: a torn-parchment main menu (nmm.pcx), the
 * "wanted poster" mission picker (Pickscrn.pcx — six gang bosses, one per
 * mission), and an options screen (volumes.PCX). Keyboard + mouse driven.
 */
#pragma once

#include "engine.h"
#include "renderer.h"
#include "input.h"
#include "audio.h"
#include "laf.h"
#include "savegame.h"   /* SAVE_SLOTS */

typedef enum {
    MENU_MAIN = 0,
    MENU_MISSION,    /* Story: wanted-poster mission select (debug only now) */
    MENU_MP,         /* Multiplayer map select */
    MENU_OPTIONS,
    MENU_INGAME,     /* menu dismissed — the game is running */
    MENU_PAUSE,      /* in-game pause overlay */
    MENU_SAVE,       /* pause: save-game slot list */
    MENU_LOAD,       /* pause: load-game slot list */
} MenuScreen;

struct Archives;  /* fwd (defined in world.h) */

typedef struct {
    MenuScreen screen;
    int   sel;                 /* highlighted item / poster */
    u32   tex_main, tex_pick, tex_opts;
    bool  assets_loaded;
    bool  prev_click;          /* mouse button edge tracking */

    /* Options */
    int   music_vol;           /* 0..10 */
    int   sfx_vol;             /* 0..10 */

    /* Outputs consumed by the app */
    bool  want_quit;
    char  start_level[64];     /* non-empty → load this level and enter game */
    bool  start_story;         /* STORY chosen → begin the campaign at level 1 */

    /* Pause-menu outputs (consumed + cleared by the app each frame). */
    bool  want_resume;         /* RESUME / ESC → unpause */
    bool  want_quit_menu;      /* QUIT TO MENU → drop the game, show front end */
    int   req_save_slot;       /* >=0 → write this slot */
    int   req_load_slot;       /* >=0 → load this slot */

    /* Save/Load slot metadata, filled by the app before showing those screens. */
    bool  slot_used[SAVE_SLOTS];
    char  slot_label[SAVE_SLOTS][40];

    MenuScreen return_screen;  /* where OPTIONS returns to (main vs pause) */

    /* Menu click sound (0 = none) */
    u32   sfx_click;

    /* Authentic Outlaws fonts (mf3s.laf = big, sf3.laf = small). */
    LafFont font_big;
    LafFont font_small;
} Menu;

/* Open the in-game pause overlay (call when the player hits ESC). */
void menu_pause_open(Menu *m);

void menu_init(Menu *m);
void menu_load_assets(Menu *m, struct Archives *arc, Renderer *r, u32 sfx_click);

/*
 * Process + render one menu frame. Reads input, updates selection, draws the
 * current screen. Sets m->start_level (mission chosen) or m->want_quit. The app
 * checks those after calling this; on a chosen mission it loads the level and
 * sets m->screen = MENU_INGAME.
 */
void menu_frame(Menu *m, InputState *in, Renderer *r, AudioSystem *au);
