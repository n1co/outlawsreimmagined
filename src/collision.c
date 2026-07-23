/*
 * collision.c - Sector-based collision detection
 */
#include "collision.h"
#include <math.h>

/* -------------------------------------------------------------------------
 * Point-in-sector (2D ray casting in XZ plane)
 * Uses the walls as polygon edges. Returns true if (x,z) is inside.
 * ---------------------------------------------------------------------- */
static bool point_in_sector(const LvtSector *sec, f32 x, f32 z) {
    int crossings = 0;
    for (u32 i = 0; i < sec->wall_count; i++) {
        const LvtWall *w = &sec->walls[i];
        if (w->v1 < 0 || w->v1 >= (i32)sec->vertex_count) continue;
        if (w->v2 < 0 || w->v2 >= (i32)sec->vertex_count) continue;
        f32 x0 = sec->vertices[w->v1].x, z0 = sec->vertices[w->v1].y;
        f32 x1 = sec->vertices[w->v2].x, z1 = sec->vertices[w->v2].y;
        /* Cast ray in +X direction from (x,z) */
        if ((z0 <= z && z < z1) || (z1 <= z && z < z0)) {
            f32 t = (z - z0) / (z1 - z0);
            if (x < x0 + t * (x1 - x0))
                crossings++;
        }
    }
    return (crossings & 1) != 0;
}

/* Deepest water floor at (x,z): the bottom of the water column under the player.
 * Outlaws water is two stacked, x/z-overlapping sectors — a thin surface slab
 * (floor = waterline) directly above a deep volume (ceiling = waterline). They
 * are NOT wall-adjoined, so to know how deep the player may sink we scan every
 * water sector whose polygon contains (x,z) and take the lowest floor. Returns
 * `fallback` (the surface) when there is no deeper water below (a shallow slab
 * you just wade on). */
f32 collision_water_bottom_at(const LvtLevel *level, f32 x, f32 z, f32 fallback) {
    if (!level) return fallback;
    f32 bottom = fallback;
    for (u32 i = 0; i < level->sector_count; i++) {
        const LvtSector *s = &level->sectors[i];
        if (!s->is_water) continue;
        f32 f = lvt_floor_at(s, x, z);
        if (f < bottom && point_in_sector(s, x, z)) bottom = f;
    }
    return bottom;
}

/* Forward declaration */
static bool wall_is_solid(const LvtLevel *level, i32 si, u32 wi, f32 y, f32 height);

/* Squared distance from point (px,pz) to segment (ax,az)-(bx,bz). */
static f32 point_seg_dist2(f32 px, f32 pz, f32 ax, f32 az, f32 bx, f32 bz) {
    f32 dx = bx - ax, dz = bz - az;
    f32 l2 = dx*dx + dz*dz;
    f32 t = (l2 > 1e-8f) ? ((px-ax)*dx + (pz-az)*dz) / l2 : 0.0f;
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    f32 cx = ax + t*dx, cz = az + t*dz;
    f32 ex = px - cx, ez = pz - cz;
    return ex*ex + ez*ez;
}

/* Supporting floor for a player of `radius` at (x,z) with feet at `feet`.
 * Outlaws (Sector_CanFitRecursive @0x4e6660 / Wall_CircleIntersects) stands the
 * player on the HIGHEST floor any part of their radius-circle overlaps — not the
 * floor under their center point. This is what lets you stand on and walk along
 * a wall-top / muret only ~1u wide (the cylinder is 3u across) instead of sliding
 * off the instant your center leaves it, and what smoothly lifts you onto a low
 * wall as you approach. Bounded to floors within STEP_HEIGHT of the feet (a floor
 * higher than that is a wall you can't be standing on). Considers the center
 * sector and each neighbour whose shared wall the circle reaches through a
 * passable (non-solid) portal. */
