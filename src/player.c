/*
 * player.c - Player FPS controller
 *
 * Faithful reimplementation of olwin.exe Player_MovePhysicsTick @0x440db0
 * horizontal model (per-tick order): sector friction multiply → per-axis
 * acceleration on input → per-axis linear drag on input-free axes → speed
 * clamp at MAX_VEL × run × crouch → integrate. Vertical physics (gravity
 * gating, floor snap, jumping, fall damage) runs in main.c where the sector
 * heights are known; this file computes intent, eye transitions and bob.
 */
#include "player.h"

void player_init(Player *p, Vec3 start_pos) {
    memset(p, 0, sizeof(*p));
    p->pos        = start_pos;
    p->yaw        = 0.0f;
    p->pitch      = 0.0f;
    p->health     = PLAYER_MAX_HEALTH;
    p->sector_idx = -1;  /* Will be resolved on first update */
    p->eye_height = PLAYER_HEIGHT;
    p->max_eye    = PLAYER_HEIGHT;
    p->energy     = 100.0f;
    weapon_init(&p->weapons);
}

bool player_update(Player *p, const InputState *input, f32 dt,
                   f32 sector_friction) {
    if (dt > PHY_DT_CLAMP) dt = PHY_DT_CLAMP;   /* 0.25 s clamp (0x50099c) */

    if (p->dead) {
        p->dead_timer += dt;
        return false;
    }

    /* ---- Mouse look ---- */
    if (input->mouse_captured) {
        p->yaw   -= input->mouse_dx * PLAYER_MOUSE_SENS;
        p->pitch -= input->mouse_dy * PLAYER_MOUSE_SENS;
    }

    /* ---- Keyboard look (BODY_YAW_RATE 150°/s, PITCH_RATE 160°/s) ---- */
    f32 yaw_rate = PHY_BODY_YAW_RATE * OL_DEG2RAD;
    if (input_key_held(input, SDL_SCANCODE_LEFT))
        p->yaw += yaw_rate * dt;
    if (input_key_held(input, SDL_SCANCODE_RIGHT))
        p->yaw -= yaw_rate * dt;
    if (input_key_held(input, SDL_SCANCODE_PAGEUP))
        p->pitch += PHY_PITCH_RATE * OL_DEG2RAD * dt;
    if (input_key_held(input, SDL_SCANCODE_PAGEDOWN))
        p->pitch -= PHY_PITCH_RATE * OL_DEG2RAD * dt;
    p->pitch = OL_CLAMP(p->pitch, -PLAYER_MAX_PITCH, PLAYER_MAX_PITCH);

    /* ---- Intent ---- */
    p->running = input_key_held(input, SDL_SCANCODE_LSHIFT) ||
                 input_key_held(input, SDL_SCANCODE_RSHIFT);
    p->crouching = input_key_held(input, SDL_SCANCODE_LCTRL) ||
                   input_key_held(input, SDL_SCANCODE_RCTRL) ||
                   input_key_held(input, SDL_SCANCODE_C);
    f32 intent = p->running ? PHY_RUN_MULT : 1.0f;   /* "Fast" = 1.5 */

    f32 fwd_in = 0.0f, str_in = 0.0f;
    if (input_key_held(input, SDL_SCANCODE_W) || input_key_held(input, SDL_SCANCODE_UP))
        fwd_in += 1.0f;
    if (input_key_held(input, SDL_SCANCODE_S) || input_key_held(input, SDL_SCANCODE_DOWN))
        fwd_in -= 1.0f;
    if (input_key_held(input, SDL_SCANCODE_D)) str_in += 1.0f;
    if (input_key_held(input, SDL_SCANCODE_A)) str_in -= 1.0f;

    /* World-space wish direction from player-relative intent */
    f32 cy = cosf(p->yaw), sy = sinf(p->yaw);
    f32 wish_x = cy * fwd_in + sy * str_in;
    f32 wish_z = sy * fwd_in - cy * str_in;

    /* ---- Horizontal physics (Player_MovePhysicsTick order) ----
     * Stamina only limits the RUN bonus and jump: walking speed is never
     * energy-scaled (scaling base speed made the player grind to a crawl —
     * the original's stamina drain applies to sprinting effort). */
    f32 energy_pct = p->energy * 0.01f;
    f32 run_bonus = p->running ? (1.0f + (PHY_RUN_MULT - 1.0f) * energy_pct)
                               : 1.0f;
    f32 ctrl = p->on_ground ? 1.0f : PHY_OFF_GROUND;   /* airborne = 0.2 */

    /* 1. Sector friction: per-tick velocity multiply (Physics_ApplyFriction
     *    @0x4e03b0). LVT FRICTION default 1.0 = no-op. */
    p->vel.x *= sector_friction;
    p->vel.z *= sector_friction;

    /* 2. Acceleration on axes with input (ACCEL 200 × intent) */
    f32 accel = PHY_ACCEL_XZ * run_bonus * ctrl * dt;
    p->vel.x += wish_x * accel;
    p->vel.z += wish_z * accel;

    /* 3. Linear drag toward rest on input-free axes (DECEL 80 u/s²,
     *    Physics_ApplyDrag @0x4e0400; snap to zero below epsilon). */
    f32 drag = PHY_DECEL_XZ * dt;
    if (fabsf(wish_x) < 1e-4f) {
        if      (p->vel.x >  drag) p->vel.x -= drag;
        else if (p->vel.x < -drag) p->vel.x += drag;
        else                       p->vel.x  = 0.0f;
    }
    if (fabsf(wish_z) < 1e-4f) {
        if      (p->vel.z >  drag) p->vel.z -= drag;
        else if (p->vel.z < -drag) p->vel.z += drag;
        else                       p->vel.z  = 0.0f;
    }

    /* 4. Speed clamp (normalize-and-scale — this also normalizes diagonals):
     *    MAX_VEL × run bonus (stamina-scaled) × crouch(0.5) */
    f32 max_v = PHY_MAX_VEL * run_bonus *
                (p->crouching ? PHY_CROUCH_SPEED_MULT : 1.0f);
    f32 sp2 = p->vel.x * p->vel.x + p->vel.z * p->vel.z;
    if (sp2 > max_v * max_v && sp2 > 1e-8f) {
        f32 s = max_v / sqrtf(sp2);
        p->vel.x *= s;
        p->vel.z *= s;
    }

    /* 5. Integrate horizontal position (collision resolves it in main.c) */
    p->pos.x += p->vel.x * dt;
    p->pos.z += p->vel.z * dt;

    /* ---- Jump request (consumed by vertical physics in main.c) ---- */
    if (input_key_pressed(input, SDL_SCANCODE_SPACE))
        p->want_jump = true;

    /* ---- Energy (stamina): drains only while SPRINTING; recovers
     * otherwise. Walking never drains (no creeping slowdown). ---- */
    f32 speed = sqrtf(sp2 > max_v * max_v ? max_v * max_v : sp2);
    bool sprinting = p->running && speed > PHY_MAX_VEL * 1.01f;
    p->energy += (sprinting ? PHY_E_MOVE : PHY_E_REST) * dt;
    p->energy = OL_CLAMP(p->energy, 25.0f, 100.0f);

    /* ---- Eye height: crouch/stand transition + step smoothing ----
     * Linear at STEP_SPEED (25 u/s, × run multiplier); standing target is
     * headroom-limited (max_eye, set by the collision pass) so the player
     * cannot rise into a low ceiling — crawling stays crouched under floors. */
    f32 eye_rate = PHY_STEP_SPEED * intent * dt;
    f32 target_eye = p->crouching ? PLAYER_CROUCH_EYE
                                  : OL_MIN(PLAYER_HEIGHT, p->max_eye);
    if (target_eye < PLAYER_CROUCH_EYE) target_eye = PLAYER_CROUCH_EYE;
    if (p->eye_height <= 0.0f) p->eye_height = PLAYER_HEIGHT;
    if (p->eye_height < target_eye)
        p->eye_height = OL_MIN(p->eye_height + eye_rate, target_eye);
    else if (p->eye_height > target_eye)
        p->eye_height = OL_MAX(p->eye_height - eye_rate, target_eye);

    /* Step-up/down smoothing: the offset accumulated on floor snaps eases
     * back to zero at STEP_SPEED (the smooth stair feel, @0x444056). */
    if (p->eye_step_ofs > 0.0f)
        p->eye_step_ofs = OL_MAX(p->eye_step_ofs - eye_rate, 0.0f);
    else if (p->eye_step_ofs < 0.0f)
        p->eye_step_ofs = OL_MIN(p->eye_step_ofs + eye_rate, 0.0f);

    /* ---- View bob: amp = min(1, speed·0.05)·0.35, one sine per stride ---- */
    if (p->on_ground && speed > 1.0f) {
        p->stride += speed * dt;
        f32 amp = OL_MIN(1.0f, speed * PHY_BOB_SPEED_AMP) * PHY_BOB_HEIGHT;
        f32 phase = (p->stride / PHY_STRIDE_LEN) * OL_PI;
        p->bob = sinf(phase) * amp;
    } else {
        p->bob *= OL_MAX(0.0f, 1.0f - 8.0f * dt);
    }

    /* ---- Weapon selection ---- */
    if (input_key_pressed(input, SDL_SCANCODE_1)) weapon_switch(&p->weapons, WEAPON_FIST);
    if (input_key_pressed(input, SDL_SCANCODE_2)) weapon_switch(&p->weapons, WEAPON_PISTOL);
    if (input_key_pressed(input, SDL_SCANCODE_3)) weapon_switch(&p->weapons, WEAPON_RIFLE);
    if (input_key_pressed(input, SDL_SCANCODE_4)) weapon_switch(&p->weapons, WEAPON_SHOTGUN);
    if (input_key_pressed(input, SDL_SCANCODE_5)) weapon_switch(&p->weapons, WEAPON_DBL_SHOTGUN);
    if (input_key_pressed(input, SDL_SCANCODE_6)) weapon_switch(&p->weapons, WEAPON_SAW_GUN);
    if (input_key_pressed(input, SDL_SCANCODE_7)) weapon_switch(&p->weapons, WEAPON_DYNAMITE);
    if (input_key_pressed(input, SDL_SCANCODE_8)) weapon_switch(&p->weapons, WEAPON_KNIFE);
    if (input_key_pressed(input, SDL_SCANCODE_9)) weapon_switch(&p->weapons, WEAPON_GATLING);

    if (input_key_pressed(input, SDL_SCANCODE_EQUALS))
        weapon_cycle_next(&p->weapons);
    if (input_key_pressed(input, SDL_SCANCODE_MINUS))
        weapon_cycle_prev(&p->weapons);

    /* ---- Weapon update ---- */
    weapon_update(&p->weapons, dt);

    /* Firing is handled by main.c (mode routing per weapon button table,
     * cook/throw for knife & dynamite, scope, reload). */
    return false;
}

