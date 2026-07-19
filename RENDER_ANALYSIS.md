# Outlaws Map Geometry Rendering — Authoritative Reference

Goal: build a correct map renderer (walls, flats, adjoins/portals, slopes, UVs) with **no hacks**, by
cross-referencing:

- **TFE** (The Force Engine, Jedi/Dark Forces) — `TheForceEngine/TheForceEngine/` (paths below are relative to it).
- **Outlaws binary** `build/data/olwin.exe` in Ghidra (addresses are virtual, image base `0x400000`).

Outlaws **evolved** the Jedi engine: it added sloped floors/ceilings (`SLOPEDFLOOR`/`SLOPEDCEILING`) and
per-wall `DADJOIN` (stacked vertical portals). Everything else about wall/flat texturing is **identical to
Dark Forces** — this was verified: Outlaws' `Wall_ComputeOpenings` computes the exact same texel-height
formulas as TFE's `rwall.cpp`, and the load-time UV scale constant is the same `8.0`.

---

## 0. The one constant that governs everything: 8 texels per world unit

Both engines map **1 world unit = 8 texture texels**. A 64×64 texture therefore tiles every **8 world units**;
a 128-wide texture every 16.

Outlaws binary (all extracted directly from `olwin.exe`):

| Constant | Value | Meaning | Where |
|---|---|---|---|
| `_DAT_00500054` | **8.0** | multiplies wall MID/TOP/BOT/OVERLAY U and V offsets when a wall is loaded | `level_LoadSectors` @ `0x41de4c` (uses at `0x41eebf,0x41eed6,0x41ef78,0x41ef8f,0x41f027,0x41f03e,0x41f0de,0x41f0f5`) |
| `_DAT_00500a60` | **8.0** | multiplies world-height differences into wall opening texel spans | `Wall_ComputeOpenings` @ `0x4e4600` |
| `_DAT_00500a5c` | **9.999e-05** | adjoin height-match epsilon (treat heights within 1e-4 as equal) | `Wall_ComputeAdjoinType` @ `0x4e3ce0` |
| `_DAT_00500030` | **0.0** | wall-length zero test | `level_LoadSectors` |

TFE equivalents (the `<<3` shifts and `8.0f`/`fixed16_16(8)` literals):

- `Level/level.cpp:589` `wall->texelLength = wall->length * 8;`
- `Level/level.cpp:556-582` wall offsets scaled ×8 on load: `wall->midOffset.x = floatToFixed16(midOffsetX) * 8;` (top/bot/sign identical)
- `Level/rwall.cpp:51,57,66-92` `topTexelHeight=(next->ceil-cur->ceil)<<3; botTexelHeight=(cur->floor-next->floor)<<3; midTexelHeight=(floor-ceiling)<<3;`
- `Renderer/RClassic_Float/rflatFloat.cpp:198,266` `const f32 worldToTexelScale = 8.0f;`
- `Renderer/RClassic_Fixed/rflatFixed.cpp:235,306` `const fixed16_16 worldToTexelScale = fixed16_16(8);`

**Your renderer already uses `WU_TO_TEXEL 8.0f` (renderer.c:422) — that master scale is correct.** The bugs
are in *what the 8 is multiplied against* (the V anchoring) and in the adjoin/portal branch logic, not the scale.

---

## A. Wall U (horizontal) coordinate

**Formula:** `U_texel = (worldDistanceAlongWall + LVT_u_offset) * 8`, then `& (texWidth-1)`, then optionally
flip if `WF1_FLIP_HORIZ`.

- The wall's full length in texels is precomputed: `texelLength = length*8` (`level.cpp:589`). In Outlaws,
  `level_LoadSectors` computes and stores wall length (`FUN_004af710(dx*dx+dz*dz)` sqrt, kept at wall `+0x58`)
  and the unit direction (`+0x4c/+0x50`).
- Per-column, perspective-correct: `uCoord = uCoord0 + midOffset.x + (interpolated dist)` where `uCoord0` is the
  segment's left-edge accumulated texel-U (advanced by frustum clipping). `rwallFloat.cpp:831-833`, sampled
  `texelU = floorFloat(uCoord) & (texWidth-1)` (`:838`), flip `WF1_FLIP_HORIZ` → `texWidth-texelU-1` (`:840`).
  Top strip uses `+topOffset.x` (`:1490`), bottom `+botOffset.x` (`:1268`).