f32 collision_support_floor(const LvtLevel *level, f32 x, f32 z, f32 feet,
                            f32 radius, int center, f32 height) {
    if (!level || center < 0 || center >= (i32)level->sector_count) return feet;
    const LvtSector *c = &level->sectors[center];
    f32 best  = lvt_floor_at(c, x, z);
    f32 limit = feet + COL_STEP_HEIGHT;
    f32 r2    = radius * radius;
    for (u32 wi = 0; wi < c->wall_count; wi++) {
        const LvtWall *w = &c->walls[wi];
        i32 adj = w->adjoin;
        if (adj < 0 || adj >= (i32)level->sector_count) continue;
        if (w->v1 < 0 || w->v2 < 0 ||
            w->v1 >= (i32)c->vertex_count || w->v2 >= (i32)c->vertex_count) continue;
        f32 ax = c->vertices[w->v1].x, az = c->vertices[w->v1].y;
        f32 bx = c->vertices[w->v2].x, bz = c->vertices[w->v2].y;
        if (point_seg_dist2(x, z, ax, az, bx, bz) > r2) continue;   /* circle can't reach it */
        if (wall_is_solid(level, center, wi, feet, height)) continue; /* solid → not standable across */
        f32 af = lvt_floor_at(&level->sectors[adj], x, z);
        if (af <= limit && af > best) best = af;
    }
    return best;
}

int collision_find_sector(const LvtLevel *level, f32 x, f32 z, int hint) {
    return collision_find_sector_y(level, x, z, -9999.0f, 0.0f, hint);
}

/* Vertical fit: does sector `s` admit a player whose feet are at `y`?
 * Standing on the floor (y≈floor) up to the ceiling, with a step-height grace
 * band below the floor (about to step up) and a small grace above the ceiling.
 * This is the Y-gate the Jedi engine's sector_which3D uses to keep stacked
 * (multi-storey) sectors separate. */
static bool sector_admits_y(const LvtSector *s, f32 x, f32 z, f32 y) {
    /* Feet must be on/above the floor (step-up grace) and BELOW the ceiling.
     * Only a tiny numerical epsilon above the ceiling — a generous ceiling grace
     * let an upper-floor player (feet 12.2) be admitted into the sector directly
     * BELOW them (ceil 11.8) at a doorway seam, dropping them through the floor
     * (TOWN hotel upstairs rooms). */
    return y >= lvt_floor_at(s, x, z) - COL_STEP_HEIGHT && y <= lvt_ceil_at(s, x, z) + 0.1f;
}

static f32 sector_bbox_area(const LvtSector *s) {
    f32 minx = 1e30f, maxx = -1e30f, minz = 1e30f, maxz = -1e30f;
    for (u32 v = 0; v < s->vertex_count; v++) {
        f32 X = s->vertices[v].x, Z = s->vertices[v].y;
        if (X < minx) minx = X;
        if (X > maxx) maxx = X;
        if (Z < minz) minz = Z;
        if (Z > maxz) maxz = Z;
    }
    if (maxx <= minx || maxz <= minz) return 1e30f;
    return (maxx - minx) * (maxz - minz);
}

/*
 * Determine the sector the player occupies. Follows the Jedi-engine model
 * (see TFE sector_which3D / playerMove): the current sector is tracked
 * incrementally from the previous one (`hint`) and only changes by crossing a
 * passable adjoin; a global search is a Y-gated, smallest-area fallback. This
 * replaces the old area-blind point-in-polygon that mis-picked stacked sectors
 * (multi-floor buildings), causing "teleport down into the floor below".
 */
