/*
 * player.h - Player state and FPS controller
 *
 * Movement model reverse-engineered from olwin.exe Player_MovePhysicsTick
 * @0x440db0 (see docs/ RE reports): variable timestep clamped to 0.25 s,
 * per-axis acceleration/drag against the sector reference frame, speed clamp
 * (not accel cutoff), STEP_HEIGHT-gated gravity, STEP_SPEED eye smoothing for
 * stairs and crouch, collision height = eye + HEAD_HEIGHT.
 *
 * Tunables come from the game's own data (outlaws.lab):
 *   air.phy:  ACCEL 200/50/200, DECEL 80/120/80, MAX_VEL 30, JUMP_VEL 20,
 *             STEP_HEIGHT 3, STEP_SPEED 25, BODY_YAW_RATE 150°/s,
 *             PITCH_RATE 160°/s, OFF_GROUND_ABILITY 0.2
 *   player ITM: HEIGHT 6.0, HEAD_HEIGHT 0.5, CROUCH = HEIGHT/4 = 1.5,
 *             RADIUS 1.5; fall damage: threshold 45.0, scale 0.2
 */
#pragma once

#include "engine.h"
#include "input.h"
#include "weapon.h"

/* air.phy (outlaws.lab) */
#define PHY_ACCEL_XZ          200.0f
#define PHY_DECEL_XZ           80.0f
#define PHY_MAX_VEL            30.0f
#define PHY_JUMP_VEL           20.0f
#define PHY_STEP_HEIGHT         3.0f
#define PHY_STEP_SPEED         25.0f
#define PHY_BODY_YAW_RATE     150.0f   /* deg/s */
#define PHY_PITCH_RATE        160.0f   /* deg/s */
#define PHY_OFF_GROUND          0.2f   /* airborne control multiplier */

/* Player ITM (cidjames.itm defaults / Player_Create @0x43c250) */
#define PLAYER_BODY_HEIGHT      6.0f   /* HEIGHT */
#define PLAYER_HEAD_HEIGHT      0.5f   /* HEAD_HEIGHT: clearance above the eye */
#define PLAYER_HEIGHT           (PLAYER_BODY_HEIGHT - PLAYER_HEAD_HEIGHT) /* standing eye 5.5 */
#define PLAYER_CROUCH_EYE       (PLAYER_BODY_HEIGHT * 0.25f)             /* 1.5 */
#define PLAYER_RADIUS_ITM       1.5f
#define PLAYER_MAX_HEALTH      100

/* Engine constants (olwin.exe .rdata) */
#define PHY_RUN_MULT            1.5f   /* "Fast" multiplier (0x50039c) */
#define PHY_CROUCH_SPEED_MULT   0.5f   /* crouch max-speed mult (0x500394) */
#define PHY_DT_CLAMP            0.25f  /* max frame step (0x50099c) */
#define PHY_FALL_DMG_SPEED     45.0f   /* fall-damage threshold (player+0x70) */
#define PHY_FALL_DMG_SCALE      0.2f   /* (player+0x74) */
#define PHY_LAND_HEAVY         (PHY_JUMP_VEL * 1.8f)  /* heavy land sound */
#define PHY_LAND_LIGHT         (PHY_JUMP_VEL * 0.2f)  /* light land sound */
#define PHY_BOB_SPEED_AMP       0.05f  /* bob amp = min(1, speed*0.05) (0x50044c) */
#define PHY_BOB_HEIGHT          0.35f  /* vertical bob amplitude wu (0x500454) */
#define PHY_STRIDE_LEN         15.0f   /* footstep/bob stride (player+0x80) */
#define PLAYER_MAX_PITCH       (50.0f * OL_DEG2RAD)   /* pitch clamp ±50° (0x4426a9) */
#define PLAYER_MOUSE_SENS       0.002f /* radians/pixel */

/* Energy/stamina (player ITM: E_REST +4/s, E_MOVE -4/s, E_JUMP -5) */
/* Swimming (water sectors). The player floats submerged with the eyes near the
 * surface and swims up/down with Space/Ctrl; water damps motion and cancels
 * gravity + fall damage. Reconstruction (the retail physics tick was too large
 * to decompile) — tune to taste. */
#define PHY_SWIM_SPEED_MULT     0.6f   /* horizontal speed while swimming */
#define PHY_SWIM_VERT          14.0f   /* up/down swim speed (u/s) */
#define PHY_SWIM_BUOY          18.0f   /* buoyancy pull toward the float level */
#define PHY_SWIM_DAMP           5.0f   /* water drag on vertical velocity /s */
#define PHY_SWIM_DEPTH         48.0f   /* max dive below the surface */

#define PHY_E_REST              4.0f
#define PHY_E_MOVE             -4.0f
#define PHY_E_JUMP             -5.0f

typedef struct {
    /* Position and orientation */
    Vec3  pos;          /* World position (feet) */
    f32   yaw;          /* Horizontal angle (radians) */
    f32   pitch;        /* Vertical angle (radians) */

    /* Physics */
    Vec3  vel;          /* Full velocity (x/z horizontal model, y = vel_y) */
    f32   vel_y;        /* Vertical velocity (alias kept for existing code) */
    bool  on_ground;
    bool  crouching;    /* Crouch key held */
    bool  want_jump;    /* Jump requested this frame (consumed by physics) */
    f32   eye_height;   /* Current (smoothed) eye height above feet */
    f32   eye_step_ofs; /* Step-up/down smoothing offset (eased at STEP_SPEED) */
    f32   max_eye;      /* Headroom-limited standing eye (set by collision) */
    f32   fall_peak;    /* Max |vel_y| during current fall (fall damage) */
    f32   energy;       /* Stamina 0..100 (scales accel/maxvel/jump) */
    bool  running;      /* Shift held this frame */

    /* View bob */
    f32   stride;       /* Distance accumulator (bob phase, footsteps) */
    f32   bob;          /* Current vertical bob offset */

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

/* Current collision-cylinder height: eye + head clearance (actor+0x3C =
 * eye + HEAD_HEIGHT in the original). Standing 6.0, crouched 2.0 — this is
 * what lets the player crawl under low openings. */
static inline f32 player_collision_height(const Player *p) {
    f32 eye = (p->eye_height > 0.0f) ? p->eye_height : PLAYER_HEIGHT;
    return eye + PLAYER_HEAD_HEIGHT;
}

/* Initialize player at given position with default loadout. */
void player_init(Player *p, Vec3 start_pos);

/*
 * Update player: input → movement intent → horizontal physics.
 * sector_friction/gravity: current sector's LVT values (1.0 / -60 defaults).
 * Returns true if player fired this frame.
 */
bool player_update(Player *p, const InputState *input, f32 dt,
                   f32 sector_friction);

/* Get player eye position (for camera and shooting). */
Vec3 player_eye_pos(const Player *p);

/* Apply a damage to the player. Negative = heal. */
void player_damage(Player *p, i32 amount);

/* Respawn player at given position. */
void player_respawn(Player *p, Vec3 pos, f32 yaw);