**Your code (renderer.c:503-528) is essentially correct for U:**
```c
u0 = u_off * (8/texW);
u1 = (u_off + wall_len) * (8/texW);
```
Because the engine stores `u_off*8` at load and adds `dist*8`, `(u_off+dist)*8/texW` is algebraically identical.
The `flags & 0x04` flip (renderer.c:527) also correctly matches `WF1_FLIP_HORIZ` (bit 2 = 0x04, `rwall.h:31`).

> ⚠️ One caveat: U must accumulate along the **whole wall from its first vertex**, using each wall's *own*
> `u_offset`. You build each wall quad independently starting `dist=0` at `v1` — that is correct **per wall
> segment** because each LVT wall carries its own `u_offset`. No change needed.

---

## B. Wall V (vertical) coordinate — **the primary bug source**

### The rule (identical for every wall section)

Every wall section — solid MID, TOP step, BOT step, and MID mask — is **anchored at its LOWER (bottom) edge**,
with V measured **in texels increasing UPWARD**, plus the LVT V offset:

```
V_texel(worldY) = (worldY - sectionBottomY) * 8 + LVT_v_offset * 8
```

so on the quad:

```
v_at_section_bottom = (LVT_v_offset)            * 8 / texH      // SMALL v (this is the anchor)
v_at_section_top    = (LVT_v_offset + sectionH) * 8 / texH      // LARGE v
sectionH = sectionTopY - sectionBottomY   (world units)
```

Per-section bottom anchor:

| Section | Bottom edge (anchor) | Top edge | texel height | TFE cite | Outlaws cite |
|---|---|---|---|---|---|
| Solid MID | `sector.floor_y` | `sector.ceil_y` | `(ceil-floor)*8` | `rwallFloat.cpp:843-856` | `Wall_ComputeOpenings` else-branch: `+0x44=(ceil-floor)*8` |
| TOP step | **adjoin** `ceil_y` | `sector.ceil_y` | `(ownCeil-adjCeil)*8` | `rwallFloat.cpp:1504-1505` | `+0x40=(ownSec.ceil - adj.ceil)*8` |
| BOT step | `sector.floor_y` | **adjoin** `floor_y` | `(adjFloor-ownFloor)*8` | `rwallFloat.cpp:1285-1287` | `+0x48=(adj.floor - ownSec.floor)*8` |
| MID mask (adjoin) | max(floor,adjFloor) | min(ceil,adjCeil) | opening height ×8 | `rwallFloat.cpp:911-978` | `+0x44` opening cases in `Wall_ComputeOpenings` |

TFE solid-wall proof (`rwallFloat.cpp:843-856`, fixed twin `rwallFixed.cpp:808-820`):
```c
f32 wallHeightPixels = y0F - y0C + 1.0f;        // floor pixel - ceil pixel
f32 vCoordStep       = midTexelHeight / wallHeightPixels;   // = (worldH*8)/pixels
f32 vPixelOffset     = y0F - f32(bot) + 0.5f;   // 0 at FLOOR, grows toward ceiling
f32 v0               = vCoordStep * vPixelOffset;
s_vCoordFixed        = floatToFixed20(v0 + cachedWall->midOffset.z);  // + LVT v (already *8)
```
`y0F` = **floor** projection ⇒ V = `midOffset.z` at the floor, `midOffset.z + midTexelHeight` at the ceiling.
Outlaws sector struct confirms `+0x84 = ceil_y`, `+0x88 = floor_y` (from `level_LoadSectors`: the `CEILING`
block writes `puVar15[0x21]`=0x84, the `FLOOR` block writes `puVar15[0x22]`=0x88), and `Wall_ComputeOpenings`
computes the identical `(ownCeil-adjCeil)*8`, `(adjFloor-ownFloor)*8`, `(ceil-floor)*8` texel spans.

### `WF1_TEX_ANCHORED` (bit 4 = 0x10) — NOT a per-pixel pegging flag