int collision_find_sector_y(const LvtLevel *level, f32 x, f32 z, f32 y, f32 height, int hint) {
    if (!level || level->sector_count == 0) return 0;
    bool have_y = (y > -9000.0f);
    (void)height;

    /* 1. Stay in the current sector while the player is still inside it in XZ
     *    AND vertically consistent with it. If they have fallen below its floor
     *    or risen above its ceiling, fall through and re-locate. */
    if (hint >= 0 && hint < (i32)level->sector_count &&
        point_in_sector(&level->sectors[hint], x, z)) {
        if (!have_y || sector_admits_y(&level->sectors[hint], x, z, y))
            return hint;
    }

    /* 2. Portal traversal (the authoritative Jedi model for MOVEMENT): starting
     *    from the current sector, walk the graph of PASSABLE adjoins (BFS) and
     *    take the sector that contains (x,z) and admits the player's Y, preferring
     *    the highest floor at/below the feet (the surface being stood on). This
     *    is what stops the player being captured by a huge overlapping "shell"
     *    sector during movement — such a sector is only entered if you actually
     *    cross a portal into it, never by a global position query. */
    if (hint >= 0 && hint < (i32)level->sector_count) {
        #define COL_BFS_MAX 256
        i32 queue[COL_BFS_MAX]; u32 qh = 0, qt = 0;
        i32 seen[COL_BFS_MAX]; u32 nseen = 0;
        queue[qt++] = hint; seen[nseen++] = hint;
        int best = -1; f32 best_floor = -1e30f;
        while (qh < qt) {
            i32 si = queue[qh++];
            const LvtSector *s = &level->sectors[si];
            /* Is the point inside this reachable sector at the right height? */
            if (point_in_sector(s, x, z) && (!have_y || sector_admits_y(s, x, z, y))) {
                f32 f = lvt_floor_at(s, x, z);
                if (f <= (have_y ? y + COL_STEP_HEIGHT : 1e30f) && f > best_floor) {
                    best_floor = f; best = si;
                }
            }
            /* Expand through passable walls. */
            for (u32 wi = 0; wi < s->wall_count && qt < COL_BFS_MAX; wi++) {
                i32 adj = s->walls[wi].adjoin;
                if (adj < 0 || adj >= (i32)level->sector_count) continue;
                if (wall_is_solid(level, si, wi, y, height)) continue;
                bool dup = false;
                for (u32 k = 0; k < nseen; k++) if (seen[k] == adj) { dup = true; break; }
                if (dup || nseen >= COL_BFS_MAX) continue;
                seen[nseen++] = adj; queue[qt++] = adj;
            }
        }
        /* Nested-sector refinement: a sub-sector may contain (x,z) at this height
         * WITHOUT being reachable via a passable adjoin from the current sector —
         * e.g. buildings/stairs nested inside a big AMBIENT courtyard on HIDEOUT.
         * Portal-BFS alone leaves the player stuck in the big shell. Pick the
         * sector the player is actually STANDING ON: among sectors containing
         * (x,z) that admit the player's Y, the one with the HIGHEST floor at/below
         * the feet (surface stood on), breaking ties by smallest area.
         * CRITICAL: keying on the highest walkable floor — NOT merely smallest
         * area — is what stops the player teleporting DOWN into a smaller stacked
         * sector below them (TOWN hotel rooms / walking under stairs). */
        if (have_y) {
            int refined = best;
            f32 best_f = (best >= 0) ? lvt_floor_at(&level->sectors[best], x, z) : -1e30f;
            f32 best_a = (best >= 0) ? sector_bbox_area(&level->sectors[best]) : 1e30f;
            for (u32 i = 0; i < level->sector_count; i++) {
                const LvtSector *s = &level->sectors[i];
                if (!sector_admits_y(s, x, z, y)) continue;
                if (!point_in_sector(s, x, z)) continue;
                f32 f = lvt_floor_at(s, x, z);
                if (f > y + COL_STEP_HEIGHT) continue;   /* floor above feet: can't stand on it */
                f32 a = sector_bbox_area(s);
                if (f > best_f + 0.1f || (f > best_f - 0.1f && a < best_a)) {
                    best_f = f; best_a = a; refined = (int)i;
                }
            }
            if (refined >= 0) return refined;
        }
        if (best >= 0) return best;
        /* No reachable sector contains the player — they slid against a wall.
         * Stay put rather than snapping into an unrelated overlapping sector. */
        return hint;
    }

    /* 3. Global fallback — ONLY for spawn/teleport (no valid hint). Among all
     *    sectors that contain (x,z) and admit the player's Y, pick the
     *    smallest-area one (innermost / most specific sector wins). */
    if (have_y) {
        int best = -1; f32 best_area = 1e30f;
        for (u32 i = 0; i < level->sector_count; i++) {
            const LvtSector *s = &level->sectors[i];
            if (!sector_admits_y(s, x, z, y)) continue;
            if (!point_in_sector(s, x, z)) continue;
            f32 area = sector_bbox_area(s);
            if (area < best_area) { best_area = area; best = (int)i; }
        }
        if (best >= 0) return best;
    }
    for (u32 i = 0; i < level->sector_count; i++)
        if (point_in_sector(&level->sectors[i], x, z))
            return (int)i;
    return 0;
}

/* -------------------------------------------------------------------------
 * Closest point on segment (ax,az)-(bx,bz) to point (px,pz).
 * Writes outward normal to *nx, *nz. Returns squared distance.
 * ---------------------------------------------------------------------- */
static f32 seg_closest_sq(f32 px, f32 pz,
                           f32 ax, f32 az, f32 bx, f32 bz,
                           f32 *nx, f32 *nz) {
    f32 dx = bx - ax, dz = bz - az;
    f32 len2 = dx*dx + dz*dz;
    f32 t = (len2 > 1e-6f) ? ((px-ax)*dx + (pz-az)*dz) / len2 : 0.0f;
    t = OL_CLAMP(t, 0.0f, 1.0f);
    f32 cx = ax + t*dx, cz = az + t*dz;
    *nx = px - cx; *nz = pz - cz;
    return (*nx)*(*nx) + (*nz)*(*nz);
}