Vec3 player_eye_pos(const Player *p) {
    f32 eye = (p->eye_height > 0.0f) ? p->eye_height : PLAYER_HEIGHT;
    return (Vec3){ p->pos.x,
                   p->pos.y + eye + p->eye_step_ofs + p->bob,
                   p->pos.z };
}

void player_damage(Player *p, i32 amount) {
    if (p->dead) return;
    p->health -= amount;
    if (p->health <= 0) {
        p->health    = 0;
        p->dead      = true;
        p->dead_timer = 0.0f;
    }
    if (p->health > PLAYER_MAX_HEALTH)
        p->health = PLAYER_MAX_HEALTH;
}

void player_respawn(Player *p, Vec3 pos, f32 yaw) {
    p->pos       = pos;
    p->yaw       = yaw;
    p->pitch     = 0.0f;
    p->vel       = (Vec3){0, 0, 0};
    p->vel_y     = 0.0f;
    p->on_ground = false;
    p->eye_height = PLAYER_HEIGHT;
    p->max_eye    = PLAYER_HEIGHT;
    p->eye_step_ofs = 0.0f;
    p->energy    = 100.0f;
    p->health    = PLAYER_MAX_HEALTH;
    p->dead      = false;
    p->dead_timer = 0.0f;
    p->sector_idx = -1;
}