It has nothing to do with top-vs-bottom pegging. It only matters when a surface **moves** (elevator/door): when
a sector's floor shifts by `Δ`, the stored V offset is compensated so the texture keeps world alignment
(`rsector.cpp:340-349`):
```c
fixed16_16 textureOffset = floorDelta * 8;
if (wall->flags1 & WF1_TEX_ANCHORED) {
    if (wall->nextSector) wall->botOffset.z -= textureOffset;   // anchored: stay put
    else                  wall->midOffset.z -= textureOffset;
}   // (un-anchored walls scroll with the surface)
```
`WF1_SIGN_ANCHORED` (bit 12) does the same for `signOffset.z`; mirror walls flip the sign (`rsector.cpp:369-384`).

### What your renderer gets wrong (renderer.c:513-545, and every strip)

Your `build_wall_quad` default (renderer.c:519-523) is **top-pegged**:
```c
v_top = v_off * v_scale;              // at CEILING  = offset       (WRONG anchor)
v_bot = (v_off + wall_h) * v_scale;   // at FLOOR    = offset+H
```
This anchors the main texture to the **ceiling** and runs V downward — the engine anchors to the **floor** and
runs V upward. It is inverted top-to-bottom **and** pinned to the wrong edge.

Additionally, all your call sites pass a hand-computed `v_adj = LVT_v - someHeight`:
- solid: `wall->mid.offset.v - sec->floor_y` (renderer.c:868)
- top strip: `wall->top.offset.v - adj_ceil` (renderer.c:895, 956, 1033)
- bot strip: `wall->bot.offset.v - sec->floor_y` (renderer.c:905, 1045)
- mid mask: `wall->mid.offset.v - open_bot` (renderer.c:926, 1071)

These `- height` subtractions are hacks trying to emulate anchoring by hand. **The engine never subtracts a
sector height from the offset.** The correct anchoring falls out automatically from choosing the right bottom
edge and letting V grow upward. Feeding both a hand-adjusted offset *and* a wrong pegging direction is why
CANYON textures slide/misalign.

**Correct `build_wall_quad` V (replace lines 513-523):**
```c
// section anchored at its bottom edge (y_bot); V grows +8 texels/world-unit upward.
// v_off is the RAW LVT v offset for this section's texture slot (mid/top/bot). No height subtraction.
f32 v_bot = (v_off)            * v_scale;   // bottom vertex (anchor)
f32 v_top = (v_off + wall_h)   * v_scale;   // top vertex, wall_h = y_top - y_bot
```
and at the call sites pass the **raw** `wall->mid/top/bot.offset.v` (no `- floor_y`, no `- adj_ceil`, etc.),
with the correct bottom edge per the table above (which you already pass as `y_bot`).

> Note on image orientation: the engine samples `texelV = floor(V) & (texH-1)` from top-of-image rows. If, after
> the fix, textures appear vertically mirrored, your PCX upload row-order differs from the engine's; flip by
> uploading rows bottom-first (or negate v and add 1). Do **not** re-introduce a per-quad height subtraction to
> "fix" orientation — verify against the anchor rule.

---

## C. Flats (floor / ceiling) texture mapping

**Formula (world-planar projection):**
```
U_texel = (worldX - sector.floorOffset.x) * 8
V_texel = (worldZ - sector.floorOffset.z) * 8
```
Sampled perspective-correct per scanline; the sector offset fields are themselves already ×8-scaled at load.
Ceiling uses `ceilOffset`, floor uses `floorOffset`.

TFE floor (`rflatFloat.cpp:224-275`; ceiling `:160-208`; fixed `rflatFixed.cpp:235,306`):
```c
f32 textureOffsetU = cameraPos.x - sectorCached->floorOffset.x;
f32 textureOffsetV = sectorCached->floorOffset.z - cameraPos.z;
const f32 worldToTexelScale = 8.0f;
s_scanlineU0 = floatToFixed20((u0 - textureOffsetU) * worldToTexelScale); // u0 reconstructs world X
s_scanlineV0 = floatToFixed20((v0 - textureOffsetV) * worldToTexelScale); // v0 reconstructs world Z
```
Tiling is a raw power-of-two mask: `rflatFixed.cpp:166` `texel=(floor(U)&63)*64 + (floor(V)&63)`;
`rflatFloat.cpp:92` generalizes to any size with width/height masks. `floorOffset`/`ceilOffset` are
`vec2_fixed` on `RSector` (`rsector.h:112-113`).