/* -------------------------------------------------------------------------
 * Check whether a wall in sector `si` is solid given player Y and height.
 * ---------------------------------------------------------------------- */
static bool wall_is_solid(const LvtLevel *level, i32 si, u32 wi,
                           f32 y, f32 height) {
    const LvtSector *sec = &level->sectors[si];
    const LvtWall   *w   = &sec->walls[wi];

    /* Evaluate floor/ceiling heights at the wall MIDPOINT so sloped sectors
     * (ramps/stairs) are gated by their height at the crossing, not their flat
     * base — otherwise a ramp whose base floor_y differs from its height at the
     * shared edge walls off the top of the ramp / the landing seam and shoves
     * the player back down. */
    f32 wmx = 0.0f, wmz = 0.0f;
    if (w->v1 >= 0 && w->v1 < (i32)sec->vertex_count &&
        w->v2 >= 0 && w->v2 < (i32)sec->vertex_count) {
        wmx = (sec->vertices[w->v1].x + sec->vertices[w->v2].x) * 0.5f;
        wmz = (sec->vertices[w->v1].y + sec->vertices[w->v2].y) * 0.5f;
    }
    f32 sec_floor = lvt_floor_at(sec, wmx, wmz);
    f32 sec_ceil  = lvt_ceil_at(sec, wmx, wmz);

    /* Walls are NOT infinitely tall. A wall only exists over its sector's
     * [floor, ceil] span; if the player's body is entirely above or below that
     * span they pass under/over it. This is what lets you walk through a
     * ground-level doorway beneath an upper-storey wall (e.g. the church nave
     * under its stacked steeple facade sector) instead of hitting an invisible
     * wall. Only applies when a real Y is supplied (movement, not 2D queries). */
    if (y > -9000.0f) {
        if (sec_floor > y + height) return false; /* wall entirely overhead */
        if (sec_ceil  < y)          return false; /* wall entirely underfoot */
    }

    if (w->adjoin < 0) return true;
    if (w->adjoin >= (i32)level->sector_count) return true;

    /* Open morph door: the leaf (this sector) or the door on the other side has
     * swung aside — let the player walk through the doorway. */
    if (sec->door_open) return false;
    if (level->sectors[w->adjoin].door_open) return false;

    /* Closed flag-door (auto mask-scroll door, sector flag 0x100): its panel
     * blocks until opened. Without this the panel's ADJOIN_MID wall would fall
     * through to the geometric check and read as an open portal. */
    if (sec->is_flag_door) return true;
    if (level->sectors[w->adjoin].is_flag_door) return true;

    /* WF3_SOLID_WALL (flags2 bit 1): explicitly solid. */
    if (w->flags2 & 0x02u)
        return true;

    /* WALL_NON_SOLID (flags bit 9, 0x200): always passable.
     * RE docs: "Player and projectiles pass through." */
    if (w->flags & 0x200u)
        return false;

    /* ADJOIN_MID walls (flag 0x2000): the 0x2000 bit does NOT by itself mean
     * "solid fence". Empirically (SCANMID sweep, 2026-07) EVERY 0x2000 wall
     * whose span is fully open (adjoin floor & ceil == this sector's) is a
     * plain open portal — never flagged WF3_SOLID_WALL, never a window — yet
     * treating 0x2000+dadjoin=-1 as a solid fence sealed whole sectors into
     * boxes (CANYON sec 131 = 17 phantom walls, HIDEOUT = 86). Passability is
     * therefore decided GEOMETRICALLY below, exactly like a normal adjoin.
     * The only genuine solid masks are:
     *   - WF3_SOLID_WALL (flags2 & 0x02) — handled above,
     *   - breakable glass windows (is_window) — blocked here until shot out.
     * Arches remain passable: their opening is a same-floor portal (geometric
     * pass) and/or carries a dadjoin (handled by the DADJOIN branch below). */
    if (w->flags & LVT_WALL_FLAG_ADJOIN_MID) {
        if (w->is_window && !w->window_broken)
            return true; /* intact glass window: solid until broken */
        /* else: fall through to the geometric step/headroom check */
    }

    const LvtSector *adj = &level->sectors[w->adjoin];
    f32 adj_floor = lvt_floor_at(adj, wmx, wmz);   /* slope-aware at the crossing */
    f32 adj_ceil  = lvt_ceil_at(adj, wmx, wmz);

    /* For DADJOIN walls (3-sector portals like BANK door), check BOTH
     * portals. The main adjoin may have a high floor (upper level) but
     * the dadjoin provides a passable lower portal.
     * TFE RE: collision uses the traversable opening, not just adjoin. */
    if (w->dadjoin >= 0 && w->dadjoin < (i32)level->sector_count) {
        const LvtSector *dadj = &level->sectors[w->dadjoin];
        /* Use the floor closest to the player's current Y */
        f32 dadj_floor = lvt_floor_at(dadj, wmx, wmz);
        f32 dadj_ceil  = lvt_ceil_at(dadj, wmx, wmz);

        /* Check if player fits through the lower portal (dadjoin) */
        f32 dadj_step = dadj_floor - y;
        f32 dadj_head = dadj_ceil - (y + height);
        if (dadj_step <= COL_STEP_HEIGHT && dadj_head >= 0.0f)
            return false; /* Can pass through lower portal */

        /* Check if player fits through the upper portal (adjoin) */
        f32 adj_step = adj_floor - y;
        f32 adj_head = adj_ceil - (y + height);
        if (adj_step <= COL_STEP_HEIGHT && adj_head >= 0.0f)
            return false; /* Can pass through upper portal */

        return true; /* Neither portal is passable */
    }

    /* Headroom check */
    f32 headroom = adj_ceil - (y + height);
    if (headroom < 0.0f)
        return true;

    /* Step-up check. A wall is a climbable step if the far floor is within
     * STEP_HEIGHT of EITHER the player's feet OR THIS sector's floor at the
     * crossing. The sector-floor test is what makes staircases work: the real
     * engine (Physics_SlideMove) subdivides the move and raises the feet onto
     * each step as it crosses, so the next riser is always measured from the
     * step you're standing on — not your global feet Y. Without it, the player
     * radius (1.5) overlaps steps two/three ahead whose risers, measured from
     * the low starting floor, exceed STEP and wall the player in place.
     * The feet-Y test is still needed so you can land on a ledge mid-jump. */
    f32 step_up      = adj_floor - y;
    f32 step_up_sec  = adj_floor - sec_floor;
    if (step_up > COL_STEP_HEIGHT && step_up_sec > COL_STEP_HEIGHT)
        return true;

    return false;
}

