/*
 * settings.h - Persistent user settings (video, audio, controls)
 *
 * Loaded from / saved to outlaws.cfg in the game directory (a simple key=value
 * text file). Holds the resolution, the five Outlaws volume channels
 * (Master/Movie/Effects/Taunts/Music, per LOCAL.MSG 5109-5113), the skill level
 * (GOOD/BAD/UGLY), and the rebindable key table. A single global g_settings is
 * read directly by the input code and the menu.
 */
#pragma once

#include "engine.h"
#include <SDL2/SDL_scancode.h>

/* Rebindable game actions. The default scancode for each is set in
 * settings_defaults() to the keys the engine originally hard-coded. */
typedef enum {
    BIND_FORWARD = 0,
    BIND_BACK,
    BIND_STRAFE_L,
    BIND_STRAFE_R,
    BIND_TURN_L,
    BIND_TURN_R,
    BIND_LOOK_UP,
    BIND_LOOK_DOWN,
    BIND_RUN,
    BIND_CROUCH,
    BIND_JUMP,
    BIND_USE,
    BIND_RELOAD,
    BIND_MAP,
    BIND_SCOPE,
    BIND_NEXT_WEAPON,
    BIND_PREV_WEAPON,
    BIND_WEAPON_1,
    BIND_WEAPON_2,
    BIND_WEAPON_3,
    BIND_WEAPON_4,
    BIND_WEAPON_5,
    BIND_WEAPON_6,
    BIND_WEAPON_7,
    BIND_WEAPON_8,
    BIND_WEAPON_9,
    BIND_COUNT,
} BindAction;

/* Difficulty (Outlaws skill levels, LOCAL.MSG 6010-6012). */
typedef enum {
    SKILL_GOOD = 1,   /* easy   */
    SKILL_BAD  = 2,   /* medium */
    SKILL_UGLY = 3,   /* hard   */
} Skill;

/* Volume channels (0..100), LOCAL.MSG 5109-5113. */
typedef enum {
    VOL_MASTER = 0,
    VOL_MOVIE,
    VOL_EFFECTS,
    VOL_TAUNTS,
    VOL_MUSIC,
    VOL_COUNT,
} VolChannel;

typedef struct {
    /* Video */
    int   win_w, win_h;
    bool  fullscreen;

    /* Audio — five channels, 0..100. */
    int   volume[VOL_COUNT];

    /* Gameplay */
    int   difficulty;          /* Skill (1..3) */

    /* Mouse */
    bool  mouse_enabled;
    float mouse_sensitivity;   /* 0.25 .. 4.0, 1.0 = default */
    bool  mouse_invert;

    /* Controls: action -> SDL_Scancode. */
    int   bind[BIND_COUNT];

    char  player_name[32];     /* multiplayer */
} Settings;

/* The one global settings instance. */
extern Settings g_settings;

/* Human-readable label for a bindable action (for the KEYBOARD menu). */
const char *bind_label(BindAction a);

/* Label for a volume channel. */
const char *vol_label(VolChannel c);

/* Reset to the built-in defaults (the engine's original hard-coded keys). */
void settings_defaults(Settings *s);

/* Path to the config file (game dir). */
const char *settings_path(void);

/* Load outlaws.cfg into *s. Returns false (and leaves defaults) if absent. */
bool settings_load(Settings *s);

/* Write *s to outlaws.cfg. */
void settings_save(const Settings *s);

/* Effective 0..1 gain for a channel = master * channel (both 0..100). */
float settings_gain(const Settings *s, VolChannel c);
