/*
 * projectile.c - Thrown-weapon projectiles (knife, dynamite)
 * See projectile.h for the olwin.exe RE references.
 */
#include "projectile.h"
#include "collision.h"
#include <math.h>
#include <string.h>

void projectile_init(ProjectileSystem *ps) {
    memset(ps->list, 0, sizeof(ps->list));
}

Projectile *projectile_throw(ProjectileSystem *ps, ProjKind kind,
                             Vec3 origin, f32 yaw, f32 pitch,
                             Vec3 inherit_vel, f32 power, bool lit,
                             f32 fuse_remaining) {
    Projectile *p = NULL;
    for (int i = 0; i < MAX_PROJECTILES; i++)
        if (!ps->list[i].active) { p = &ps->list[i]; break; }
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));

    f32 speed  = (kind == PROJ_KNIFE ? PROJ_KNIFE_SPEED : PROJ_TNT_SPEED) * power;
    if (speed < PROJ_MIN_THROW_SPEED) speed = PROJ_MIN_THROW_SPEED;
    f32 tpitch = (kind == PROJ_KNIFE ? PROJ_KNIFE_PITCH : PROJ_TNT_PITCH)
                 * OL_DEG2RAD;
    f32 pt = pitch + tpitch;

    p->active = true;
    p->kind   = kind;
    p->tdo    = -1;
    p->pos    = origin;
    p->vel.x  = cosf(yaw) * cosf(pt) * speed + inherit_vel.x;
    p->vel.z  = sinf(yaw) * cosf(pt) * speed + inherit_vel.z;
    p->vel.y  = sinf(pt) * speed + inherit_vel.y;
    p->yaw    = yaw;
    p->spin   = (6.0f + 6.0f * power);   /* randomized tumble ×power */
    p->lit    = lit;
    p->fuse   = lit ? fuse_remaining : 0.0f;
    p->sector = -1;
    return p;
}

static void proj_explode(ProjectileSystem *ps, Projectile *p) {
    Vec3 at = p->pos;
    p->active = false;
    if (ps->play_sfx) ps->play_sfx("explode", at);
    if (ps->on_explode) ps->on_explode(at);
    /* Chain reaction: nearby resting dynamite detonates too. */
    projectile_chain(ps, at, PROJ_TNT_BLAST_R);
}

void projectile_spawn_fx(ProjectileSystem *ps, Vec3 pos,
                         const u32 *frames, u32 nframes, f32 frame_dt,
                         f32 w, f32 h) {
    if (!nframes) return;
    Projectile *p = NULL;
    for (int i = 0; i < MAX_PROJECTILES; i++)
        if (!ps->list[i].active) { p = &ps->list[i]; break; }
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->active = true;
    p->kind   = PROJ_FX;
    p->tdo    = -1;
    p->pos    = pos;
    p->resting = true;   /* no physics */
    p->fx_nframes = (nframes < 16) ? nframes : 16;
    for (u32 f = 0; f < p->fx_nframes; f++) p->fx_frames[f] = frames[f];
    p->fx_dt  = (frame_dt > 0.0f) ? frame_dt : 0.08f;
    p->tex    = frames[0];
    p->w = w; p->h = h;
}