/* -------------------------------------------------------------------------
 * Push player out of a single sector's solid walls.
 * `from_x/from_z`: player's position BEFORE this frame's movement — used to
 * determine which side of the wall the player came from so the push is always
 * directed back toward the original side (prevents wall-tunneling push inversion).
 * ---------------------------------------------------------------------- */
static void push_from_sector(const LvtLevel *level, i32 si,
                              f32 from_x, f32 from_z,
                              f32 *x, f32 *z, f32 y,
                              f32 radius, f32 height) {
    if (si < 0 || si >= (i32)level->sector_count) return;
    const LvtSector *sec = &level->sectors[si];

    for (u32 wi = 0; wi < sec->wall_count; wi++) {
        const LvtWall *w = &sec->walls[wi];
        if (w->v1 < 0 || w->v1 >= (i32)sec->vertex_count) continue;
        if (w->v2 < 0 || w->v2 >= (i32)sec->vertex_count) continue;
        if (!wall_is_solid(level, si, wi, y, height)) continue;

        f32 x0 = sec->vertices[w->v1].x, z0 = sec->vertices[w->v1].y;
        f32 x1 = sec->vertices[w->v2].x, z1 = sec->vertices[w->v2].y;

        /* Closest point on wall segment to current player position */
        f32 nx, nz;
        f32 dsq = seg_closest_sq(*x, *z, x0, z0, x1, z1, &nx, &nz);
        f32 d   = sqrtf(dsq);
        if (d < 1e-5f) continue;  /* degenerate: player exactly on wall */

        if (d < radius) {
            /* `nx, nz` points from the wall toward the player (current pos).
             * Verify this direction agrees with the "from" side.  If the player
             * tunneled through the wall this frame, `nx,nz` would point the
             * wrong way — in that case flip it so we push back to the origin side. */
            f32 from_dot = (from_x - *x + nx) * nx + (from_z - *z + nz) * nz;
            /* from_dot > 0 means `from` is on the same side as nx,nz → push OK.
             * from_dot < 0 means the player crossed the wall → flip push.      */
            f32 sign = (from_dot >= 0.0f) ? 1.0f : -1.0f;
            f32 push = (radius - d) / d;
            *x += sign * nx * push;
            *z += sign * nz * push;
        }
    }
}

