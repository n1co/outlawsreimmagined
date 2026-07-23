/*
 * collision.h - Sector-based collision detection
 *
 * Uses the LVT sector/wall data to resolve player movement:
 *   - Wall collision (solid walls block movement)
 *   - Floor/ceiling clamping
 *   - Portal step height check
 */
#pragma once

#include "engine.h"
#include "lvt.h"

/* Player capsule radius */
#define PLAYER_RADIUS       1.5f
/* Maximum step-up height. RE-verified: AIR.PHY / WATER.PHY / DEFAULT.PHY all
 * carry STEP_HEIGHT: 3.0, and the engine's wall-cross test (FUN_004de6d0) blocks
 * only when (dest_floor - feet) > STEP_HEIGHT. Matches PHY_STEP_HEIGHT (player.h). */
#define COL_STEP_HEIGHT     3.0f

/*
 * Find the sector index that contains 2D point (x, z).
 * hint: sector to check first (optimization for staying in same sector).
 * Returns sector index, or hint (clamped) if not found.
 */
int collision_find_sector(const LvtLevel *level, f32 x, f32 z, int hint);

/* Height-aware version: rejects sectors whose floor is above player_y + body_height */
int collision_find_sector_y(const LvtLevel *level, f32 x, f32 z, f32 y, f32 height, int hint);

/*
 * Resolve movement from 'from' to 'to' against level geometry.
 * radius: player horizontal collision radius
 * height: player height (for step/ceiling checks)
 * sector_idx: in/out current sector index
 * Returns the collision-resolved position (Y not modified by this function).
 */
Vec3 collision_resolve(const LvtLevel *level, Vec3 from, Vec3 to,
                       f32 radius, f32 height, int *sector_idx);

/*
 * Get floor and ceiling heights for a given sector at world point (x,z)
 * (accounts for sloped floors/ceilings — ramps and stairs).
 */
void collision_heights(const LvtLevel *level, int sector_idx, f32 x, f32 z,
                       f32 *out_floor, f32 *out_ceil);

/* Is wall `wi` of sector `si` solid for a player of the given feet-Y and body
 * height? (Exposes the internal wall solidity test for the door test harness.) */
bool collision_wall_is_solid(const LvtLevel *level, int si, int wi,
                             f32 y, f32 height);

/* Deepest water floor at (x,z) — the bottom of the water column under the
 * player, across the two stacked (overlapping) water sectors. Returns
 * `fallback` when there is no deeper water below the point. */
f32 collision_water_bottom_at(const LvtLevel *level, f32 x, f32 z, f32 fallback);

/* Highest floor the player's radius-circle overlaps (center sector + reachable
 * neighbours), bounded to within a step of `feet`. Lets the player stand on and
 * step onto narrow wall-tops/murets instead of sliding off (Sector_CanFitRecursive). */
f32 collision_support_floor(const LvtLevel *level, f32 x, f32 z, f32 feet,
                            f32 radius, int center, f32 height);

/*
 * 2D line-of-sight check through sector portals.
 * Returns true if a ray from (ax,az,ay) to (bx,bz,by) can reach the target
 * without being blocked by a solid wall or closed portal opening.
 */
bool collision_has_los(const LvtLevel *level, f32 ax, f32 az, f32 ay,
                       f32 bx, f32 bz, f32 by);
