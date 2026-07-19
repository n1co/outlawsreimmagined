/*
 * wax.h - Outlaws NWX/WAX sprite format decoder
 *
 * NWX files use "WAXF" magic and contain animated, multi-angle sprite data.
 * Format reverse-engineered from olwin.exe (wax_LoadCostume @0x0044d240,
 * wax_LoadFrames @0x0044dac0, wax_LoadChors @0x0044de10, RLE @0x004d3cf0).
 *
 * File layout:
 *   WAXF header (32 bytes)
 *     +0x00 magic "WAXF"
 *     +0x04 version major (2)
 *     +0x08 version minor (1)
 *     +0x0C scale_x (f32)
 *     +0x10 scale_y (f32)
 *     +0x14 CELT chunk offset (u32)
 *     +0x18 FRMT chunk offset (u32)
 *     +0x1C CHOT chunk offset (u32)
 *
 *   CELT chunk (at CELT offset):
 *     +0x00 magic "CELT"
 *     +0x04 cell_count (u32)
 *     then cell_count cells, each:
 *       +0x00 synch_index (u32, == cell index)
 *       +0x04 data_size (u32, bytes following the 20-byte header)
 *       +0x08 width  (u32)
 *       +0x0C height (u32)
 *       +0x10 flags  (u32); bit0: 0 = row-major, 1 = column-major
 *       then data_size bytes: [offset_table][RLE pixel data]
 *         offset_table has N u32 entries where N = height (row-major) or
 *         width (column-major); entry i = byte offset (from data start) to
 *         line i's RLE stream.
 *
 *   FRMT chunk (at FRMT offset): "FRMT", frame_count (u32), then per frame:
 *     +0x00 synch_index (u32)
 *     +0x04 field_a (i32)
 *     +0x08 field_b (i32)
 *     +0x0C (i32, unused)
 *     +0x10 (i32, unused)
 *     +0x14 view_count (u32)  -- one view per viewing direction
 *     then view_count views, each 16 bytes:
 *       +0x00 cell_index (u32)
 *       +0x04 off_x (i32)  anchor X of this cell relative to sprite origin
 *       +0x08 off_y (i32)  anchor Y
 *       +0x0C flags (u32)
 *
 *   CHOT chunk (at CHOT offset): "CHOT", chor_count (u32), then per chor:
 *     +0x00 synch_index (u32)
 *     +0x04 (u32, unused)
 *     +0x08 field0 (i32)
 *     +0x0C field1 (i32)
 *     +0x10 rate   (i32)  animation speed (sequence tokens advanced per tick)
 *     +0x14 field3 (i32)
 *     +0x18 ndirs  (u32)  viewing directions (usually 32)
 *     +0x1C nfrm   (u32)  tokens per direction
 *     then ndirs*nfrm u16 tokens (direction-major): token[dir*nfrm + t].
 *     A token < 0x8000 is a FRMT frame index to display for one tick.
 *     A token >= 0x8000 is an opcode (0xFFFF stop, 0xFFFE loop, 0xFFFB event
 *     with inline params, etc.), each followed by at least one param word.
 *
 * RLE line encoding (per line in the offset table):
 *   control byte: bit0 = mode (0 literal, 1 fill); count = (byte>>1)+1 (1..128)
 *   literal: control + count raw palette indices
 *   fill:    control + one palette index repeated count times
 *   palette index 0 = transparent.
 */
#pragma once

#include "engine.h"

#define WAX_MAX_CELLS  256   /* Max decoded cells per sprite */
#define WAX_MAX_FRAMES 256   /* Max FRMT frames per sprite */
#define WAX_MAX_CHORS   64   /* Max CHOT choreographies per sprite */
#define WAX_MAX_SEQ     64   /* Max display frames extracted from one chor */
#define WAX_DIRS         8   /* Number of viewing directions we expose */

/* Cell render flags (from the CELT cell header). */
#define WAX_CELL_TRANSLUCENT 0x20u  /* Draw additively/translucently (muzzle flash) */