/* -------------------------------------------------------------------------
 * Slide movement from `from` toward `to` against a solid wall segment.
 * If the swept circle (radius R moving from→to) would penetrate the wall,
 * clips `to` so the circle stops at distance R from the wall and the
 * remaining velocity is projected onto the wall tangent (sliding).
 * ---------------------------------------------------------------------- */
static void slide_against_wall(f32 from_x, f32 from_z,
                                f32 *to_x, f32 *to_z,
                                f32 ax, f32 az, f32 bx, f32 bz,
                                f32 radius) {
    /* Wall tangent and perpendicular (unit vectors) */
    f32 wdx = bx - ax, wdz = bz - az;
    f32 wlen2 = wdx*wdx + wdz*wdz;
    if (wlen2 < 1e-8f) return;
    f32 wlen = sqrtf(wlen2);
    /* Outward normal candidate (perpendicular, CCW of tangent) */
    f32 wnx = -wdz / wlen, wnz =  wdx / wlen;

    /* Signed distances from wall line */
    f32 from_d = (from_x - ax)*wnx + (from_z - az)*wnz;
    f32 to_d   = (*to_x  - ax)*wnx + (*to_z  - az)*wnz;

    /* Ensure the normal points toward `from` (the "safe" side) */
    if (from_d < 0.0f) { wnx = -wnx; wnz = -wnz; from_d = -from_d; to_d = -to_d; }

    /* `from` side check: from_d > 0, player approaching wall from the outside */
    if (from_d < radius) return;  /* already too close at start: skip (handled by push) */
    if (to_d >= radius)  return;  /* destination still safe: no clipping needed */

    /* Closest point on wall segment to the collision point */
    f32 col_x = *to_x + wnx * (radius - to_d);
    f32 col_z = *to_z + wnz * (radius - to_d);
    f32 seg_t = ((col_x - ax)*wdx + (col_z - az)*wdz) / wlen2;
    if (seg_t < -0.05f || seg_t > 1.05f) return;   /* collision outside wall extent */

    /* Clip `to` to keep the circle at distance `radius` from the wall */
    *to_x += wnx * (radius - to_d);
    *to_z += wnz * (radius - to_d);
}

/* -------------------------------------------------------------------------
 * Apply sliding collision for all solid walls in a sector.
 * ---------------------------------------------------------------------- */
static void slide_from_sector(const LvtLevel *level, i32 si,
                               f32 from_x, f32 from_z,
                               f32 *to_x, f32 *to_z,
                               f32 y, f32 radius, f32 height) {
    if (si < 0 || si >= (i32)level->sector_count) return;
    const LvtSector *sec = &level->sectors[si];
    for (u32 wi = 0; wi < sec->wall_count; wi++) {
        const LvtWall *w = &sec->walls[wi];
        if (w->v1 < 0 || w->v1 >= (i32)sec->vertex_count) continue;
        if (w->v2 < 0 || w->v2 >= (i32)sec->vertex_count) continue;
        if (!wall_is_solid(level, si, wi, y, height)) continue;
        f32 x0 = sec->vertices[w->v1].x, z0 = sec->vertices[w->v1].y;
        f32 x1 = sec->vertices[w->v2].x, z1 = sec->vertices[w->v2].y;
        slide_against_wall(from_x, from_z, to_x, to_z, x0, z0, x1, z1, radius);
    }
}

/* -------------------------------------------------------------------------
 * Resolve movement
 * ---------------------------------------------------------------------- */
