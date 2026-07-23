/*
 * settings.c - Persistent user settings implementation.
 */
#include "settings.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

Settings g_settings;

/* Keep this order in sync with BindAction; used as config keys + menu labels. */
static const struct { const char *key; const char *label; } s_bind_info[BIND_COUNT] = {
    [BIND_FORWARD]     = { "forward",     "Move Forward" },
    [BIND_BACK]        = { "back",        "Move Backward" },
    [BIND_STRAFE_L]    = { "strafe_left", "Strafe Left" },
    [BIND_STRAFE_R]    = { "strafe_right","Strafe Right" },
    [BIND_TURN_L]      = { "turn_left",   "Turn Left" },
    [BIND_TURN_R]      = { "turn_right",  "Turn Right" },
    [BIND_LOOK_UP]     = { "look_up",     "Look Up" },
    [BIND_LOOK_DOWN]   = { "look_down",   "Look Down" },
    [BIND_RUN]         = { "run",         "Run" },
    [BIND_CROUCH]      = { "crouch",      "Crouch" },
    [BIND_JUMP]        = { "jump",        "Jump" },
    [BIND_USE]         = { "use",         "Use / Open" },
    [BIND_RELOAD]      = { "reload",      "Reload" },
    [BIND_MAP]         = { "map",         "Automap" },
    [BIND_SCOPE]       = { "scope",       "Rifle Scope" },
    [BIND_NEXT_WEAPON] = { "next_weapon", "Next Weapon" },
    [BIND_PREV_WEAPON] = { "prev_weapon", "Prev Weapon" },
    [BIND_WEAPON_1]    = { "weapon_1",    "Weapon: Fists" },
    [BIND_WEAPON_2]    = { "weapon_2",    "Weapon: Pistol" },
    [BIND_WEAPON_3]    = { "weapon_3",    "Weapon: Rifle" },
    [BIND_WEAPON_4]    = { "weapon_4",    "Weapon: Shotgun" },
    [BIND_WEAPON_5]    = { "weapon_5",    "Weapon: Dbl. Shotgun" },
    [BIND_WEAPON_6]    = { "weapon_6",    "Weapon: Sawed-off" },
    [BIND_WEAPON_7]    = { "weapon_7",    "Weapon: Dynamite" },
    [BIND_WEAPON_8]    = { "weapon_8",    "Weapon: Knife" },
    [BIND_WEAPON_9]    = { "weapon_9",    "Weapon: Gatling" },
};

static const char *s_vol_label[VOL_COUNT] = {
    "Master Volume", "Movie Volume", "Effects Volume", "Taunts Volume", "Music Volume",
};

const char *bind_label(BindAction a) {
    return (a >= 0 && a < BIND_COUNT) ? s_bind_info[a].label : "?";
}
const char *vol_label(VolChannel c) {
    return (c >= 0 && c < VOL_COUNT) ? s_vol_label[c] : "?";
}

void settings_defaults(Settings *s) {
    memset(s, 0, sizeof(*s));
    s->win_w = 800; s->win_h = 600; s->fullscreen = false;
    for (int i = 0; i < VOL_COUNT; i++) s->volume[i] = 80;
    s->difficulty = SKILL_BAD;      /* medium, matches the engine default */
    s->mouse_enabled = true;
    s->mouse_sensitivity = 1.0f;
    s->mouse_invert = false;
    snprintf(s->player_name, sizeof(s->player_name), "Player");

    /* Defaults = the keys the engine originally hard-coded (player.c/main.c). */
    s->bind[BIND_FORWARD]     = SDL_SCANCODE_W;
    s->bind[BIND_BACK]        = SDL_SCANCODE_S;
    s->bind[BIND_STRAFE_L]    = SDL_SCANCODE_A;
    s->bind[BIND_STRAFE_R]    = SDL_SCANCODE_D;
    s->bind[BIND_TURN_L]      = SDL_SCANCODE_LEFT;
    s->bind[BIND_TURN_R]      = SDL_SCANCODE_RIGHT;
    s->bind[BIND_LOOK_UP]     = SDL_SCANCODE_PAGEUP;
    s->bind[BIND_LOOK_DOWN]   = SDL_SCANCODE_PAGEDOWN;
    s->bind[BIND_RUN]         = SDL_SCANCODE_LSHIFT;
    s->bind[BIND_CROUCH]      = SDL_SCANCODE_LCTRL;
    s->bind[BIND_JUMP]        = SDL_SCANCODE_SPACE;
    s->bind[BIND_USE]         = SDL_SCANCODE_E;
    s->bind[BIND_RELOAD]      = SDL_SCANCODE_R;
    s->bind[BIND_MAP]         = SDL_SCANCODE_TAB;
    s->bind[BIND_SCOPE]       = SDL_SCANCODE_V;
    s->bind[BIND_NEXT_WEAPON] = SDL_SCANCODE_EQUALS;
    s->bind[BIND_PREV_WEAPON] = SDL_SCANCODE_MINUS;
    s->bind[BIND_WEAPON_1]    = SDL_SCANCODE_1;
    s->bind[BIND_WEAPON_2]    = SDL_SCANCODE_2;
    s->bind[BIND_WEAPON_3]    = SDL_SCANCODE_3;
    s->bind[BIND_WEAPON_4]    = SDL_SCANCODE_4;
    s->bind[BIND_WEAPON_5]    = SDL_SCANCODE_5;
    s->bind[BIND_WEAPON_6]    = SDL_SCANCODE_6;
    s->bind[BIND_WEAPON_7]    = SDL_SCANCODE_7;
    s->bind[BIND_WEAPON_8]    = SDL_SCANCODE_8;
    s->bind[BIND_WEAPON_9]    = SDL_SCANCODE_9;
}

