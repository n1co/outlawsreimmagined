# Outlaws Engine Reverse Engineering: Complete Level Loading & Rendering Pipeline

## Table of Contents
1. [Sector Structure Layout (0xD0 bytes)](#1-sector-structure-layout)
2. [Wall Structure Layout (0xB0 bytes)](#2-wall-structure-layout)
3. [Level Loading Pipeline](#3-level-loading-pipeline)
4. [Sector Flags](#4-sector-flags)
5. [Wall Flags](#5-wall-flags)
6. [Floor/Ceiling Rendering](#6-floorceling-rendering)
7. [Wall Rendering (UV Computation)](#7-wall-rendering)
8. [Portal/Adjoin System](#8-portaladjoin-system)
9. [Slope System](#9-slope-system)
10. [Texture System](#10-texture-system)
11. [Key Constants](#11-key-constants)
12. [Rendering Pipeline Overview](#12-rendering-pipeline-overview)

---

## 1. Sector Structure Layout

Size: **0xD0 bytes** (208 bytes), allocated as `sectorCount * 0xD0`.

Derived from `level_LoadSectors` (0x0041de4c) and `Sector_Serialize` (0x004dc8f0) and `FUN_004dd640` (sector init at 0x004dd640):

```
Offset  Size  Field               Notes
------  ----  -----               -----
0x00    4     id                  ((uint)ptr & 0xFFFF) + 0x20000 (type tag)
0x04    4     index               Sector index (set during load)
0x08    4     name_hash           Sector name/ID (from LVT SECTOR tag)
0x0C    4     layer               Layer number (ambient)
0x10    4     padding/unknown
0x14    1     floor_light         Floor light level (byte)
0x15    1     ceil_light          Ceiling light level (byte)
0x18    4     ambient             Ambient light (from LVT AMBIENT)
0x1C    4     vertices_ptr        Pointer to vertex array (float x,z pairs)
0x20    4     bbox_min_x          Bounding box min X (float)
0x24    4     bbox_max_x          Bounding box max X (float)
0x28    4     bbox_min_z          Bounding box min Z (float)
0x2C    4     bbox_max_z          Bounding box max Z (float)
0x30    4     intensity           Light intensity (float, default 1.0)
0x34    4     ceil_alt            Ceiling altitude/second height (float, default -60.0)
0x38    4     velocity_ptr        Velocity vector pointer (allocated 0xC if non-zero)
0x3C    4     velocity_copy_ptr   Copy of velocity vector pointer
0x40    4     extra_light         Extra light (float, default 0.3)
0x44    4     objects_head        Linked list of objects in sector
0x48    4     sound_ptr           Sound/WAV resource pointer
0x4C    4     ceil_tex_handle     Ceiling texture handle (loaded from texture table)
0x50    4     floor_tex_handle    Floor texture handle
0x54    4     floor_sign_tex      Floor sign/overlay texture handle
0x58    4     ceil_sign_tex       Ceiling sign/overlay texture handle
0x5C    4     ceil_tex_offset_x   Ceiling texture offset X (float)
0x60    4     ceil_tex_offset_z   Ceiling texture offset Z (float)
0x64    4     ceil_sign_off_x     Ceiling sign offset X (float)
0x68    4     ceil_sign_off_z     Ceiling sign offset Z (float)
0x6C    4     floor_tex_offset_z  Floor texture offset Z (float)  -- NOTE: Z stored first
0x70    4     floor_tex_offset_x  Floor texture offset X (float)
0x74    4     floor_sign_off_x    Floor sign offset X (float)
0x78    4     floor_sign_off_z    Floor sign offset Z (float)
0x7C    2     ceil_tex_angle      Ceiling texture rotation angle (ushort, 14-bit)
0x7E    2     floor_tex_angle     Floor texture rotation angle (ushort, 14-bit)
0x80    2     floor_sign_angle    Floor sign texture rotation angle
0x82    2     ceil_sign_angle     Ceiling sign texture rotation angle
0x84    4     ceil_y              Ceiling Y height (float) -- KEY FIELD
0x88    4     floor_y             Floor Y height (float) -- KEY FIELD
0x8C    4     slope_ceil_ptr      Sloped ceiling data pointer (0x58 bytes)
0x90    4     slope_floor_ptr     Sloped floor data pointer (0x58 bytes)
0x94    4     layer_sector_ptr    Layer/linked sector pointer
0xA4    4     vertex_count        Number of vertices
0xA8    4     transformed_vtx_ptr Transformed vertex array pointer
0xAC    4     wall_count          Number of walls
0xB0    4     walls_ptr           Pointer to wall array
0xC4    4     render_stamp        Render frame stamp (prevents re-rendering)
0xC8    4     flags1              Primary sector flags (bit 0=exterior, etc.)
0xCC    4     flags2              Secondary flags / dirty bits
```

## 2. Wall Structure Layout

Size: **0xB0 bytes** (176 bytes), allocated as `wallCount * 0xB0`.

From `level_LoadSectors` wall parsing and `FUN_004e3ab0` (wall init):

```
Offset  Size  Field               Notes
------  ----  -----               -----
0x00    4     id                  ((uint)ptr & 0xFFFF) + 0x10000 (type tag)
0x04    4     v1_idx              Vertex 1 index
0x08    4     v2_idx              Vertex 2 index (unused after resolve)
0x0C    4     unused
0x10    4     top_tex             TOP texture handle
0x14    4     mid_tex             MID texture handle
0x18    4     bot_tex             BOT texture handle
0x1C    4     overlay_tex         OVERLAY texture handle
0x20    4     top_tex_u           TOP texture U offset (float * _DAT_00500054)
0x24    4     mid_tex_u           MID texture U offset (float * _DAT_00500054)
0x28    4     bot_tex_u           BOT texture U offset (float * _DAT_00500054)
0x2C    4     overlay_tex_u       OVERLAY texture U offset
0x30    4     top_tex_v           TOP texture V offset (float * _DAT_00500054)
0x34    4     mid_tex_v           MID texture V offset (float * _DAT_00500054)
0x38    4     bot_tex_v           BOT texture V offset (float * _DAT_00500054)
0x3C    4     overlay_tex_v       OVERLAY texture V offset
0x40    4     top_height          TOP wall height (float) -- computed post-load
0x44    4     mid_height          MID wall height (float) -- computed post-load
0x48    4     bot_height          BOT wall height (float) -- computed post-load
0x4C    4     wall_normal_x       Wall normalized direction X (float)
0x50    4     wall_normal_z       Wall normalized direction Z (float)
0x54    2     wall_angle          Wall angle (short, computed from vertices)
0x58    4     wall_length         Wall length (float, computed from vertices)
0x5C    4     light               Wall light level (int)
0x60    4     adjoin_sector_ptr   Adjoin (portal) sector pointer
0x64    4     adjoin_wall_ptr     Adjoin (portal) mirror wall pointer
0x68    4     dadjoin_sector_ptr  Double adjoin sector pointer
0x6C    4     dadjoin_wall_ptr    Double adjoin mirror wall pointer
0x70    4     sign_tex            Wall sign texture
0x74    4     v1_ptr              Pointer to vertex 1 (float x,z)
0x78    4     v2_ptr              Pointer to vertex 2 (float x,z)
0x7C    4     parent_sector_ptr   Pointer to parent sector
0x80    4     flags1              Wall flags (primary)
0x84    4     flags2              Wall flags (secondary / dirty)
0x94    4     v1_x_cached         Cached vertex 1 X (float)
0x98    4     v1_z_cached         Cached vertex 1 Z (float)
0x9C    4     v1_transformed_ptr  Pointer to transformed vertex 1
0xA0    4     v2_transformed_ptr  Pointer to transformed vertex 2
0xA4    4     adj_mirror_idx      Adjoin mirror index (temp, resolved to pointer)
0xA8    4     render_stamp        Render frame stamp
```

## 3. Level Loading Pipeline

### Level_Load (0x0041bb10)
1. Allocates level structure (0x194 bytes)
2. Opens LVT file (tries .LVB first, then .LVT)
3. Parses LVT chunk header (magic 0x2E4C5654 = ".LVT")
4. Loads header data via `FUN_0041d7a0`
5. **Loads textures**: Parses TEXTURES section
   - Each texture entry: filename + extension check (.ATX or .PCX)
   - ATX textures loaded via FUN_00489ee0 with magic 0x41545846 ("FXTA")
   - PCX textures loaded via FUN_00489ee0 with magic 0x5458544d ("MTXT")
   - Texture table: 0xC bytes per entry (refcount:2, flags:2, padding:4, handle:4)
6. **Loads sectors** via `level_LoadSectors`
7. Loads objects via `FUN_0041c8d0`
8. Loads game data via `FUN_00489400`

### level_LoadSectors (0x0041de4c)
Sector loading proceeds in THREE passes:

**Pass 1** - Parse each sector from LVT:
1. Allocate sector array: `sectorCount * 0xD0`
2. For each sector:
   - Parse SECTOR tag (index 0x0F)
   - Parse NAME tag (0x10)
   - Parse AMBIENT (0x11)
   - Parse FLOOR_LIGHT (0x12) / CEIL_LIGHT (0x13)
   - Parse INTENSITY (0x14), default 1.0
   - Parse CEILING_ALT (0x15), default -60.0
   - Parse EXTRA_LIGHT (0x16), default 0.3
   - Parse VELOCITY (0x17), 3 floats
   - Parse LAYER_LINK (0x18) - converts to sector pointer
   - Parse SOUND (0x19)
   - **Parse FLOOR Y** (0x1A): 5 values: `floor_tex_offset_x, floor_tex_offset_z, floor_sign_off_x, floor_sign_off_z(?), floor_tex_idx`
     - sector[0x22] = floor_tex_offset_x
     - sector[0x17] = floor_tex_offset_z (tex offset Z)
     - sector[0x1B] = floor_sign_off_x
     - sector[0x1F] = (short)floor_sign_angle
     - Floor texture resolved from texture table
   - **Parse CEILING TEXTURE** (0x1B): same pattern
     - sector[0x21] = ceil_tex_offset_x (stored as float)
     - sector[0x18] = ceil_tex_offset_z
     - sector[0x1C] = ceil_sign_off_x
     - sector[0x7E>>2] = (short)ceil_sign_angle
     - Ceiling texture resolved from texture table
   - Parse F (floor sign) (0x1C) and C (ceiling sign) (0x1D)
   - Parse FLOOR OFFSETS (0x1E) - offset entries (read and discarded)
   - Parse FLOOR_Y/CEIL_Y heights (0x20):
     - **Check bit 30 (0x40000000) of sector flags1 (offset 0xC8)** → sloped ceiling
     - **Check bit 31 (0x80000000) of sector flags1** → sloped floor
   - Parse SLOPEDCEILING (0x21) and SLOPEDFLOOR (0x22) if flagged
     - Packed as `sectorIdx << 16 | wallIdx` into slope struct
   - Parse LAYER (0x23): floor Y and layer for height tracking
   - **Parse VERTICES** (0x24): allocate `vertexCount * 8` bytes (2 floats per vertex)
     - Also allocate transformed vertex array (same size)
     - Track bounding box (min/max X and Z)
   - **Parse WALLS** (0x26): allocate `wallCount * 0xB0`
     - Parse WALL tag (0x27) with 22 fields:
       `V1, V2, MID_tex, MID_u, MID_v, TOP_tex, TOP_u, TOP_v, BOT_tex, BOT_u, BOT_v, OVERLAY_tex, OVERLAY_u, OVERLAY_v, ADJOIN, MIRROR, DADJOIN, DMIRROR, FLAGS1, FLAGS2, LIGHT`
     - **Wall texture UV offsets scaled by _DAT_00500054** (=8.0):
       ```c
       wall->mid_tex_u = fStack_22c * _DAT_00500054;  // = texU_from_LVT * 8.0
       wall->mid_tex_v = fStack_228 * _DAT_00500054;  // = texV_from_LVT * 8.0
       ```
     - Adjoin sector resolved: `adjoinSectorIdx * 0xD0 + sectorsBasePtr`
     - Wall angle computed: `FUN_004af8a0(v2.x - v1.x, v2.z - v1.z)` → 16-bit angle
     - Wall length computed: `sqrt((v2.x-v1.x)^2 + (v2.z-v1.z)^2)`
     - Wall normal computed: `(dx/len, dz/len)`

**Pass 2** - Resolve wall mirrors and set derivative flags:
1. For each wall with adjoin:
   - `wall->adjoin_wall_ptr = wall->adj_mirror_idx * 0xB0 + adjoinSector->walls_ptr`
   - Same for double adjoin
2. **Exterior sector propagation** (flag 0x10000000):
   - If parent sector has flag 0x10000000 AND wall does NOT have flag 0x10000:
     - Set wall flag2 bit 0x20000000
     - UNLESS adjoin/dadjoin sector also has 0x10000000 (then CLEAR bit 0x20000000)
   - If wall has flag 0x10000 OR sector has 0x10000000:
     - If sector also has 0x40000000 (sloped ceil):
       Set sector flag 0x8000000 and flag2 bit 0x8
3. **ADJOIN_MID (flag 0x4000)** handling:
   - Sets wall flags 0x800, 0x8000 (= transparent/always-draw)
   - Calls `FUN_0045b7b0(0x80000000, wall)` to set up sign rendering
   - Also sets same flags on mirror wall

**Pass 3** - Compute wall heights and convexity:
1. For each sector, call `FUN_004dda00` (wall connectivity check) and `FUN_004ddaa0` (convexity test)
2. `FUN_004ddaa0` return: 2 = convex → set flag 0x40000000; 1 = concave → set 0x20000000
3. Call `FUN_004e4600` for each wall: computes wall TOP/MID/BOT heights:
   - **Solid wall (no adjoin)**: `mid_height = (ceil_y - floor_y) * _DAT_00500a60`
   - **Portal wall**:
     - `top_height = (adjoin_sector->ceil_y - sector->ceil_y) * _DAT_00500a60` (if flag bit 0)
     - `bot_height = (sector->floor_y - adjoin_sector->floor_y) * _DAT_00500a60` (if flag bit 1, reversed)
     - `mid_height = (adjoin_ceil_y - adjoin_floor_y) * _DAT_00500a60` (when MID texture)

## 4. Sector Flags

`sector->flags1` at offset 0xC8:

```
Bit 0  (0x00000001) = EXTERIOR - Sky sector (no ceiling rendered, uses sky texture)
Bit 1  (0x00000002) = NO_CEIL - Ceiling not rendered (separate from exterior)
Bit 4  (0x00000010) = SKY_PIT - Exterior floor uses sky pit rendering
Bit 7  (0x00000080) = SUBSECTOR - Part of another sector (layer link)
Bit 8  (0x00000100) = Unknown (triggers FUN_0045cbb0 during load)
Bit 23 (0x00800000) = HAS_VISIBLE_3D_OBJECTS - Set when sector has type-2 visible objects
Bit 24 (0x01000000) = (reserved for ATX texture tracking)
Bit 25 (0x02000000) = EXTERIOR_ADJOIN_COUNT - increments global counter
Bit 26 (0x04000000) = NO_FLOOR_LIGHT_BLEND - Disables floor light smoothing
Bit 27 (0x08000000) = SLOPED_CEIL_EXTERIOR - Sloped + exterior ceil combo
Bit 28 (0x10000000) = SKY_EXTERIOR_SECTOR - Sector marked as exterior (propagated to walls)
Bit 29 (0x20000000) = CONCAVE_SECTOR - Sector polygon is concave
Bit 30 (0x40000000) = SLOPED_CEILING - Has sloped ceiling
Bit 31 (0x80000000) = SLOPED_FLOOR - Has sloped floor
```

`sector->flags2` at offset 0xCC (dirty/runtime flags):

```
Bit 0  (0x01) = DIRTY - Needs re-computation
Bit 3  (0x08) = SLOPE_MODIFIED - Slope data changed
```

### Floor/Ceiling Rendering Decision Tree (from FUN_004a7430 at ~0x004a7eb4):

```
CEILING (sector->flags1):
  if (flag & 0x02) == 0:       // NOT no_ceil
    if (flag & 0x40000000) == 0:   // NOT sloped
      FUN_004b4bb0()               // FLAT ceiling renderer
    else:
      FUN_004b54e0()               // SLOPED ceiling renderer
  else:
    if (flag & 0x10) == 0:
      FUN_004b6410()               // EXTERIOR ceiling (column renderer)
    else:
      FUN_004b68e0()               // SKY PIT ceiling renderer

FLOOR (sector->flags1):
  if (flag & 0x01) == 0:       // NOT exterior
    if (flag & 0x80000000) == 0:   // NOT sloped
      FUN_004b42a0()               // FLAT floor renderer
    else:
      FUN_004b5b40()               // SLOPED floor renderer
  else:
    if (flag & 0x10) == 0:
      FUN_004b6190()               // EXTERIOR floor (column renderer)
    else:
      FUN_004b66b0()               // SKY PIT floor renderer
```

**CRITICAL**: Floor is NOT rendered when `sector->flags1 & 0x01` (EXTERIOR bit set). Ceiling is NOT rendered when `sector->flags1 & 0x02` (NO_CEIL bit set).

The flat floor renderer `FUN_004b42a0` has an **early return** guard:
```c
if (*(float *)(param_1 + 0x1c) <= *(float *)(param_2 + 0x84))
    // proceed with rendering
else
    return;  // Camera BELOW ceiling → skip floor
```
This means: floor only renders if `camera_y <= sector->ceil_y`. The ceiling renderer `FUN_004b4bb0` has the opposite guard: `sector->floor_y <= camera_y`.

## 5. Wall Flags

`wall->flags1` at offset 0x80:

```
Bit 0  (0x0001) = ADJOIN_TOP - Has TOP strip (portal with ceil height difference)
Bit 1  (0x0002) = ADJOIN_BOT - Has BOT strip (portal with floor height difference)
Bit 2  (0x0004) = FLIP_TEX_HORIZ - Flip texture horizontally
Bit 3  (0x0008) = ILLUM_SIGN - Sign is illuminated
Bit 4  (0x0010) = FLIP_TEX_VERT - Flip texture vertically (used for texture flipping on portal walls)
Bit 5  (0x0020) = RENDER_AS_FULL_ALPHA - Uses alpha blending
Bit 6  (0x0040) = TEX_ANCHORED - Texture anchored (bottom-pegged), propagated to mirror
Bit 7  (0x0080) = MORPH_WALL - Morphing wall (dynamic geometry)
Bit 8  (0x0100) = UNKNOWN
Bit 9  (0x0200) = UNKNOWN
Bit 10 (0x0400) = UNKNOWN
Bit 11 (0x0800) = SIGN_ANCHORED - Sign texture anchored
Bit 12 (0x2000) = ADJOIN_MID_TX - Adjoin MID texture (forces mid rendering on portal)
Bit 13 (0x4000) = ADJOIN_MID - Portal with always-visible MID texture (fence/grate)
Bit 14 (0x8000) = SIGN_ANCHORED2 - Another sign anchoring flag
Bit 16 (0x10000) = ALWAYS_DRAW_WALL - Overrides portal visibility
Bit 17 (0x20000) = SKY_BOUNDARY - Sky volume boundary wall (skipped in rendering)
```

`wall->flags2` at offset 0x84:

```
Bit 0  (0x0001) = DIRTY
Bit 2  (0x0004) = FLAGS_MODIFIED
Bit 26 (0x04000000) = PROPAGATED_FROM_MIRROR
Bit 27 (0x08000000) = ADJOIN_MID_PROCESSED
Bit 28 (0x10000000) = SLOPE_WALL_FLAG
Bit 29 (0x20000000) = EXTERIOR_WALL_FLAG
Bit 30 (0x40000000) = RENDERED_THIS_FRAME
```

## 6. Floor/Ceiling Rendering

### How the Original Engine Renders Floors (NOT polygon tessellation)

The Outlaws engine does **NOT** tessellate floor polygons into triangles. Instead, it uses a **scanline-based span renderer** that works column-by-column (or row-by-row depending on perspective).

#### Flat Floor Renderer: FUN_004b42a0

The flat floor renderer iterates screen rows from `DAT_00520b80` (top visible row) to `DAT_00520b90` (bottom visible row):

```c
iVar7 = DAT_00520b80;  // topmost visible screen row for floor
if (DAT_00520b80 <= DAT_00520b90) {
    do {
        // For each screen row, find the horizontal span to fill
        // using wall edge data from the wall edge buffer

        // fStack_104 = perspective correction factor from lookup table
        fStack_104 = *(float *)(tanTable + ((row - fov_offset) + pitch_offset + fov_shift) * 4);
        fStack_5c = fStack_104 * floor_height_factor;

        // Walk wall edges to find left/right span boundaries
        // ... (edge traversal logic finds iStack_fc..iStack_108 span) ...

        // Clip to screen bounds
        FUN_004aa5b0(&iStack_fc, &iStack_108, iVar7);

        // Compute texture UV at this screen row
        fStack_ec = (float)(iStack_108 - screen_center_x);

        // U = (cos_component - row_offset * sin_component) * perspective - tex_offset
        fStack_20 = (fStack_18 - fStack_ec * fStack_cc) * fStack_104 - fStack_1c;
        DAT_005d6388 = ROUND(fStack_20 * 65536.0) >> textureBits;  // U start

        // V = (row_offset * cos_component + sin_component) * perspective - tex_offset
        fStack_38 = (fStack_ec * fStack_d0 + fStack_30) * fStack_104 - fStack_34;
        DAT_005d63c8 = ROUND(fStack_38 * 65536.0) >> textureBits;  // V start

        // dU/dx = sin_component * perspective
        fStack_44 = fStack_104 * fStack_cc;
        DAT_005d63ec = ROUND(fStack_44 * 65536.0) >> textureBits;

        // dV/dx = -(cos_component * perspective)
        fStack_50 = -(fStack_104 * fStack_d0);
        DAT_005d642c = ROUND(fStack_50 * 65536.0) >> textureBits;

        // Set framebuffer pointer and call scanline renderer
        DAT_005d637c = framebuffer + row_stride * row + leftX;
        DAT_005d636c = (rightX - leftX) + 1;

        // Call appropriate scanline filler (textured or lit)
        (*pcStack_d8)();  // or (*pcStack_d4)() for lit version

        iVar7++;
    } while (iVar7 <= DAT_00520b90);
}
```

### Floor UV Formula (EXACT)

Setup phase (before scanline loop):
```c
// Input: sector texture rotation angle (ushort), camera position, floor height
FUN_004af400(-(camera_angle + tex_angle), &sin_a, &cos_a);  // combined rotation
FUN_004af400(tex_angle, &sin_t, &cos_t);  // texture rotation only

// Camera offset from texture origin
float dx = camera_x - sector->floor_tex_offset_x;
float dz = sector->floor_tex_offset_z - camera_z;   // NOTE: Z is negated

// Pre-compute rotated offsets
float tex_offset_u = (cos_t * dx - sin_t * dz) * _DAT_005008ac;
float tex_offset_v = (dz * cos_t + dx * sin_t) * _DAT_005008ac;

// Pre-compute rotated scanline deltas
float floor_height = -(sector->floor_y - camera_y);
float fh_factor = floor_height * camera_focal_length;  // focal_length at param_1+0x58

float u_row_factor = fh_factor * cos_a * _DAT_005008ac;
float v_row_factor = fh_factor * sin_a * _DAT_005008ac;
float u_col_factor = floor_height * sin_a * _DAT_005008b0;
float v_col_factor = floor_height * cos_a * _DAT_005008b0;
```

Per-scanline:
```c
float perspective = tanTable[row_index];
float row_offset = (float)(rightX - screen_center_x);

// Starting U at rightmost pixel of span, stepping left
float U = (u_row_factor - row_offset * u_col_factor) * perspective - tex_offset_u;
float V = (row_offset * v_col_factor + v_row_factor) * perspective - tex_offset_v;

// Step per pixel (moving left to right)
float dU = perspective * u_col_factor;
float dV = -(perspective * v_col_factor);

// Convert to 16.16 fixed point, shifted by texture size bits
int u_fixed = ROUND(U * 65536.0) >> tex_bits;
int v_fixed = ROUND(V * 65536.0) >> tex_bits;
int du_fixed = ROUND(dU * 65536.0) >> tex_bits;
int dv_fixed = ROUND(dV * 65536.0) >> tex_bits;
```

**Key constants:**
- `_DAT_005008ac` = texture scale factor for floors (likely 8.0, matching wall 8x scale)
- `_DAT_005008b0` = another texture scale factor (likely related, e.g., 8.0 or 1/8.0)
- `tex_bits` = log2(texture_size) - 1 (e.g., for 128x128: bits=7, mask=0x7F)

### Ceiling UV Formula

Identical to floor, but:
- Uses `sector->ceil_y` (offset 0x84) instead of `floor_y` (offset 0x88)
- Uses ceiling texture offsets (0x5C, 0x6C) instead of floor offsets (0x60, 0x70)
- Uses ceiling texture angle (offset 0x7C) instead of floor angle (0x7E)
- Scanline iteration goes from `DAT_00520b94` to `DAT_00520b84` (bottom to top region)
- Guard condition is REVERSED: `floor_y <= camera_y` (only render ceil when above floor)

### Sign/Overlay Texture on Floors

If `sector->floor_sign_tex != 0`, a second pass renders the sign texture over the floor using the same scanline method but with different UV parameters (sign offsets and sign angle).

## 7. Wall Rendering

### wall_FillZBuffer_Textured (0x004c71c0)

The wall renderer determines which strip(s) to draw based on `wall->flags1 & 0x70` (the "type" field):

```
uVar15 = *(uint *)(wall + 0x70);  // wall type/flags

if (uVar15 == 0xC):  // Portal wall with TOP strip
    top_y = adjoin_sector->floor_y   (get from wall->adjoin_sector via offset 0x60)
    bot_y = parent_sector->floor_y   (get from wall->parent_sector via offset 0x7C)

if (uVar15 == 4):   // Portal wall with BOT strip
    top_y = parent_sector->ceil_y
    bot_y = adjoin_sector->floor_y

else:                // Solid wall or MID
    top_y = parent_sector->ceil_y
    bot_y = parent_sector->floor_y
```

### Sloped Height Computation for Walls

When a sector has sloped floor/ceiling, wall heights are computed per-vertex:
```c
if ((sector->flags1 & 0x40000000) == 0) {
    // Flat ceiling
    floor_y_at_v1 = sector->floor_y;
    floor_y_at_v2 = sector->floor_y;
} else {
    // Sloped ceiling - compute height at each wall vertex
    slope = sector->slope_floor_ptr;
    floor_y_at_v1 = slope->base_height - slope->normal_z * v1.z - slope->normal_x * v1.x;
    floor_y_at_v2 = slope->base_height - slope->normal_z * v2.z - slope->normal_x * v2.x;
}
```

### Wall Texture UV Computation (EXACT)

From the wall preparation in `FUN_004c2820` and rendering in `wall_FillZBuffer_Textured`:

**Horizontal (U) coordinate:**
```c
// wall->wall_length contains length in world units
// wall->mid_tex_u (offset 0x24) = LVT tex_offset * 8.0 (the _DAT_00500054 scale)
// param_2[0xc] = texture U scale factor (from wall prep)

// Perspective-correct interpolation per column:
if (param_2[10] == 0) {  // normal orientation
    float t = fVar8 / (column_depth - camera_offset) - camera_x_offset;
    float tex_u = wall->tex_scale * t;
    float depth = t * camera_offset + camera_depth;
} else {  // swapped orientation (wall nearly parallel to view)
    float depth = fVar8 / (column_depth - camera_offset);
    float tex_u = (depth - camera_depth) * wall->tex_scale;
}

// Final U = tex_u + wall_tex_offset + wall_tex_u_base
fStack_260 = tex_u + wall->anchor_offset + wall->sign_offset;

// Convert to texel coordinates
uint u_texel = ROUND(fStack_260 * 65536.0) >> 16;
u_texel &= (texture_width_pixels - 1);  // wrap with power-of-2 mask

// If FLIP_TEX_HORIZ flag (0x04):
if (wall->flags1 & 4) {
    u_texel = (texture_width_pixels - u_texel) - 1;
}
```

**Vertical (V) coordinate:**
```c
// wall->tex_v_height = wall height in texture space
// Computed per-column from wall edge positions

float wall_span = (bottom_screen_y - top_screen_y) + 1.0;  // pixel span
int v_step = ROUND((tex_v_height / wall_span) * 65536.0);   // fixed-point step

// Starting V position at top of column
int v_start = ROUND(((float)v_step * (wall_span - (float)visible_bottom) + tex_anchor) * 65536.0);

// v_step shifted by texture height bits for wrapping
DAT_005d6390 = v_step >> tex_bits;
DAT_005d63dc = v_start >> tex_bits;
```

**The 8x texture scale:**
Wall texture UV offsets from the LVT file are multiplied by `_DAT_00500054` (= **8.0**) during loading:
```c
wall->mid_tex_u = fStack_22c * _DAT_00500054;  // = lvt_offset_u * 8.0
wall->mid_tex_v = fStack_228 * _DAT_00500054;  // = lvt_offset_v * 8.0
```
This means **1 LVT texture unit = 8 texels**. A wall offset of 1.0 in the LVT file shifts the texture by 8 pixels.

### Wall Sign/Overlay Rendering

After the main texture column is drawn, if the wall has a sign texture (`wall->sign_tex != 0` at offset 0x1C), a second column is rendered overlaying the sign. The sign has its own UV offsets and uses a special height offset (`wall->sign_offset` at offset 0x2C).

## 8. Portal/Adjoin System

### Portal Traversal (Recursive)

The main sector rendering function `FUN_004a7430` (at 0x004a7430) is **recursive**. The portal traversal works as follows:

1. **Render all walls** of current sector:
   - Solid walls → `wall_FillZBuffer_*` variants
   - Portal walls (flags & 4) → render TOP and BOT strips, then add portal window

2. **For portal walls**: `Render_AddWallWindow` records the portal opening (top/bottom screen edges per column)

3. **Render floor/ceiling** of current sector using wall edges as clipping boundaries

4. **Recurse into adjoined sectors**:
```c
// Portal recursion (from FUN_004a7430)
if (iVar19 != 0 && DAT_00520bc0 == 0 && DAT_00560ef4 < 0x8c) {
    FUN_004aa3a0(param_1, puStack_298, iVar19, ...);  // Setup portal clip regions

    for each portal window {
        int wall = *puStack_2ac;
        int adjoin_sector;

        if (portal_type == 8) {
            adjoin_sector = wall->dadjoin_sector;  // Double adjoin
        } else {
            adjoin_sector = wall->adjoin_sector;   // Normal adjoin
        }

        // Check if sector already rendered this frame
        if (adjoin_visible) {
            wall->render_stamp = current_frame;

            // Set up clip bounds from portal window
            DAT_00520ba0 = portal_top_buffer;
            DAT_00520ba4 = portal_bot_buffer;

            // RECURSE into adjoined sector
            FUN_004a7430(renderer, adjoin_sector, wall_data, param4);

            // Restore clip state
            FUN_004aa220();
            wall->render_stamp = 0;

            // If ADJOIN_MID flag, render MID texture OVER the portal
            if (wall->flags1 & 1) {
                FUN_004c39a0(renderer, wall_data, puStack_298);
            }
        }
    }
}
```

### Portal Clip Region Setup (FUN_004aa3a0)

For each portal window, the engine maintains **per-column top and bottom clip buffers**:
- Portal top buffer: maximum of wall top edge and existing clip top
- Portal bottom buffer: minimum of wall bottom edge and existing clip bottom

The portal windows are clipped against each other using `FUN_004c2f00` (wall occlusion sorter) which sorts walls front-to-back and clips overlapping portal regions.

### ADJOIN_MID Walls (FUN_004c39a0)

ADJOIN_MID walls (flag 0x4000/0x2000) are portal walls that also render a MID texture (e.g., fences, grates). The rendering:
1. Portal traversal happens normally (you can see through)
2. AFTER returning from the recursive portal render, the MID texture is drawn using the portal's screen bounds as vertical limits
3. The MID texture uses the same UV computation as normal walls but within the portal opening span

### Portal Window Data Structure

Each portal window is stored in a 0x3C-byte (60 byte) structure:
```
Offset  Field
0x00    wall pointer
0x04    top screen pos (float, interpolated)
0x08    bottom screen pos (float, delta per column)
0x0C    sign/direction
0x10    top row start
0x14    top row end
0x18    bottom float
0x1C    ...
0x20    bottom delta
0x24    top row
0x28    bottom row
0x2C    left column
0x30    start column (in screen coords)
0x34    end column
0x38    type (8 = double adjoin)
```

### Recursion Limits

- Maximum wall edges: 0x567 (1383)
- Maximum wall windows: 0x467 (1127)
- Maximum adjoining sectors per frame: 0x3B (59)
- Maximum active windows: checked separately
- Maximum recursion depth: tracked by `DAT_00588728`, limit checked as `< 2` for certain operations

## 9. Slope System

### Slope Structure (0x58 bytes, allocated by FUN_004ac760)

```
Offset  Size  Field
0x00    4     packed_ref          sectorIdx << 16 | wallIdx (packed reference)
0x04    4     parent_sector_ptr   Pointer to parent sector
0x08    2     angle               Slope angle (short)
0x0C    4     min_height          Minimum height across all vertices (float)
0x10    4     max_height          Maximum height across all vertices (float)
0x14    4     normal_x            Slope normal X component (float)
0x18    4     normal_z            Slope normal Z component (float)
0x1C    4     hinge_v1_x          Hinge wall vertex 1 X (float)
0x20    4     hinge_v1_y          Hinge wall vertex 1 Y (height, float)
0x24    4     hinge_v1_z          Hinge wall vertex 1 Z (float)
0x28    4     origin_x            Slope origin X (float)
0x2C    4     origin_y            Slope origin Y (float)
0x30    4     origin_z            Slope origin Z (float)
0x34    4     base_height         Base height at origin (float)
0x38    4     tangent1_x          First tangent vector X (float)
0x3C    4     tangent1_y          First tangent vector Y (float)
0x40    4     tangent1_z          First tangent vector Z (float)
0x44    4     tangent2_x          Second tangent vector X (float)
0x48    4     tangent2_y          Second tangent vector Y (float)
0x4C    4     tangent2_z          Second tangent vector Z (float)
0x50    4     dot_product1        tangent1 . origin (float)
0x54    4     dot_product2        tangent2 . origin (float)
```

### Slope Initialization (FUN_004ac890)

```c
// From hinge wall vertices and angle:
ushort angle = slope->angle;
float v1_x = slope->hinge_v1_x;
float v1_y = slope->hinge_v1_y;
float v1_z = slope->hinge_v1_z;

// Compute edge direction from hinge wall
float edge_x = v2_x - v1_x;
float edge_y = v2_y - v1_y;
float edge_z = v2_z - v1_z;
float edge_len = sqrt(edge_x*edge_x + edge_z*edge_z);

// Rotate perpendicular to edge by slope angle
float perp_x = -edge_z;
float perp_y = 0;
float perp_z = edge_x;

// Apply angle rotation
float rotated_y = cos(angle) * edge_len + v1_y;  // FUN_004af5f0(angle) * edge_len

// Compute slope normal from 3 points
float cross_denom = 1.0 / (perp_z * edge_x - perp_x * edge_z);
slope->normal_x = (rotated_y * edge_z - perp_z * edge_y_diff) * cross_denom;
slope->normal_z = (perp_x * edge_y_diff - rotated_y * edge_x) * cross_denom;
slope->base_height = normal_z * v1_z + normal_x * v1_x + v1_y;
```

### Height at Point on Slope

From `FUN_004aca70`:
```c
float height_at_point(Slope* slope, float x, float y_cam, float z) {
    return (slope->normal_x * x + slope->normal_z * z - slope->base_height) + y_cam;
}
```

Actually, the height at a world point is:
```c
float height = slope->base_height - slope->normal_z * z - slope->normal_x * x;
```

### Sloped Floor Rendering (FUN_004b5b40)

The sloped floor renderer is fundamentally different from the flat renderer. Instead of using a lookup table for perspective correction, it computes the slope plane intersection per-scanline:

```c
// Per-scanline computation:
float fov_y = (float)(camera_fov_center - screen_row);
float col_offset = (float)(leftX - screen_center_x);
float col_y = (float)(int)fov_y;

// Plane intersection in view space
_DAT_005d63cc = normal_view * col_offset + focal_length * tangent + col_y;
_DAT_005d6368 = tangent1_view * col_offset + tangent1_y * col_y + tangent1_focal * focal_length;
_DAT_005d6370 = tangent2_focal * focal_length + tangent2_y * col_y + tangent2_view * col_offset;

// Reciprocal for perspective division
_DAT_005d6398 = 1.0 / _DAT_005d63cc;

// Then the per-pixel inner loop computes:
// u = _DAT_005d6368 * _DAT_005d6398  (stepping per pixel)
// v = _DAT_005d6370 * _DAT_005d6398
```

## 10. Texture System

### Texture Table Entry (0xC bytes per texture)

```
Offset  Size  Field
0x00    2     refcount        Reference count (incremented per sector/wall use)
0x02    2     flags           Bit 1 = ATX animated texture
0x04    4     padding
0x08    4     handle          Texture resource handle (pointer to loaded texture data)
```

### Texture Data Object

The texture handle points to a structure accessed via vtable:
```
Offset  Field
0x00    vtable/data_ptr     Raw pixel data pointer
0x04    width               Texture width in pixels
0x08    height              Texture height in pixels
0x14    stride              Row stride in bytes
0x1C    log2_height         Log2 of height (byte at offset 0x1F)
```

The texture mask is `height - 1` (stored as `puVar21[2] + -1` = `DAT_005d63b4`).

### Texture Size Detection in Renderer

The wall/floor renderers dispatch to different inner loop implementations based on texture height:
```c
if (texMask == 0x1FF) → 512-pixel texture renderer
if (texMask == 0xFF)  → 256-pixel texture renderer
if (texMask == 0x7F)  → 128-pixel texture renderer
if (texMask == 0x3F)  → 64-pixel texture renderer
else                  → generic texture renderer
```

### Texture Scale: 1 World Unit = 8 Texels

The constant `_DAT_00500054` = **8.0** is used to scale LVT texture offsets to pixel coordinates during level loading. This establishes that **1 world unit = 8 texels** for wall textures.

For floor/ceiling textures, the scale factors `_DAT_005008ac` and `_DAT_005008b0` serve a similar purpose.

### Palette Index 0 Transparency

Palette index 0 is transparent. The RLE decoder (confirmed in session 7) handles this: when decompressing columns, index 0 pixels are left as RGBA (0,0,0,0). The renderer's inner loop functions (the LAB_004d* variants) check for index 0 to implement transparency blending.

### ATX Animated Textures

ATX textures are detected during loading by file extension comparison:
```c
// Extension check: ".ATX" vs ".PCX"
if (extension == "ATX") {
    tex_entry->flags |= 2;  // Mark as animated
    handle = FUN_00489ee0(0x41545846, filename);  // magic "FXTA"
} else {
    tex_entry->flags &= ~2;
    handle = FUN_00489ee0(0x5458544d, filename);  // magic "MTXT"
}
```

## 11. Key Constants

```
Address       Value    Name/Purpose
----------    -----    ------------
_DAT_00500054  8.0     Wall texture UV scale (1 world unit = 8 texels)
_DAT_0050095c  ?       Wall texture length scale factor
_DAT_00500958  0.0     Zero threshold for backface culling
_DAT_00500960  0.5     Half-pixel offset for rounding
_DAT_00500964  1.0     Unit value (used in many interpolations)
_DAT_00500974  ?       1/65536.0 or similar fixed-point conversion
_DAT_00500978  ?       Slope threshold for flat detection
_DAT_005008ac  ?       Floor/ceiling texture scale (likely 8.0)
_DAT_005008b0  ?       Floor/ceiling secondary scale factor
_DAT_005008b4  ?       Slope rendering scale
_DAT_00500a60  ?       Wall height scale for TOP/MID/BOT computation
_DAT_00500878  ?       Slope visibility threshold
_DAT_0050085c  ?       Exterior sky column scale factor (1/65536?)
_DAT_005c0fac  ?       Exterior texture scroll offset X
_DAT_005c0fc4  ?       Exterior texture scroll offset Y
_DAT_005007e8  1.0     Unit used in slope normal computation
_DAT_00500794  ?       Time/velocity scale factor
```

## 12. Rendering Pipeline Overview

### Frame Rendering (FUN_004a69e0, entry at 0x004a69e0)

1. **Increment frame counter** (`render_stamp++`)
2. **Initialize rendering state**:
   - Clear wall edge count, window count, sector count
   - Set screen bounds from viewport
   - Initialize clip buffers
3. **Find camera sector** via `FUN_004abfc0`
4. **Handle screen effects** (damage flash, etc.)
5. **Setup light/colormap** resources
6. **Clear per-column strip counter** (each column tracks up to 0x1E = 30 strips)
7. **Call `FUN_004a7430`** (main recursive sector renderer) with camera sector

### Per-Sector Rendering (FUN_004a7430)

1. **Transform sector objects** to view space (for sprite rendering)
2. **Prepare wall list**: `FUN_004c2d40` → `FUN_004c2820` (per-wall view-space projection & clipping)
3. **Sort walls**: by depth (using `_qsort` with comparator at LAB_004aa740)
4. **Prepare wall edges and windows** (for portal detection)
5. **Setup floor/ceiling texture** (locks texture resource, sets up UV params)
6. **Render all walls** in sorted order:
   - For each wall, dispatch to appropriate `wall_FillZBuffer_*` based on type:
     - Type 0 (no texture) → `wall_FillZBuffer_Colored`
     - Type 1 (lit) → `wall_FillZBuffer_Lit`
     - Type 2 (textured) → `wall_FillZBuffer_Textured`
     - Type 3 (transparent) → `wall_FillZBuffer_Trans`
     - Portal walls: render TOP/BOT strips, then add window
   - For portal walls with both TOP and BOT: swap texture data, render both strips
7. **Render floor** (dispatch based on sector flags → flat/sloped/exterior/sky)
8. **Render ceiling** (same dispatch)
9. **Process portal windows** (recursive descent into adjoined sectors):
   - For each portal window:
     - Setup clip bounds from portal edges
     - Set adjoin sector's render stamp
     - Recursively call `FUN_004a7430` for adjoin sector
     - After return: render ADJOIN_MID if flagged
10. **Render sprites** (sorted, billboarded objects within sector)

### Wall Occlusion/Sorting (FUN_004c2f00)

Walls are sorted front-to-back. The sorter handles overlapping walls by:
1. Comparing vertex positions (which wall's vertices are "in front" of the other)
2. Using cross-product tests (`FUN_004afd50`) to determine which side of a wall a point lies on
3. Splitting partially-overlapping column ranges (up to 0x28 = 40 splits)
4. Removing fully-occluded walls from the render list

### Convexity Detection (FUN_004ddaa0)

Per-sector convexity check:
- Walks all walls, checking that each wall's v2 connects to next wall's v1
- Checks angle differences: if any turn > 0x2000 (90 degrees in 14-bit), sector is non-convex
- Returns: 2 = fully convex, 1 = concave, 0 = disconnected
- Convex sectors get flag 0x40000000 in `sector->flags2[0x33]`
- Concave sectors get flag 0x20000000

This flag affects rendering: concave sectors may need different floor tessellation or additional edge processing during scanline fill.
