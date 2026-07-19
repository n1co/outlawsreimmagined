/*
 * savegame.h - Story campaign save/load (fixed slots, like the original).
 *
 * A save captures the current campaign position (level + progress index) and
 * the player's state (position, health, keys, full weapon/ammo loadout) so the
 * mission can be resumed exactly. Slots are files under saves/ (saveNN.osv).
 */
#pragma once

#include "engine.h"
#include "weapon.h"

#define SAVE_SLOTS 6

typedef struct {
    u32         magic;            /* SAVE_MAGIC */
    u32         version;          /* SAVE_VERSION */
    char        level[64];        /* level file name */
    i32         campaign_active;  /* 1 = story campaign, 0 = single map */
    i32         campaign_idx;     /* index into the campaign order */
    f32         px, py, pz;       /* player position */
    f32         yaw, pitch;
    i32         health;
    i32         keys;
    WeaponState weapons;          /* POD: current, ammo[], clip[], has_weapon[] */
    char        label[40];        /* display name for the slot list */
} SaveGame;

/* Write / read slot 0..SAVE_SLOTS-1. Returns false on I/O error. */
bool savegame_write(int slot, const SaveGame *sg);
bool savegame_read(int slot, SaveGame *sg);

/*
 * Fill out_label with a slot's display name (and return true) if the slot holds
 * a valid save; return false for an empty/invalid slot.
 */
bool savegame_peek(int slot, char *out_label, int cap);
