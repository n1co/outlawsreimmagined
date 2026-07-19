/*
 * lab.h - LucasArts LAB archive reader
 *
 * LAB format (LABN):
 *   Header: magic "LABN", version, file_count, name_list_length
 *   File entries: array of {name_offset, data_offset, size, type_id[4]}
 *   Name table: null-terminated ASCII strings
 *   File data: raw file bytes at their respective offsets
 */
#pragma once

#include "engine.h"

/* Maximum number of files per archive (raised if needed) */
#define LAB_MAX_FILES 4096

typedef struct {
    char     name[256];    /* Filename (lowercase) */
    u32      offset;       /* Data offset in archive */
    u32      size;         /* Data size in bytes */
    char     type[5];      /* 4CC type id + null terminator */
} LabEntry;

typedef struct {
    u8      *data;         /* Full archive in memory */
    u64      data_size;    /* Archive size in bytes */
    LabEntry entries[LAB_MAX_FILES];
    u32      entry_count;
    char     path[512];
} LabArchive;

/* Open a LAB archive, load it fully into memory. Returns false on error. */
bool lab_open(LabArchive *lab, const char *path);

/* Close and free an archive. */
void lab_close(LabArchive *lab);

/* Find a file by name (case-insensitive). Returns NULL if not found. */
const LabEntry *lab_find(const LabArchive *lab, const char *name);

/* Get a pointer to file data and its size. Returns NULL if not found. */
const u8 *lab_get(const LabArchive *lab, const char *name, u32 *out_size);

/* Extract a file to memory (caller must free). Returns NULL on failure. */
u8 *lab_extract(const LabArchive *lab, const char *name, u32 *out_size);

/* List all entries to stdout (for debugging). */
void lab_list(const LabArchive *lab);
