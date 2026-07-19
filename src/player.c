/*
 * player.c - Player FPS controller
 */
#include "player.h"

void player_init(Player *p, Vec3 start_pos) {
    memset(p, 0, sizeof(*p));
    p->pos       = start_pos;
    p->yaw       = 0.0f;
    p->pitch     = 0.0f;
    p->health    = PLAYER_MAX_HEALTH;
    p->sector_idx = -1;  /* Will be resolved on first update */
    p->eye_height = PLAYER_HEIGHT;
    weapon_init(&p->weapons);
}

bool player_update(Player *p, const InputState *input, f32 dt) {
    if (p->dead) {
        p->dead_timer += dt;
        return false;
    }

    /* ---- Mouse look ---- */
    if (input->mouse_captured) {
        p->yaw   -= input->mouse_dx * PLAYER_MOUSE_SENS;
        p->pitch -= input->mouse_dy * PLAYER_MOUSE_SENS;
        p->pitch  = OL_CLAMP(p->pitch, -PLAYER_MAX_PITCH, PLAYER_MAX_PITCH);
    }

    /* ---- Keyboard look ---- */
    if (input_key_held(input, SDL_SCANCODE_LEFT))
        p->yaw += PLAYER_TURN_SPEED * dt;
    if (input_key_held(input, SDL_SCANCODE_RIGHT))
        p->yaw -= PLAYER_TURN_SPEED * dt;

    /* ---- Movement speed ---- */
    f32 speed = PLAYER_MOVE_SPEED * dt;
    if (input_key_held(input, SDL_SCANCODE_LSHIFT) ||
        input_key_held(input, SDL_SCANCODE_RSHIFT))
        speed *= PLAYER_RUN_MULT;

    /* ---- Forward/strafe vectors in XZ plane ---- */
    f32 cy = cosf(p->yaw), sy = sinf(p->yaw);
    Vec3 fwd = { cy, 0, sy };    /* +Z in LVT when yaw=π/2 */
    Vec3 rgt = { sy, 0, -cy };   /* Right of forward (A=left, D=right) */

    Vec3 move = { 0, 0, 0 };
    if (input_key_held(input, SDL_SCANCODE_W) || input_key_held(input, SDL_SCANCODE_UP))
        move = vec3_add(move, vec3_scale(fwd, speed));
    if (input_key_held(input, SDL_SCANCODE_S) || input_key_held(input, SDL_SCANCODE_DOWN))
        move = vec3_sub(move, vec3_scale(fwd, speed));
    if (input_key_held(input, SDL_SCANCODE_A))
        move = vec3_sub(move, vec3_scale(rgt, speed));
    if (input_key_held(input, SDL_SCANCODE_D))
        move = vec3_add(move, vec3_scale(rgt, speed));

    /* Apply horizontal movement (collision resolved externally) */
    p->pos.x += move.x;
    p->pos.z += move.z;

    /* ---- Gravity / vertical movement ---- */
    if (!p->on_ground) {
        p->vel_y += PLAYER_GRAVITY * dt;
    }
    p->pos.y += p->vel_y * dt;

    /* Crouch: Ctrl lowers the eye height to the crouch height (smoothly),
     * matching the original (body HEIGHT/4). The feet position is unchanged. */
    p->crouching = input_key_held(input, SDL_SCANCODE_LCTRL) ||
                   input_key_held(input, SDL_SCANCODE_RCTRL);
    f32 target_eye = p->crouching ? PLAYER_CROUCH_EYE : PLAYER_HEIGHT;
    if (p->eye_height <= 0.0f) p->eye_height = PLAYER_HEIGHT; /* init */
    f32 step_eye = PLAYER_CROUCH_SPEED * dt;
    if (p->eye_height < target_eye)
        p->eye_height = OL_MIN(p->eye_height + step_eye, target_eye);
    else if (p->eye_height > target_eye)
        p->eye_height = OL_MAX(p->eye_height - step_eye, target_eye);

    /* ---- Weapon cycling ---- */
    if (input_key_pressed(input, SDL_SCANCODE_1)) weapon_switch(&p->weapons, WEAPON_FIST);
    if (input_key_pressed(input, SDL_SCANCODE_2)) weapon_switch(&p->weapons, WEAPON_PISTOL);
    if (input_key_pressed(input, SDL_SCANCODE_3)) weapon_switch(&p->weapons, WEAPON_RIFLE);
    if (input_key_pressed(input, SDL_SCANCODE_4)) weapon_switch(&p->weapons, WEAPON_SHOTGUN);
    if (input_key_pressed(input, SDL_SCANCODE_5)) weapon_switch(&p->weapons, WEAPON_DBL_SHOTGUN);
    if (input_key_pressed(input, SDL_SCANCODE_6)) weapon_switch(&p->weapons, WEAPON_SAW_GUN);
    if (input_key_pressed(input, SDL_SCANCODE_7)) weapon_switch(&p->weapons, WEAPON_DYNAMITE);
    if (input_key_pressed(input, SDL_SCANCODE_8)) weapon_switch(&p->weapons, WEAPON_KNIFE);
    if (input_key_pressed(input, SDL_SCANCODE_9)) weapon_switch(&p->weapons, WEAPON_GATLING);

    /* Mouse wheel weapon cycling */
    if (input_key_pressed(input, SDL_SCANCODE_EQUALS))
        weapon_cycle_next(&p->weapons);
    if (input_key_pressed(input, SDL_SCANCODE_MINUS))
        weapon_cycle_prev(&p->weapons);

    /* ---- Weapon update ---- */
    weapon_update(&p->weapons, dt);

    /* ---- Fire ---- */
    bool firing = input->mouse_buttons[0];
    bool fired  = false;
    if (firing && weapon_can_fire(&p->weapons)) {
        if (weapon_fire(&p->weapons))
            fired = true;
    }
    p->fire_held = firing;

    return fired;
}

Vec3 player_eye_pos(const Player *p) {
    f32 eye = (p->eye_height > 0.0f) ? p->eye_height : PLAYER_HEIGHT;
    return (Vec3){ p->pos.x, p->pos.y + eye, p->pos.z };
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
    p->vel_y     = 0.0f;
    p->on_ground = false;
    p->health    = PLAYER_MAX_HEALTH;
    p->dead      = false;
    p->dead_timer = 0.0f;
    p->sector_idx = -1;
}
