/*
 * obt.h - Outlaws OBT (Object Table) parser
 *
 * OBT files list all objects (enemies, items, decorations) placed in a level.
 * Text format: "OBT 1.0"
 *
 * Each entry:
 *   NAME: <type>  ID: <hex>  SECTOR: <hex>  X: <f>  Y: <f>  Z: <f>
 *   PCH: <f>  YAW: <f>  ROL: <f>  FLAGS: <u> <u>
 */
#pragma once

#include "engine.h"

#define OBT_MAX_OBJECTS  2048
#define OBT_NAME_LEN     64

typedef struct {
    char  type[OBT_NAME_LEN];  /* Object type (links to ITM definition) */
    u32   id;                  /* Unique object ID (hex) */
    u32   sector_id;           /* Sector containing this object */
    Vec3  pos;                 /* World position (LVT XYZ) */
    f32   pitch, yaw, roll;    /* Euler angles (degrees) */
    u32   flags[2];
} ObtObject;

typedef struct {
    char       level_name[64];
    ObtObject *objects;
    u32        object_count;
} ObtTable;

/* Parse an OBT file from text data. Returns true on success. */
bool obt_parse(ObtTable *table, const char *text, u32 text_len);

/* Free OBT resources. */
void obt_free(ObtTable *table);

/* Find objects of a specific type (e.g. "PLAYER"). Returns count found. */
u32 obt_find_by_type(const ObtTable *table, const char *type,
                     ObtObject **results, u32 max_results);