Vec3 collision_resolve(const LvtLevel *level, Vec3 from, Vec3 to,
                       f32 radius, f32 height, int *sector_idx) {
    if (!level) return to;

    int si = (*sector_idx >= 0 && *sector_idx < (i32)level->sector_count)
             ? *sector_idx : 0;

    f32 x = to.x, z = to.z;

    /* Pass 1 — sliding pre-clip: prevent tunneling through walls.
     * Check from→to against all solid walls in current + 1st-order adjacent
     * sectors.  Clip `to` so the circle never crosses a wall.             */
    slide_from_sector(level, si, from.x, from.z, &x, &z, to.y, radius, height);
    {
        const LvtSector *sec = &level->sectors[si];
        for (u32 wi = 0; wi < sec->wall_count; wi++) {
            i32 adj = sec->walls[wi].adjoin;
            if (adj < 0 || adj >= (i32)level->sector_count) continue;
            slide_from_sector(level, adj, from.x, from.z, &x, &z, to.y, radius, height);
        }
    }

    /* Pass 2 — push correction: expel from any remaining penetration.
     * Three iterations for stability with corners/concave geometry.       */
    for (int pass = 0; pass < 3; pass++) {
        if (si < 0 || si >= (i32)level->sector_count) break;

        push_from_sector(level, si, from.x, from.z, &x, &z, to.y, radius, height);

        const LvtSector *sec = &level->sectors[si];
        for (u32 wi = 0; wi < sec->wall_count; wi++) {
            i32 adj = sec->walls[wi].adjoin;
            if (adj < 0 || adj >= (i32)level->sector_count) continue;
            push_from_sector(level, adj, from.x, from.z, &x, &z, to.y, radius, height);

            const LvtSector *adj_sec = &level->sectors[adj];
            for (u32 wj = 0; wj < adj_sec->wall_count; wj++) {
                i32 adj2 = adj_sec->walls[wj].adjoin;
                if (adj2 >= 0 && adj2 < (i32)level->sector_count)
                    push_from_sector(level, adj2, from.x, from.z, &x, &z,
                                     to.y, radius, height);
            }
        }
    }

    /* Update current sector */
    *sector_idx = collision_find_sector_y(level, x, z, to.y, height, si);
    return (Vec3){ x, to.y, z };
}

/* -------------------------------------------------------------------------
 * 2D line-of-sight check using sector portals.
 *
 * Traces a 2D ray from (ax,az) to (bx,bz) through the sector/portal graph.
 * Returns true if the ray can reach the target without being blocked by a
 * solid wall. The height parameters (ay, by) are checked against portal
 * floor/ceiling openings so enemies can't "see" through floor/ceiling gaps.
 * ---------------------------------------------------------------------- */
