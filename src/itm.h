/*
 * itm.h - Outlaws ITM item-definition parser
 *
 * ITM is a small text format describing items/weapons/projectiles:
 *
 *   ITEM 1.0
 *   NAME <name>
 *   FUNC <handler>          # engine handler: Weapon_TNTHandler, Inv_Object,
 *                           # shot_ProjectileDispatch, Gen_New, ...
 *   ANIM <sprite file>      # .nwx sprite or .3do model
 *   DATA <count>
 *       STR   KEY  value
 *       INT   KEY  value
 *       FLOAT KEY  value
 *
 * The engine dispatches behavior on FUNC and reads typed DATA fields by key
 * (Ghidra: weapon ITM loader FUN_0046fda0 reads SLOT/AMMO/DAMAGE_1/...).
 * This parser is generic: it exposes NAME/FUNC/ANIM plus key/value lookups.
 */
#pragma once

#include "engine.h"

#define ITM_MAX_FIELDS 48
#define ITM_KEY_LEN    32
#define ITM_STR_LEN    64

typedef enum { ITM_STR, ITM_INT, ITM_FLOAT } ItmFieldType;

typedef struct {
    ItmFieldType type;
    char key[ITM_KEY_LEN];
    char sval[ITM_STR_LEN];   /* STR value (also raw token for INT/FLOAT) */
    i32  ival;
    f32  fval;
} ItmField;

typedef struct {
    char name[ITM_STR_LEN];
    char func[ITM_STR_LEN];
    char anim[ITM_STR_LEN];
    ItmField fields[ITM_MAX_FIELDS];
    u32  field_count;
} ItmFile;

/* Parse an ITM text buffer. Returns true on success. */
bool itm_parse(ItmFile *itm, const char *text, u32 len);

/* Field lookup by key (case-insensitive). Return the field or NULL. */
const ItmField *itm_find(const ItmFile *itm, const char *key);

/* Convenience getters with defaults. */
f32         itm_get_float(const ItmFile *itm, const char *key, f32 def);
i32         itm_get_int(const ItmFile *itm, const char *key, i32 def);
const char *itm_get_str(const ItmFile *itm, const char *key, const char *def);