typedef struct {
    u8  *pixels;    /* RGBA8 decoded pixels (width * height * 4) */
    u32  width;
    u32  height;
    i32  offset_x;  /* Anchor offset X (from FRMT off_x of first ref frame) */
    i32  offset_y;  /* Anchor offset Y (from FRMT off_y of first ref frame) */
    u32  flags;     /* CELT cell flags (bit0 orientation, 0x20 translucent, ...) */
} WaxCell;

/* One FRMT animation frame entry */
typedef struct {
    u32 seq;        /* Legacy grouping field (kept for compatibility) */
    u32 cell_idx;   /* Cell displayed for viewing direction 0 (view[0]) */
    f32 frame_w;    /* Pixel width of view[0]'s cell */
    f32 frame_h;    /* Pixel height of view[0]'s cell */
    i32 off_x, off_y;  /* Anchor offset of view[0] relative to sprite origin */
    u32 dt;         /* Display time ms (0 = driven by chor rate) */
    u32 view_cell[WAX_DIRS]; /* Cell index per viewing direction */
    u32 view_count;
} WaxFrame;

/* Max viewing directions / tokens stored per choreography for the full
 * per-direction grid (used for 8-directional enemy animation). */
#define WAX_CHOR_MAXDIRS 32
#define WAX_CHOR_MAXNFRM 16

/* One CHOT choreography (animation sequence such as REST/FIRE/RELOAD/WALK). */
typedef struct {
    i32 rate;       /* Sequence advance speed (tokens per tick base) */
    u32 ndirs;      /* Viewing directions */
    u32 nfrm;       /* Tokens per direction */
    u16 seq[WAX_MAX_SEQ]; /* Direction-0 token stream (frames + opcodes) */
    u32 seq_len;
    /* Full per-direction token grid: grid[dir][token]. Directional sprites
     * (enemies) reference different FRMT frames per viewing angle. */
    u16 grid[WAX_CHOR_MAXDIRS][WAX_CHOR_MAXNFRM];
    u32 grid_dirs;  /* min(ndirs, WAX_CHOR_MAXDIRS) */
    u32 grid_nfrm;  /* min(nfrm, WAX_CHOR_MAXNFRM) */
} WaxChor;

typedef struct {
    WaxCell  cells[WAX_MAX_CELLS];
    u32      cell_count;

    WaxFrame frames[WAX_MAX_FRAMES];
    u32      frame_count;

    WaxChor  chors[WAX_MAX_CHORS];
    u32      chor_count;

    /* For 8-directional rendering: idle cell for each of 8 viewing directions */
    u32      dir_cell[WAX_DIRS];

    f32      scale_x, scale_y;

    /* Pixel dimensions of the idle (direction-0) cell */
    f32      first_w, first_h;
} WaxSprite;

/*
 * Decode an NWX/WAX sprite file from memory.
 * palette: 256-color RGB palette (768 bytes) for colorization.
 * Returns true on success. Call wax_free() to release.
 */
bool wax_decode(WaxSprite *sprite, const u8 *data, u32 size,
                const u8 palette[256][3]);

/*
 * Expand a choreography's direction-0 token stream into an ordered list of
 * display frames. For each display frame, fills the FRMT frame index into
 * out_frames[] and its per-frame duration (ms) into out_dt[] when non-null.
 * Opcodes (stop/loop/events) terminate or are skipped. Returns the number of
 * display frames written (<= max). If chor is out of range returns 0.
 *
 * Duration per frame is derived from the chor rate assuming a 1 kHz time base
 * (dt_ms = 1000 / rate, matching the original engine's wax time base).
 */
u32 wax_chor_frames(const WaxSprite *sprite, u32 chor,
                    u32 *out_frames, u32 *out_dt, u32 max);

/*
 * Like wax_chor_frames but for a specific viewing direction `dir` (0-based
 * index into the chor's direction grid, 0..ndirs-1). Used to build the 8
 * directional animation sequences of enemy sprites. Falls back to direction 0
 * data when the grid is unavailable.
 */
u32 wax_chor_frames_dir(const WaxSprite *sprite, u32 chor, u32 dir,
                        u32 *out_frames, u32 *out_dt, u32 max);

/* Free all sprite resources. */
void wax_free(WaxSprite *sprite);