void projectile_update(ProjectileSystem *ps, const LvtLevel *lvt, f32 dt) {
    if (dt > 0.25f) dt = 0.25f;
    for (int i = 0; i < MAX_PROJECTILES; i++) {
        Projectile *p = &ps->list[i];
        if (!p->active) continue;

        /* One-shot FX animation */
        if (p->kind == PROJ_FX) {
            p->fx_timer += dt;
            u32 fr = (u32)(p->fx_timer / p->fx_dt);
            if (fr >= p->fx_nframes) { p->active = false; continue; }
            p->fx_frame = fr;
            p->tex = p->fx_frames[fr];
            continue;
        }

        /* Fuse runs regardless of state (held/flying/resting). */
        if (p->lit) {
            p->fuse -= dt;
            if (p->fuse <= 0.0f) { proj_explode(ps, p); continue; }
        }
        if (p->resting) continue;

        /* Sector gravity (default -60) */
        f32 gravity = -60.0f;
        if (p->sector >= 0 && p->sector < (i32)lvt->sector_count)
            gravity = lvt->sectors[p->sector].gravity;
        p->vel.y += gravity * dt;

        Vec3 from = p->pos;
        Vec3 to = { p->pos.x + p->vel.x * dt,
                    p->pos.y + p->vel.y * dt,
                    p->pos.z + p->vel.z * dt };

        /* Wall collision: slide/stop with speed halved (0x50052c). */
        f32 radius = (p->kind == PROJ_KNIFE) ? PROJ_KNIFE_RADIUS : PROJ_TNT_RADIUS;
        int sec = (p->sector >= 0) ? p->sector
                  : collision_find_sector_y(lvt, from.x, from.z, from.y, 1.0f, 0);
        Vec3 res = collision_resolve(lvt, from, to, radius * 0.5f, 1.0f, &sec);
        if (fabsf(res.x - to.x) > 1e-4f || fabsf(res.z - to.z) > 1e-4f) {
            /* Hit a wall: keep the resolver's tangential motion, halve speed */
            f32 inv_dt = (dt > 1e-6f) ? 1.0f / dt : 0.0f;
            p->vel.x = (res.x - from.x) * inv_dt * PROJ_WALL_DAMP;
            p->vel.z = (res.z - from.z) * inv_dt * PROJ_WALL_DAMP;
        }
        p->pos.x = res.x;
        p->pos.z = res.z;
        p->pos.y = to.y;
        p->sector = sec;

        /* Floor / ceiling bounce (Physics_FloorBounce @0x4e2c50):
         * restitution = sector ELASTICITY (LVT default 0.3). */
        f32 floor_y = 0, ceil_y = 256;
        collision_heights(lvt, sec, p->pos.x, p->pos.z, &floor_y, &ceil_y);
        f32 elast = 0.3f;
        if (sec >= 0 && sec < (i32)lvt->sector_count) {
            f32 e = lvt->sectors[sec].elasticity;
            if (e >= 0.0f && e <= 1.0f) elast = e;
        }
        if (p->pos.y <= floor_y) {
            p->pos.y = floor_y;
            if (p->vel.y < 0.0f) {
                p->vel.y = -p->vel.y * elast;
                p->spin *= -0.5f;                 /* tumble ×(-0.5) on bounce */
                if (p->vel.y < 1.0f) p->vel.y = 0.0f;
            }
        }
        if (p->pos.y + 0.5f >= ceil_y && p->vel.y > 0.0f) {
            p->pos.y = ceil_y - 0.5f;
            p->vel.y = 0.0f;
        }

        /* Rolling friction when grounded (×0.75/tick below |vy|<0.1) */
        if (fabsf(p->vel.y) < 0.1f && p->pos.y <= floor_y + 0.01f) {
            p->vel.x *= PROJ_ROLL_FRICTION;
            p->vel.z *= PROJ_ROLL_FRICTION;
        }

        p->tumble += p->spin * dt;   /* end-over-end tumble (yaw = heading) */

        /* Rest detection: |v| < 0.1 → knife/unlit dynamite becomes a ground
         * item; a LIT stick keeps its fuse running while resting. */
        f32 v2 = p->vel.x*p->vel.x + p->vel.y*p->vel.y + p->vel.z*p->vel.z;
        if (v2 < PROJ_REST_SPEED * PROJ_REST_SPEED &&
            p->pos.y <= floor_y + 0.05f) {
            p->resting = true;
            p->spin = 0.0f;
            p->vel = (Vec3){0, 0, 0};
            /* Settle lying flat on the ground */
            p->tumble = roundf(p->tumble / OL_PI) * OL_PI;
        }
    }
}

int projectile_raycast(const ProjectileSystem *ps, Vec3 origin, Vec3 dir,
                       f32 max_dist, f32 *hit_dist) {
    int best = -1;
    f32 best_d = max_dist;
    for (int i = 0; i < MAX_PROJECTILES; i++) {
        const Projectile *p = &ps->list[i];
        if (!p->active) continue;
        /* Only dynamite is meaningfully shootable (gdynam dispatch). */
        if (p->kind != PROJ_DYNAMITE) continue;
        f32 rx = origin.x - p->pos.x, rz = origin.z - p->pos.z;
        f32 r = 1.5f;   /* generous target for a small stick */
        f32 a = dir.x*dir.x + dir.z*dir.z;
        f32 b = 2.0f*(rx*dir.x + rz*dir.z);
        f32 c = rx*rx + rz*rz - r*r;
        f32 disc = b*b - 4.0f*a*c;
        if (disc < 0.0f || a < 1e-6f) continue;
        f32 t = (-b - sqrtf(disc)) / (2.0f*a);
        if (t < 0.0f || t > best_d) continue;
        f32 hy = origin.y + dir.y * t;
        if (hy < p->pos.y - 0.5f || hy > p->pos.y + 1.5f) continue;
        best_d = t;
        best = i;
    }
    if (hit_dist) *hit_dist = best_d;
    return best;
}

void projectile_damage(ProjectileSystem *ps, int idx) {
    if (idx < 0 || idx >= MAX_PROJECTILES) return;
    Projectile *p = &ps->list[idx];
    if (!p->active || p->kind != PROJ_DYNAMITE) return;
    /* shot_ProjectileDispatch msg 0x7D1: incoming damage > 0 on an object
     * whose def has an EXPLOSION → detonate. */
    proj_explode(ps, p);
}

void projectile_chain(ProjectileSystem *ps, Vec3 pos, f32 radius) {
    for (int i = 0; i < MAX_PROJECTILES; i++) {
        Projectile *p = &ps->list[i];
        if (!p->active || p->kind != PROJ_DYNAMITE) continue;
        f32 dx = p->pos.x - pos.x, dy = p->pos.y - pos.y, dz = p->pos.z - pos.z;
        f32 d2 = dx*dx + dy*dy + dz*dz;
        if (d2 > radius * radius || d2 < 0.01f) continue;
        /* Slight fuse so chains propagate visibly instead of one megablast */
        if (!p->lit) { p->lit = true; p->fuse = 0.12f; }
    }
}

ProjKind projectile_try_pickup(ProjectileSystem *ps, Vec3 player_pos) {
    for (int i = 0; i < MAX_PROJECTILES; i++) {
        Projectile *p = &ps->list[i];
        if (!p->active || !p->resting || p->lit) continue;
        f32 dx = p->pos.x - player_pos.x, dz = p->pos.z - player_pos.z;
        f32 dy = p->pos.y - player_pos.y;
        if (dx*dx + dz*dz < 6.0f*6.0f && fabsf(dy) < 6.0f) {
            ProjKind k = p->kind;
            p->active = false;
            return k;
        }
    }
    return PROJ_NONE;
}
