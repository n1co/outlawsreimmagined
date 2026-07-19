/*
 * tdo.h - Outlaws 3DO object format parser
 *
 * 3DO files are text-based 3D models (similar to Wavefront OBJ).
 * Format: "3DO 2.0"
 *
 * Structure:
 *   3DONAME <name>
 *   OBJECTS <count>
 *   VERTICES <total>
 *   POLYGONS <total>
 *   PALETTE <pal.PAL>
 *   TEXTURES <count>
 *     TEXTURE: <filename>  # index
 *   OBJECT "<name>"
 *     TEXTURE <idx>
 *     VERTICES <count>
 *       idx: x y z
 *     TRIANGLES <count>
 *       idx: a b c <color> <shading> # material
 *     TEXTURE VERTICES <count>
 *       idx: u v
 *     TEXTURE TRIANGLES <count>
 *       idx: a b c
 */
#pragma once

#include "engine.h"

#define TDO_MAX_OBJECTS    32
#define TDO_MAX_VERTS      4096
#define TDO_MAX_TRIS       4096
#define TDO_MAX_TEXTURES   16
#define TDO_MAX_NAME       64

typedef struct {
    Vec3 pos;
} TdoVertex;

typedef struct {
    i32 a, b, c;           /* Vertex indices */
    i32 ta, tb, tc;        /* Texture vertex indices */
    i32 color;             /* Palette color */
    i32 shading;           /* 0=flat, 1=gouraud, 2=phong */
} TdoTriangle;

typedef struct {
    char     name[TDO_MAX_NAME];
    i32      texture_idx;  /* Index into model textures, -1 = none */

    TdoVertex   verts[TDO_MAX_VERTS];
    u32          vert_count;

    Vec2         tex_verts[TDO_MAX_VERTS];
    u32          tex_vert_count;

    TdoTriangle  tris[TDO_MAX_TRIS];
    u32          tri_count;
} TdoObject;

typedef struct TdoModel {
    char name[TDO_MAX_NAME];
    char palette[TDO_MAX_NAME];
    char textures[TDO_MAX_TEXTURES][TDO_MAX_NAME];
    u32  texture_count;

    TdoObject objects[TDO_MAX_OBJECTS];
    u32       object_count;
} TdoModel;

/* Parse a 3DO model from text data. Returns true on success. */
bool tdo_parse(TdoModel *model, const char *text, u32 text_len);

/* Free model resources (currently no dynamic alloc, just resets). */
void tdo_free(TdoModel *model);
