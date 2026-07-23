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
    MENU_OPTIONS,    /* options hub (VOLUME / CONTROLS / VIDEO) */
    MENU_INGAME,     /* menu dismissed — the game is running */
    MENU_PAUSE,      /* in-game pause overlay */
    MENU_SAVE,       /* pause: save-game slot list */
    MENU_LOAD,       /* save-game slot list (from pause OR main menu) */
    MENU_HISTORICAL, /* standalone "Historical Missions" list */
    MENU_DIFFICULTY, /* GOOD/BAD/UGLY skill pick before a launch */
    MENU_OPT_VOLUME, /* options: 5 volume channels */
    MENU_OPT_CONTROLS,/* options: key bindings + mouse */
    MENU_OPT_VIDEO,  /* options: resolution + fullscreen */
} MenuScreen;

struct Archives;  /* fwd (defined in world.h) */

typedef struct {
    MenuScreen screen;
    int   sel;                 /* highlighted item / poster */
    u32   tex_main, tex_pick, tex_opts;
    u32   tex_diff;            /* difficulty screen bg (md220c: cave, parchment right) */
    u32   tex_choose;          /* choose-game bg (MC220: mesas, parchment right) */
    u32   tex_hand;            /* pointing-hand cursor (GCURSOR cell 0) */
    f32   hand_w, hand_h;      /* hand cell aspect (world px) */
    bool  assets_loaded;
    bool  prev_click;          /* mouse button edge tracking */

    /* Options */
    int   music_vol;           /* 0..10 */
    int   sfx_vol;             /* 0..10 */

    /* Outputs consumed by the app */
    bool  want_quit;
    char  start_level[64];     /* non-empty → load this level and enter game */
    bool  start_story;         /* STORY chosen → begin the campaign at level 1 */
    bool  start_historical;    /* Historical mission → run its .rca campaign */
    int   chosen_difficulty;   /* skill (1..3) chosen for this launch */

    /* Deferred launch: a game type was picked but waits on the difficulty screen. */
    int   pending_kind;        /* 0=none, 1=story, 2=single level */
    char  pending_level[64];
    MenuScreen diff_return;     /* where CANCEL on the difficulty screen returns */

    /* Options / settings edit state */
    int   rebind_action;       /* BIND_* awaiting a key, or -1 */
    int   ctrl_scroll;         /* controls list scroll offset */
    bool  apply_video;         /* → app: apply g_settings win_w/h/fullscreen */
    bool  settings_dirty;      /* → app: persist settings (outlaws.cfg) */
    MenuScreen load_return;     /* where LOAD returns / who invoked it */

    /* Pause-menu outputs (consumed + cleared by the app each frame). */
    bool  want_resume;         /* RESUME / ESC → unpause */
    bool  want_quit_menu;      /* QUIT TO MENU → drop the game, show front end */
    int   req_save_slot;       /* >=0 → write this slot */
    int   req_load_slot;       /* >=0 → load this slot */

    /* Save/Load slot metadata, filled by the app before showing those screens. */
    bool  slot_used[SAVE_SLOTS];
    char  slot_label[SAVE_SLOTS][40];

    MenuScreen return_screen;  /* where OPTIONS returns to (main vs pause) */

    /* Menu sounds (0 = none): navigate blip, activate/select, back. */
    u32   sfx_click;
    u32   sfx_nav;
    u32   sfx_select;
    u32   sfx_back;

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
