/*
 * projectile.h - Thrown-weapon projectiles (knife, dynamite)
 *
 * Mechanics from olwin.exe (shot_ProjectileDispatch LAB_00459380,
 * shot_ExpProjectileDispatch LAB_00459ae0, Projectile_Move @0x459f30,
 * Object_Physics_Move @0x4e0ab0, Physics_FloorBounce @0x4e2c50) and the
 * projectile ITM data (pknife.itm / pdynam.itm):
 *
 *   - Throw: speed = RATE × power (power = clamp(heldSeconds, 0.5, 1.0),
 *     min speed 10), pitch += THROW_PITCH (knife 5°, dynamite 35°),
 *     inherits the thrower's velocity, launched from y + 0.85·height.
 *   - Physics: per-sector gravity (default -60); floor bounce with the
 *     sector ELASTICITY (default 0.3); wall hit halves speed; rolling
 *     friction ×0.75/tick when |vy| < 0.1; rest when |v| < 0.1.
 *   - At rest, a knife becomes a gknife pickup; UNLIT dynamite becomes
 *     gdynam — still a live projectile object whose damage handler sets
 *     the explode flag: SHOOTING GROUND DYNAMITE DETONATES IT, and one
 *     explosion damages nearby sticks → chain reactions.
 *   - Lit dynamite: fuse = 4.5 s from LIGHTING (LIFE 4500 ms) — it keeps
 *     counting while held and explodes in hand.
 *   - Explosion (pdynam): people RADIUS 50, DAMAGE 12, LOS-tested,
 *     falloff = 1.0 within 0.25·R else ((R−d)/(0.75·R))²; knockback along
 *     the blast direction; WALL_RADIUS 15 for wall/INF damage triggers.
 */
#pragma once

#include "engine.h"
#include "lvt.h"
#include "entity.h"

typedef enum {
    PROJ_NONE = 0,
    PROJ_KNIFE,      /* pknife */
    PROJ_DYNAMITE,   /* pdynam */
    PROJ_FX,         /* one-shot billboard animation (tntboom explosion) */
} ProjKind;

typedef struct {
    bool     active;
    ProjKind kind;
    Vec3     pos;
    Vec3     vel;
    f32      yaw;          /* Tumble display angle */
    f32      spin;         /* Tumble rate (rad/s) */
    bool     lit;          /* Dynamite with a burning fuse */
    f32      fuse;         /* Seconds until detonation (lit only) */
    bool     resting;      /* At rest on the ground (pickup / shootable) */
    int      sector;       /* Tracked sector */
    u32      tex;          /* Billboard texture (FX / fallback) */
    f32      w, h;         /* Billboard size (world units) */
    int      tdo;          /* 3DO model id (renderer_upload_tdo), -1 = none */
    f32      tumble;       /* End-over-end tumble angle (radians) */

    /* PROJ_FX: one-shot frame animation */
    u32      fx_frames[16];
    u32      fx_nframes;
    f32      fx_dt;        /* seconds per frame */
    f32      fx_timer;
    u32      fx_frame;
} Projectile;

#define MAX_PROJECTILES 64

/* pknife.itm / pdynam.itm constants */
#define PROJ_KNIFE_SPEED       120.0f
#define PROJ_KNIFE_PITCH         5.0f   /* degrees */
#define PROJ_KNIFE_RADIUS        2.0f   /* COLLIDE_RADIUS */
#define PROJ_TNT_SPEED          60.0f
#define PROJ_TNT_PITCH          35.0f
#define PROJ_TNT_RADIUS          1.0f
#define PROJ_TNT_FUSE            4.5f   /* LIFE 4500 ms */
#define PROJ_TNT_DMG            12.0f
#define PROJ_TNT_BLAST_R        50.0f   /* RADIUS: people */
#define PROJ_TNT_WALL_R         15.0f   /* WALL_RADIUS */
/* Engine constants (0x500560/64, 0x50052c, sector ELASTICITY default) */
#define PROJ_REST_SPEED          0.1f
#define PROJ_ROLL_FRICTION       0.75f
#define PROJ_WALL_DAMP           0.5f
#define PROJ_MIN_THROW_SPEED    10.0f

typedef struct ProjectileSystem {
    Projectile list[MAX_PROJECTILES];
    /* Explosion callback: main.c applies damage to player/entities/walls. */
    void (*on_explode)(Vec3 pos);
    void (*play_sfx)(const char *name, Vec3 pos);
} ProjectileSystem;

void projectile_init(ProjectileSystem *ps);

/* Throw a projectile. power in [0.5, 1.0]; lit only for dynamite. */
Projectile *projectile_throw(ProjectileSystem *ps, ProjKind kind,
                             Vec3 origin, f32 yaw, f32 pitch,
                             Vec3 inherit_vel, f32 power, bool lit,
                             f32 fuse_remaining);

/* Per-tick physics + fuses. */
void projectile_update(ProjectileSystem *ps, const LvtLevel *lvt, f32 dt);

/* Hitscan test against projectiles (ray vs cylinders). Returns index or -1. */
int projectile_raycast(const ProjectileSystem *ps, Vec3 origin, Vec3 dir,
                       f32 max_dist, f32 *hit_dist);

/* Damage a projectile (shooting ground dynamite → detonation). */
void projectile_damage(ProjectileSystem *ps, int idx);

/* Detonate all resting dynamite within `radius` of pos (chain reactions). */
void projectile_chain(ProjectileSystem *ps, Vec3 pos, f32 radius);

/* Try to pick up a resting projectile near the player (knife → +1 iknife,
 * unlit dynamite → +1 idynam). Returns the kind or PROJ_NONE. */
ProjKind projectile_try_pickup(ProjectileSystem *ps, Vec3 player_pos);

/* Spawn a one-shot billboard FX (explosion). */
void projectile_spawn_fx(ProjectileSystem *ps, Vec3 pos,
                         const u32 *frames, u32 nframes, f32 frame_dt,
                         f32 w, f32 h);
