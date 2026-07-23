/*
 * lvb.h - Binary .lvb level loader (historical missions)
 *
 * The historical missions (Civil War / Ice Caves / Villa / Marshal Training /
 * Wharf Town) ship only as binary .lvb, not text .LVT. The .lvb is the same
 * level data in the engine's tagged binary chunk stream (Tokenize_* in olwin.exe):
 *   file: [u32 0][u32 ".LVT" big-endian magic] then chunks
 *   chunk: [u32 tag][u8 field_count][fields]
 *   field: 4 bytes for %d/%u/%f/%x ; null-terminated string for %s/%c
 * The tag schema mirrors the text LVT field layout exactly (level_LoadSectors
 * @0x41de4c, level_ParseLVTHeader @0x41d7a0). This parser fills the same
 * LvtLevel that lvt_parse() produces, so the rest of the engine is unchanged.
 */
#pragma once

#include "lvt.h"
#include "obt.h"

/* Parse a binary .lvb buffer into `level`. Returns true on success (call
 * lvt_free() to release, same as lvt_parse). */
bool lvb_parse(LvtLevel *level, const u8 *data, u32 size);

/* Quick check: does this buffer look like a binary .lvb (0-magic + ".LVT")? */
bool lvb_is_binary(const u8 *data, u32 size);

/* Parse a binary .obb object list (same tagged chunk stream, ".OBT" magic) into
 * `table` — the same ObtTable obt_parse() produces. Fills table->objects (caller
 * frees). Returns true on success. */
bool obb_parse(ObtTable *table, const u8 *data, u32 size);
bool obb_is_binary(const u8 *data, u32 size);