**Your code (renderer.c:599-603):**
```c
v.u = (u_off + verts_xz[i].x) * (8/texW);
v.v = (v_off + verts_xz[i].y) * (8/texH);   // verts_xz[i].y is world Z
```
This is **structurally correct** (world XZ × 8 / texDim) — which is why TOWN flats look right. Two refinements:

1. **Offset sign.** Engine *subtracts* the sector offset (`worldX - floorOffset.x`); you *add* it
   (`u_off + x`). For most sectors the offset is 0 so it doesn't matter, but where a sector sets a non-zero
   floor/ceil UV offset your result is shifted the wrong way. Use `(x - u_off)` / `(z - v_off)`.
   (Watch that Outlaws stores these already ×8 internally, but your LVT parser reads the raw text value into
   `floor_offset.u/v` (lvt.c:255-256, 270-271) — so multiply by 8 exactly once, as you do.)
2. **Sloped flats.** DF/TFE flats are never sloped, so TFE offers no runtime reference here — this is an
   Outlaws addition. The correct behavior is: keep the **planar XZ projection unchanged** (U,V still come from
   world X,Z), and only displace the vertex **height** via the slope plane. You already do exactly that
   (renderer.c:594-596 sets `v.y` from the slope, UV from XZ) — this is right. Do **not** project UV along the
   slope surface; the height is a pure function of (x,z) and the texture is mapped top-down.

Slope height math (already fixed and shared with collision, `lvt.c:484-502`) matches TFE
`editGeometry.cpp:3142-3145` `slope_getHeightAtXZ = dp - x*normal.x - z*normal.z` and the
`slope_calculatePlane` hinge/normal construction (`:3152-3250`, comment "mirrors Outlaws behavior"). Outlaws
resolves the slope hinge in `level_LoadSectors` (@`0x41de4c`, the `puVar15[0x32] & 0x40000000`/`0x80000000`
blocks near `LAB_0041f986`), keyed by `SLOPEDFLOOR/SLOPEDCEILING <hingeSector> <hingeWall> <angle>` — matching
your parser's fix (lvt.c:230-248: first token = self sector id, second = pivot wall, third = angle).

---

## D. Portal vs solid, step strips, and Outlaws additions

### D.1 Solid vs portal is decided by the ADJOIN pointer, **not by flags**

- A wall is **solid** iff it has no adjoin (`nextSector == null`). Dispatch: `rsectorFloat.cpp:326`
  `if (!nextSector) wall_drawSolid(...)`.
- In Outlaws load, `level_LoadSectors` sets `wall+0x60` (adjoin sector ptr) to `0` when the LVT `ADJOIN` field
  is `-1` (`iStack_244 == -1 → puVar11[0x18]=0`), else to the resolved sector. `wall+0x68` is the DADJOIN ptr,
  set the same way from the LVT `DADJOIN` field.
- **Maps directly to your `LvtWall.adjoin` (< 0 = solid) — which you use correctly at renderer.c:855.**

### D.2 Which step strips exist is decided by **height comparison**, not flags

`Wall_ComputeAdjoinType` (@`0x4e3ce0`) and TFE `wall_setupAdjoinDrawFlags` (`rwall.cpp:30-37`) both compare
this sector's floor/ceil against the adjoin's (slope-aware, with the 1e-4 epsilon), and set:

- **TOP strip** exists iff `adjoin.ceil < sector.ceil` (WDF_TOP=1). Renders `sector.ceil → adjoin.ceil`, TOP texture.
- **BOT strip** exists iff `adjoin.floor > sector.floor` (WDF_BOT=2). Renders `adjoin.floor → sector.floor`, BOT texture.
- The remaining **opening** (`max(floor,adjFloor) → min(ceil,adjCeil)`) is a see-through portal into the
  neighbour; the neighbour's geometry is drawn through it (`wall_addAdjoinSegment`, `rwallFloat.cpp:2201`).

