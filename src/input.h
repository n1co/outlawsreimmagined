/*
 * input.h - Input state management (keyboard + mouse)
 */
#pragma once

#include "engine.h"
#include <SDL2/SDL.h>

#define KEY_MAX SDL_NUM_SCANCODES

typedef struct {
    /* Keyboard */
    bool keys[KEY_MAX];       /* Currently held */
    bool keys_prev[KEY_MAX];  /* Previous frame */

    /* Mouse */
    i32  mouse_x, mouse_y;
    i32  mouse_dx, mouse_dy;  /* Frame delta */
    bool mouse_buttons[5];
    bool mouse_captured;      /* True when cursor is grabbed */
    bool suppress_capture;    /* App: don't auto-grab on click (menus/pause) */

    /* Quit flag */
    bool quit;
} InputState;

/* Initialize input state. */
void input_init(InputState *input);

/* Process all pending SDL events. Call once per frame. */
void input_update(InputState *input);

/* Query helpers */
static inline bool input_key_held(const InputState *s, SDL_Scancode k)    { return s->keys[k]; }
static inline bool input_key_pressed(const InputState *s, SDL_Scancode k) { return s->keys[k] && !s->keys_prev[k]; }
static inline bool input_key_released(const InputState *s, SDL_Scancode k){ return !s->keys[k] && s->keys_prev[k]; }
/* Consume a key's press edge this frame so later input_key_pressed() calls (e.g.
 * a menu opened by the same key) don't re-fire on the same press. */
static inline void input_consume_key(InputState *s, SDL_Scancode k){ s->keys_prev[k] = s->keys[k]; }

/* Capture/release mouse cursor for FPS look. */
void input_capture_mouse(InputState *input, bool capture);
