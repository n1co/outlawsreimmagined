/*
 * input.c - Input state management
 */
#include "input.h"
#include "debug_ui.h"

void input_init(InputState *input) {
    memset(input, 0, sizeof(*input));
}

void input_update(InputState *input) {
    /* Save previous key state */
    memcpy(input->keys_prev, input->keys, sizeof(input->keys));

    /* Reset per-frame deltas */
    input->mouse_dx = 0;
    input->mouse_dy = 0;

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        /* Forward to ImGui debug UI */
        debug_ui_process_event(&ev);
        switch (ev.type) {
        case SDL_QUIT:
            input->quit = true;
            break;
        case SDL_KEYDOWN:
            if (ev.key.keysym.scancode < KEY_MAX)
                input->keys[ev.key.keysym.scancode] = true;
            /* Escape releases mouse capture (the game uses ESC for the pause
             * menu; quitting is via the menu's QUIT or closing the window). */
            if (ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                if (input->mouse_captured)
                    input_capture_mouse(input, false);
            }
            break;
        case SDL_KEYUP:
            if (ev.key.keysym.scancode < KEY_MAX)
                input->keys[ev.key.keysym.scancode] = false;
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (ev.button.button <= 5) {
                input->mouse_buttons[ev.button.button - 1] = true;
                /* Clicking in the window grabs the mouse for FPS look — but NOT
                 * while a menu/pause is up (suppress_capture), and NOT when the
                 * click lands on the debug UI (ImGui wants it). Clicking the game
                 * world while the debug UI is open resumes play: grab + close it. */
                if (!input->mouse_captured && !input->suppress_capture) {
                    if (g_debug.visible && debug_ui_wants_mouse()) {
                        /* interacting with the debug panel — leave cursor free */
                    } else {
                        input_capture_mouse(input, true);
                        if (g_debug.visible) g_debug.visible = false;
                    }
                }
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (ev.button.button <= 5)
                input->mouse_buttons[ev.button.button - 1] = false;
            break;
        case SDL_MOUSEMOTION:
            input->mouse_x  = ev.motion.x;
            input->mouse_y  = ev.motion.y;
            input->mouse_dx += ev.motion.xrel;
            input->mouse_dy += ev.motion.yrel;
            break;
        case SDL_WINDOWEVENT:
            /* Window events handled by main loop */
            break;
        }
    }
}

void input_capture_mouse(InputState *input, bool capture) {
    input->mouse_captured = capture;
    SDL_SetRelativeMouseMode(capture ? SDL_TRUE : SDL_FALSE);
}