bool collision_has_los(const LvtLevel *level, f32 ax, f32 az, f32 ay,
                       f32 bx, f32 bz, f32 by) {
    if (!level || level->sector_count == 0) return false;

    /* Find the starting sector 3D-aware: among sectors that contain (ax,az) in
     * 2D, pick the one whose floor..ceiling range contains the eye height ay.
     * (A plain 2D find can pick a stacked sector at the wrong height.) */
    int cur = -1;
    {
        f32 best = 1e18f;
        for (u32 si = 0; si < level->sector_count; si++) {
            const LvtSector *s = &level->sectors[si];
            if (!point_in_sector(s, ax, az)) continue;
            if (ay >= s->floor_y - 1.0f && ay <= s->ceil_y + 1.0f) { cur = (i32)si; break; }
            f32 d = (ay < s->floor_y) ? (s->floor_y - ay) : (ay - s->ceil_y);
            if (d < best) { best = d; cur = (i32)si; }
        }
        if (cur < 0) cur = collision_find_sector(level, ax, az, 0);
    }

    /* Ray direction */
    f32 rdx = bx - ax, rdz = bz - az;
    f32 ray_len = sqrtf(rdx*rdx + rdz*rdz);
    if (ray_len < 0.01f) return true; /* same position */

    /* Walk through sectors along the ray, max 64 portal crossings */
    f32 cx = ax, cz = az;
    for (int step = 0; step < 64; step++) {
        if (cur < 0 || cur >= (i32)level->sector_count) return false;
        const LvtSector *sec = &level->sectors[cur];

        /* Check if target point is in this sector. The 2D test isn't enough:
         * Outlaws stacks sectors that overlap in 2D (balconies, cellars). If the
         * target is 2D-inside but its eye height is outside this sector's
         * floor..ceiling range, it is actually in a vertically-separated sector
         * behind a solid floor/ceiling — no line of sight (prevents enemies
         * shooting through floors/ceilings). */
        if (point_in_sector(sec, bx, bz)) {
            if (by >= sec->floor_y - 1.0f && by <= sec->ceil_y + 1.0f)
                return true;
            return false;
        }

        /* Find the portal wall we exit through */
        f32 best_t = 2.0f;
        i32 best_adj = -1;
        i32 best_wi  = -1;

        for (u32 wi = 0; wi < sec->wall_count; wi++) {
            const LvtWall *w = &sec->walls[wi];
            if (w->v1 < 0 || w->v1 >= (i32)sec->vertex_count) continue;
            if (w->v2 < 0 || w->v2 >= (i32)sec->vertex_count) continue;

            f32 wx0 = sec->vertices[w->v1].x, wz0 = sec->vertices[w->v1].y;
            f32 wx1 = sec->vertices[w->v2].x, wz1 = sec->vertices[w->v2].y;

            /* 2D ray-segment intersection */
            f32 edx = wx1 - wx0, edz = wz1 - wz0;
            f32 denom = rdx * edz - rdz * edx;
            if (fabsf(denom) < 1e-8f) continue;

            f32 t = ((wx0 - cx) * edz - (wz0 - cz) * edx) / denom;
            f32 u = ((wx0 - cx) * rdz - (wz0 - cz) * rdx) / denom;

            if (t < 1e-4f || t > 1.0f + 1e-4f) continue; /* behind or past target */
            if (u < -0.001f || u > 1.001f) continue;       /* outside wall segment */

            if (t < best_t) {
                best_t  = t;
                best_adj = w->adjoin;
                best_wi  = (i32)wi;
            }
        }

        if (best_wi < 0) return false; /* no wall crossed — stuck (shouldn't happen) */

        const LvtWall *bw = &sec->walls[best_wi];

        /* Solid wall (no adjoin): blocks LOS. */
        if (best_adj < 0 || best_adj >= (i32)level->sector_count) return false;

        /* Only an intact glass mask window blocks LOS. A bare open portal does
         * not (0x2000 alone is not a fence). */
        if (bw->is_window && !bw->window_broken)
            return false;

        /* Height gate — the ray must clear an opening at the crossing point.
         * Evaluate heights slope-aware AT the crossing (not the flat base), and
         * consider BOTH the adjoin and the dadjoin openings: an arch / under-a-
         * bridge shot passes through the LOWER (dadjoin) opening even though it
         * would not fit the upper one. LOS succeeds if the ray fits EITHER. */
        f32 hx = ax + best_t * rdx, hz = az + best_t * rdz;
        f32 eye_at_t = ay + best_t * (by - ay);
        f32 sF = lvt_floor_at(sec, hx, hz), sC = lvt_ceil_at(sec, hx, hz);

        i32 openings[2] = { best_adj, bw->dadjoin };
        bool passed = false; i32 through = -1;
        for (int oi = 0; oi < 2 && !passed; oi++) {
            i32 as = openings[oi];
            if (as < 0 || as >= (i32)level->sector_count) continue;
            const LvtSector *adj = &level->sectors[as];
            f32 aF = lvt_floor_at(adj, hx, hz), aC = lvt_ceil_at(adj, hx, hz);
            f32 pf = (sF > aF) ? sF : aF;   /* opening bottom */
            f32 pc = (sC < aC) ? sC : aC;   /* opening top */
            if (pc <= pf) continue;         /* no opening here */
            if (eye_at_t >= pf && eye_at_t <= pc) { passed = true; through = as; }
        }
        if (!passed) return false;

        /* Advance through the opening the ray actually used. */
        cx = hx;
        cz = hz;
        cur = through;
    }
    return false; /* too many portal crossings */
}

/* -------------------------------------------------------------------------
 * Floor/ceiling heights
 * ---------------------------------------------------------------------- */
bool collision_wall_is_solid(const LvtLevel *level, int si, int wi,
                             f32 y, f32 height) {
    if (!level || si < 0 || si >= (i32)level->sector_count) return true;
    if (wi < 0 || wi >= (i32)level->sectors[si].wall_count) return true;
    return wall_is_solid(level, si, (u32)wi, y, height);
}

void collision_heights(const LvtLevel *level, int sector_idx, f32 x, f32 z,
                       f32 *out_floor, f32 *out_ceil) {
    if (!level || sector_idx < 0 || sector_idx >= (i32)level->sector_count) {
        *out_floor = 0.0f;
        *out_ceil  = 256.0f;
        return;
    }
    /* Sloped floors/ceilings (ramps, stairs) return the height at (x,z). */
    *out_floor = lvt_floor_at(&level->sectors[sector_idx], x, z);
    *out_ceil  = lvt_ceil_at(&level->sectors[sector_idx], x, z);
}