**Your `has_top = adj_ceil < sec->ceil_y` / `has_bot = adj_floor > sec->floor_y` (renderer.c:991-992) matches
this exactly.** Keep it. The bug is not here — it's that you *also* branch on the `ADJOIN_MID (0x2000)` flag and
on `window`/`door` special cases (renderer.c:882-1077), layering hacks over the clean height rule.

### D.3 MID texture on an adjoin wall (mask walls / glass / fences)

- The only flag that controls this is **`WF1_ADJ_MID_TEX` = bit 0 (0x01)** (`rwall.h:25` "mid texture rendered
  even with adjoin — maskwall"). Draw a **transparent** MID quad spanning the portal opening
  (`max(floor,adjFloor) → min(ceil,adjCeil)`), bottom-anchored, offset `midOffset.z` (`rsectorFloat.cpp:488-492`
  → `wall_drawTransparent`, `rwallFloat.cpp:911-978`).
- **Your bit-0 check (renderer.c:920, 1065) is correct.** But it should be the *sole* condition for the MID
  overlay on a portal wall — independent of your `0x2000` "ADJOIN_MID" classification.

### D.4 Outlaws-specific: `DADJOIN` (stacked vertical portals)

DF has no `DADJOIN`. Outlaws adds a second adjoin (`wall+0x68`, your `LvtWall.dadjoin`) so one wall hosts **two
stacked openings** (e.g. a walkway that passes both under a high sector and over a low one).
`Wall_ComputeAdjoinType`'s second branch (when `wall+0x68 != 0`) computes a 3-way (`uVar9` starts at `4`) type
from three sectors (own, adjoin `+0x60`, dadjoin `+0x6c`). `Wall_ComputeOpenings`'s first branch
(`iVar3 = wall+0x68 != 0`) then produces top (`+0x40`), mid (`+0x44`), and bottom (`+0x48`) spans across the two
openings. Your DADJOIN handling (renderer.c:933-988) has the right *shape* (TOP / MID between openings / BOT),
but inherits the same wrong V anchoring as everything else — fix B first, then this becomes correct.

---

## E. Wall flag reference (TFE `rwall.h`, WallFlags1)

These are the **verified DF/Jedi** bit meanings. Bits you rely on that match: 0x01, 0x04. Bits you mis-use:
0x40 (see below).

| Bit | Value | Name | Render effect |
|---|---|---|---|
| 0 | 0x0001 | `WF1_ADJ_MID_TEX` | draw MID even on an adjoin (mask wall) — **your bit-0 use is correct** |
| 1 | 0x0002 | `WF1_ILLUM_SIGN` | sign fullbright |
| 2 | 0x0004 | `WF1_FLIP_HORIZ` | mirror U — **your 0x04 flip is correct** |
| 3 | 0x0008 | `WF1_CHANGE_WALL_LIGHT` | |
| 4 | 0x0010 | `WF1_TEX_ANCHORED` | keep texture world-aligned when surface moves (NOT a pegging flag) |
| 5 | 0x0020 | `WF1_WALL_MORPHS` | |
| 6-9 | 0x40-0x200 | `WF1_SCROLL_TOP/MID/BOT/SIGN_TEX` | animated scroll |
| 10 | 0x0400 | `WF1_HIDE_ON_MAP` | automap |
| 11 | 0x0800 | `WF1_SHOW_NORMAL_ON_MAP` | automap |
| 12 | 0x1000 | `WF1_SIGN_ANCHORED` | |
| 13 | 0x2000 | `WF1_DAMAGE_WALL` | |
| 14 | 0x4000 | `WF1_SHOW_AS_LEDGE_ON_MAP` | automap |
| 15 | 0x8000 | `WF1_SHOW_AS_DOOR_ON_MAP` | automap |

`WallFlags3` (`rwall.h:45-51`) are **collision, not render**: `WF3_ALWAYS_WALK`(0), `WF3_SOLID_WALL`(1),
`WF3_PLAYER_WALK_ONLY`(2), `WF3_CANNOT_FIRE_THROUGH`(3).

> ⚠️ **Flag-bit caveat for Outlaws.** Your `LVT_WALL_FLAG_ADJOIN_MID = 0x2000` and `SKY_BOUNDARY = 0x20000`
> (lvt.h:37-38) do **not** line up with TFE's WallFlags1 (0x2000 = `WF1_DAMAGE_WALL` there). Outlaws may pack
> its two LVT flag words differently from DF's flags1/flags2/flags3. Since the engine decides portal
> structure from **geometry** (adjoin pointer + height comparison), the safest correct renderer does **not**
> depend on `0x2000` at all: use `adjoin>=0` for portal, height comparison for strips, and bit-0 for the MID
> mask. Treat `0x2000`/`0x20000` as suspect until confirmed by decompiling Outlaws' wall-flag consumer.

---

## F. Field mapping (Outlaws internal ↔ TFE ↔ your `lvt.h`)

| Concept | Outlaws struct | TFE (`RSector`/`RWall`) | Your field |
|---|---|---|---|
| sector ceiling Y | sector `+0x84` | `ceilingHeight` | `LvtSector.ceil_y` |
| sector floor Y | sector `+0x88` | `floorHeight` | `LvtSector.floor_y` |
| floor tex UV offset | sector `+0x60..` (×8) | `floorOffset` (vec2_fixed) | `LvtSector.floor_offset{u,v}` |
| ceil tex UV offset | sector `+0x5c..` (×8) | `ceilOffset` | `LvtSector.ceil_offset{u,v}` |
| wall adjoin (portal) | wall `+0x60` (0=solid) | `nextSector` (null=solid) | `LvtWall.adjoin` (<0=solid) |
| wall dadjoin | wall `+0x68` | — (Outlaws only) | `LvtWall.dadjoin` |
| wall mirror | wall `+0x64` | `mirrorWall` | `LvtWall.mirror` |
| wall length | wall `+0x58` | `length`/`texelLength` | computed in `build_wall_quad` |
| MID/TOP/BOT U off (×8) | wall `+0x20/0x24/0x28..` | `midOffset.x`/`topOffset.x`/`botOffset.x` | `LvtWall.{mid,top,bot}.offset.u` (raw; ×8 at use) |
| MID/TOP/BOT V off (×8) | wall `+0x30/0x34/0x38..` | `.z` of same | `LvtWall.{mid,top,bot}.offset.v` |
| top/mid/bot texel span | wall `+0x40/0x44/0x48` | `topTexelHeight/midTexelHeight/botTexelHeight` | derived from heights in renderer |

---

## G. Prioritized fix list for `src/renderer.c`

### 1. (ROOT CAUSE) Wall V anchoring — floor/bottom-anchored, V upward, NO height subtraction
`build_wall_quad` (renderer.c:513-523) is top-pegged and inverted. Replace with:
```c
f32 v_bot = v_off * v_scale;                 // bottom vertex = anchor
f32 v_top = (v_off + wall_h) * v_scale;      // top vertex
```
and delete the `flags & 0x40` "TEX_ANCHORED bottom-peg" branch (renderer.c:514-518) — 0x40 is
`WF1_SCROLL_TOP_TEX`, not an anchor, and anchoring is not per-pixel. Then at **every** call site pass the **raw**
LVT V offset (remove `- sec->floor_y`, `- adj_ceil`, `- adj_floor`, `- open_bot`): lines 868, 895, 905, 926,
956, 965, 976, 1018, 1033, 1045, 1071. Keep the `y_bot`/`y_top` world heights you already pass (they encode the
correct anchor per the D.2 table). Source: TFE `rwallFloat.cpp:843-856,1285-1287,1504-1505`; Outlaws
`Wall_ComputeOpenings` @`0x4e4600` (spans `(ceil-floor)*8`, `(ownCeil-adjCeil)*8`, `(adjFloor-ownFloor)*8`).

**Expected effect:** fixes the "texture bugs everywhere" on CANYON (walls with non-zero floor heights and
non-zero V offsets — most of a canyon — are exactly where the `- floor_y` hack and inverted pegging diverge;
TOWN mostly has floor_y≈0 so it looked OK).

### 2. Stop branching portal structure on the `0x2000`/window/door flags; use geometry only
Collapse the `ADJOIN_MID (0x2000)` special case (renderer.c:882-932) into the plain portal path
(renderer.c:989-1078). The correct, complete rule for any wall with `adjoin >= 0`:
```
if (adj_ceil  < sec->ceil_y)  draw TOP strip [adj_ceil .. sec->ceil_y]  with top_tex
if (adj_floor > sec->floor_y) draw BOT strip [sec->floor_y .. adj_floor] with bot_tex
if (wall->flags & 0x01)       draw MID mask  [max(floor,adjFloor)..min(ceil,adjCeil)] transparent
if (wall->dadjoin >= 0)       additionally handle the second (lower) opening (D.4)
```
Source: `rsectorFloat.cpp:326-388`, `rwall.cpp:30-37`; Outlaws `Wall_ComputeAdjoinType` @`0x4e3ce0`.

**Expected effect:** removes phantom/missing walls where CANYON walls carry flag bits your `0x2000` logic
misreads. Also stops MID fences from being drawn as full-height solids.

### 3. Flat offset sign
renderer.c:601-602 uses `(u_off + x)`, `(v_off + z)`; engine subtracts the sector offset. Change to
`(x - u_off) * scale`, `(z - v_off) * scale`. Source: `rflatFloat.cpp:224-275`. (Low visual impact unless a
sector sets non-zero floor/ceil UV offsets — but required for correctness.)

### 4. Confirm Outlaws LVT wall-flag bit layout before trusting `0x2000`/`0x20000`
`lvt.h:37-38` guesses conflict with TFE. Decompile the Outlaws wall-flag consumer (start from
`Wall_ComputeAdjoinType`/`Render_AddWallWindow` @`0x4a9e30`, and `level_LoadSectors`' `puVar11[0x1c]` flag
stores) to pin the real bit meanings. Until then, rely on geometry (fixes 1-2), which needs none of these bits
except `WF1_ADJ_MID_TEX` (0x01) and `WF1_FLIP_HORIZ` (0x04), both already verified correct.

### Non-issues (verified correct — do not touch)
- Master texel scale `WU_TO_TEXEL = 8.0` (renderer.c:422). ✓
- Wall U accumulation `(u_off + wall_len)*8/texW` (renderer.c:510-511). ✓
- `WF1_FLIP_HORIZ` = 0x04 (renderer.c:527). ✓
- MID-mask condition = flag bit 0 (renderer.c:920, 1065). ✓
- Solid vs portal = `adjoin < 0` (renderer.c:855). ✓
- Flat world-XZ planar projection incl. slopes displacing only Y (renderer.c:594-603). ✓ (aside from fix #3 sign)
- Slope plane math (`lvt.c:484-502`) matches TFE `editGeometry.cpp:3142-3250`. ✓

---

## H. Source index

**Outlaws (`olwin.exe`):** `level_LoadSectors@0x41de4c`, `Wall_ComputeAdjoinType@0x4e3ce0`,
`Wall_ComputeOpenings@0x4e4600`, `Render_AddWallEdge@0x4a9c20`, `Render_AddWallWindow@0x4a9e30`,
`Level_Load@0x41bb10`, `level_LoadObjects@0x41ccc0`. Constants: `8.0@0x500054`, `8.0@0x500a60`,
`1e-4@0x500a5c`.

**TFE (`TheForceEngine/TheForceEngine/`):** `TFE_Jedi/Renderer/RClassic_Float/rwallFloat.cpp` (U:831-840,
solid V:843-856, bot:1103-1337, top:1339-1556, mask:911-978, adjoin seg:2201-2224),
`RClassic_Float/rflatFloat.cpp:160-275`, `RClassic_Fixed/rwallFixed.cpp:808-820`,
`RClassic_Fixed/rflatFixed.cpp:166,235,306`, `RClassic_Float/rsectorFloat.cpp:326-388,488-492`,
`Level/rwall.cpp:30-37,51-92`, `Level/rwall.h:15-51`, `Level/level.cpp:556-589`,
`Level/rsector.cpp:340-384`, `Level/rsector.h:112-113`, `Editor/LevelEditor/editGeometry.cpp:3142-3250`.

---

# APPENDIX — Ghidra pass 2 (2026-07-19): olwin.exe rasterizer ground truth

All items below were extracted from the binary itself (functions renamed in the Ghidra DB) and
are now implemented in src/renderer.c / src/world.c / src/lvt.c.

## Texture memory layout (Pcx_ReadImage @0x4a3040, MTXT loader @0x44b1a0)
Level textures are stored **column-major with columns bottom-up**: `mem[x*H + (H-1-pcxRow)]`.
Texel V=0 = image BOTTOM row — identical to the DF BM convention. GL port: level textures
(walls/flats/ATX/sky) are row-flipped at upload (`flip_rgba_rows`, world.c) so GL v=0 = bottom row.

## Wall rasterizer (wall_FillZBuffer_Textured @0x4c71c0, Wall_ProjectToScreen @0x4c2820)
- Columns drawn bottom→up, V starts at the part's bottom edge, +8 texels/wu upward. No inversion.
- U = uOffset at stored vertex1, increasing toward vertex2; front-face test
  `v2.x*v1.z - v1.x*v2.z > 0` guarantees v1 projects screen-left. (The earlier "U mirrored"
  hypothesis was a misdiagnosis of the vertical flip.)
- WF 0x4 flip: `Uidx = texW - Uidx - 1` (texture-space mirror, u→1-u), applied to the main
  texture only — never to signs.
- Projection: screenX = centerX + focal*viewX/viewZ; yaw=0 faces +Z, +X is screen-right
  (Render_SetCamera @0x4a66e0, Math_SinCos16k @0x4af400, 16384 units/turn; pitch = Y-shear).

## Flats (flat_FillZBuffer_Floor @0x4b4bb0 / _Ceiling @0x4b42a0) — WITH ROTATION
```
U_texel =  8*((z-offZ)*cos(rot) - (x-offX)*sin(rot))   ; U = within-column index (image rows)
V_texel = -8*((x-offX)*cos(rot) + (z-offZ)*sin(rot))   ; V = column index (image x)
```
Floor offsets +0x5c/+0x6c, rot +0x7c; ceiling +0x60/+0x70/+0x7e. level_LoadSectors confirms LVT
token order `FLOOR Y <y> <tex> <offX> <offZ> <rotDeg>`. GL: u=-(dx*c+dz*s)*8/W, v=(dz*c-dx*s)*8/H.

## Sky (sky_DrawCeiling @0x4b6190, sky_BuildColumnAngleTable @0x4b09b0, offsets @0x4b0920)
- Sector flags bit0 = ceiling sky, bit1 = floor sky (pit). The sky texture is the sky sector's
  own ceiling (resp. floor) texture.
- U(x) = (yawTurns + atan((x-centerX)/focal)/2π)*parallaxX + ceilOffX, wrap texW.
- V(y) = (texH-1) - ((yDown - viewH/2)*(640/viewH) + 100 - pitchTurns*parallaxY + ceilOffZ), wrap.
- Software renderer hardcodes parallax 1024×1024 (setter @0x4b0990); LVT PARALLAX matches anyway.
Implemented as the fullscreen `prog_sky` shader.

## Wall signs (inline in @0x4c71c0; blitters @0x4d0d80/0x4d0e10/0x4d3d90)
Sign drawn where the host part's U (incl. part's own u offset) ∈ [ov.u*8 .. +signW texels];
V anchored at the part's bottom edge, positive ov.v moves the sign DOWN ov.v wu; clipped to the
part; never flipped; flag 0x2 = fullbright; color-0 transparent. Implemented as `build_sign_quad`
+ is_sign meshes drawn with polygon offset.

## Status
All of the above implemented and verified 2026-07-19: 24/24 level sweep PASS; TOWN sheriff/jail
signs read correctly; HIDEOUT/SIMMS/DRYGULCH skies correct. Remaining gaps: colormap-based light
diminishing, glass translucency LUT, MORPH-door static-mesh workaround, wall flag 0x20000 skip.