const char *settings_path(void) { return "outlaws.cfg"; }

static int bind_from_key(const char *key) {
    for (int i = 0; i < BIND_COUNT; i++)
        if (strcmp(s_bind_info[i].key, key) == 0) return i;
    return -1;
}

bool settings_load(Settings *s) {
    settings_defaults(s);
    FILE *f = fopen(settings_path(), "r");
    if (!f) return false;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64], val[160];
        if (sscanf(line, " %63[^=] = %159[^\n]", key, val) != 2) continue;
        /* trim trailing spaces on key */
        for (int i = (int)strlen(key) - 1; i >= 0 && key[i] == ' '; i--) key[i] = '\0';
        if      (strcmp(key, "win_w") == 0) s->win_w = atoi(val);
        else if (strcmp(key, "win_h") == 0) s->win_h = atoi(val);
        else if (strcmp(key, "fullscreen") == 0) s->fullscreen = atoi(val) != 0;
        else if (strcmp(key, "vol_master")  == 0) s->volume[VOL_MASTER]  = atoi(val);
        else if (strcmp(key, "vol_movie")   == 0) s->volume[VOL_MOVIE]   = atoi(val);
        else if (strcmp(key, "vol_effects") == 0) s->volume[VOL_EFFECTS] = atoi(val);
        else if (strcmp(key, "vol_taunts")  == 0) s->volume[VOL_TAUNTS]  = atoi(val);
        else if (strcmp(key, "vol_music")   == 0) s->volume[VOL_MUSIC]   = atoi(val);
        else if (strcmp(key, "difficulty")  == 0) s->difficulty = atoi(val);
        else if (strcmp(key, "mouse_enabled") == 0) s->mouse_enabled = atoi(val) != 0;
        else if (strcmp(key, "mouse_sensitivity") == 0) s->mouse_sensitivity = (float)atof(val);
        else if (strcmp(key, "mouse_invert") == 0) s->mouse_invert = atoi(val) != 0;
        else if (strcmp(key, "player_name") == 0) snprintf(s->player_name, sizeof(s->player_name), "%s", val);
        else if (strncmp(key, "bind_", 5) == 0) {
            int b = bind_from_key(key + 5);
            if (b >= 0) {
                SDL_Scancode sc = SDL_GetScancodeFromName(val);
                if (sc != SDL_SCANCODE_UNKNOWN) s->bind[b] = sc;
            }
        }
    }
    fclose(f);
    /* sanity clamps */
    if (s->difficulty < SKILL_GOOD || s->difficulty > SKILL_UGLY) s->difficulty = SKILL_BAD;
    for (int i = 0; i < VOL_COUNT; i++)
        s->volume[i] = s->volume[i] < 0 ? 0 : s->volume[i] > 100 ? 100 : s->volume[i];
    if (s->win_w < 320) s->win_w = 800;
    if (s->win_h < 240) s->win_h = 600;
    if (s->mouse_sensitivity < 0.1f) s->mouse_sensitivity = 1.0f;
    return true;
}

void settings_save(const Settings *s) {
    FILE *f = fopen(settings_path(), "w");
    if (!f) return;
    fprintf(f, "# Outlaws (reimplementation) settings\n");
    fprintf(f, "win_w = %d\n", s->win_w);
    fprintf(f, "win_h = %d\n", s->win_h);
    fprintf(f, "fullscreen = %d\n", s->fullscreen ? 1 : 0);
    fprintf(f, "vol_master = %d\n",  s->volume[VOL_MASTER]);
    fprintf(f, "vol_movie = %d\n",   s->volume[VOL_MOVIE]);
    fprintf(f, "vol_effects = %d\n", s->volume[VOL_EFFECTS]);
    fprintf(f, "vol_taunts = %d\n",  s->volume[VOL_TAUNTS]);
    fprintf(f, "vol_music = %d\n",   s->volume[VOL_MUSIC]);
    fprintf(f, "difficulty = %d\n", s->difficulty);
    fprintf(f, "mouse_enabled = %d\n", s->mouse_enabled ? 1 : 0);
    fprintf(f, "mouse_sensitivity = %.3f\n", s->mouse_sensitivity);
    fprintf(f, "mouse_invert = %d\n", s->mouse_invert ? 1 : 0);
    fprintf(f, "player_name = %s\n", s->player_name);
    for (int i = 0; i < BIND_COUNT; i++) {
        const char *n = SDL_GetScancodeName((SDL_Scancode)s->bind[i]);
        fprintf(f, "bind_%s = %s\n", s_bind_info[i].key, (n && n[0]) ? n : "Unknown");
    }
    fclose(f);
}

float settings_gain(const Settings *s, VolChannel c) {
    if (c < 0 || c >= VOL_COUNT) return 1.0f;
    return (s->volume[VOL_MASTER] / 100.0f) * (s->volume[c] / 100.0f);
}
