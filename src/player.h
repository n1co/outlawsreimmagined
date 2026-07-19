/*
 * player.h - Player state and FPS controller
 */
#pragma once

#include "engine.h"
#include "input.h"
#include "weapon.h"

/* Player constants — from comprehensive Ghidra RE (docs/collision_system.md) */
#define PLAYER_MOVE_SPEED     30.0f    /* MAX_VEL from air.phy */
#define PLAYER_RUN_MULT       1.5f
#define PLAYER_TURN_SPEED     2.6f    /* BODY_YAW_RATE=150 deg/s */
#define PLAYER_MOUSE_SENS     0.002f  /* Radians/pixel */
#define PLAYER_HEIGHT         5.5f    /* EYE_HEIGHT = HEIGHT(6.0) - HEAD(0.5) */
#define PLAYER_BODY_HEIGHT    6.0f    /* HEIGHT from RE docs */
/* Crouch (Ctrl): body HEIGHT/CROUCH_DIV = 6.0/4.0 = 1.5; eye = 1.5 - HEAD(0.5) */
#define PLAYER_CROUCH_HEIGHT  1.5f    /* Crouched body height */
#define PLAYER_CROUCH_EYE     1.0f    /* Crouched eye height */
#define PLAYER_CROUCH_SPEED   14.0f   /* Eye-height transition rate (u/s) */
#define PLAYER_MAX_PITCH      (OL_HALF_PI - 0.05f)
#define PLAYER_GRAVITY       -40.0f   /* GRAVITY from RE docs */
#define PLAYER_STEP_HEIGHT     0.4f   /* STEP_HEIGHT from RE docs */
#define PLAYER_MAX_HEALTH      100

typedef struct {
    /* Position and orientation */
    Vec3  pos;          /* World position (feet) */
    f32   yaw;          /* Horizontal angle (radians) */
    f32   pitch;        /* Vertical angle (radians) */

    /* Physics */
    f32   vel_y;        /* Vertical velocity */
    bool  on_ground;
    bool  crouching;    /* Ctrl held: crouched this frame */
    f32   eye_height;   /* Current (smoothed) eye height above feet */

    /* Game stats */
    i32   health;
    i32   keys;         /* Bitmask of collected keys */

    /* Weapons */
    WeaponState weapons;

    /* Sector tracking (for collision and physics) */
    int   sector_idx;

    /* State flags */
    bool  dead;
    f32   dead_timer;   /* Time since death (for respawn) */
    bool  fire_held;    /* Was fire held last frame? */

    /* Weapon fire animation */
    bool  fire_anim;       /* Currently playing fire animation */
    bool  fire_alt;        /* Was this an alternate fire? */
    f32   fire_anim_timer; /* Time elapsed in fire animation */
    f32   fire_anim_dur;   /* Total duration of fire animation */
} Player;

/* Initialize player at given position with default loadout. */
void player_init(Player *p, Vec3 start_pos);

/*
 * Update player: input → movement → (collision applied externally).
 * Returns true if player fired this frame.
 */
bool player_update(Player *p, const InputState *input, f32 dt);

/* Get player eye position (for camera and shooting). */
Vec3 player_eye_pos(const Player *p);

/* Apply a damage to the player. Negative = heal. */
void player_damage(Player *p, i32 amount);

/* Respawn player at given position. */
void player_respawn(Player *p, Vec3 pos, f32 yaw);
