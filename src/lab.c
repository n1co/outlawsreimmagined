/*
 * lab.c - LucasArts LAB archive reader implementation
 */
#include "lab.h"
#include <strings.h>
#include <ctype.h>

#pragma pack(push, 1)
typedef struct {
    u8  id[4];          /* "LABN" */
    u32 unknown;        /* 0x10000 for Outlaws */
    u32 file_count;
    u32 name_list_len;
} LabHeader;

typedef struct {
    u32 name_offset;
    u32 data_offset;
    u32 size_bytes;
    u8  type_id[4];
} LabFileEntry;
#pragma pack(pop)

bool lab_open(LabArchive *lab, const char *path) {
    memset(lab, 0, sizeof(*lab));
    snprintf(lab->path, sizeof(lab->path), "%s", path);

    FILE *f = fopen(path, "rb");
    if (!f) { OL_ERR("Cannot open LAB: %s\n", path); return false; }

    fseek(f, 0, SEEK_END);
    lab->data_size = (u64)ftell(f);
    rewind(f);

    if (lab->data_size < sizeof(LabHeader)) {
        OL_ERR("LAB file too small: %s\n", path);
        fclose(f); return false;
    }

    lab->data = malloc(lab->data_size);
    if (!lab->data) { OL_ERR("OOM reading LAB\n"); fclose(f); return false; }

    if (fread(lab->data, 1, lab->data_size, f) != lab->data_size) {
        OL_ERR("Read error on LAB: %s\n", path);
        free(lab->data); fclose(f); return false;
    }
    fclose(f);

    const LabHeader *hdr = (const LabHeader *)lab->data;
    if (memcmp(hdr->id, "LABN", 4) != 0) {
        OL_ERR("Bad LAB magic in: %s\n", path);
        free(lab->data); return false;
    }

    u32 file_count = hdr->file_count;
    if (file_count > LAB_MAX_FILES) {
        OL_WARN("LAB has %u files, clamping to %d\n", file_count, LAB_MAX_FILES);
        file_count = LAB_MAX_FILES;
    }

    const LabFileEntry *entries = (const LabFileEntry *)(hdr + 1);
    const char *names = (const char *)(entries + hdr->file_count);

    for (u32 i = 0; i < file_count; i++) {
        const LabFileEntry *e = &entries[i];

        /* Validate offsets */
        if (e->name_offset >= hdr->name_list_len) continue;
        if (e->data_offset >= lab->data_size) continue;
        if ((u64)e->data_offset + e->size_bytes > lab->data_size) continue;

        LabEntry *le = &lab->entries[lab->entry_count++];
        const char *raw_name = names + e->name_offset;
        snprintf(le->name, sizeof(le->name), "%s", raw_name);
        /* Store lowercase for case-insensitive lookup */
        for (char *p = le->name; *p; p++) *p = tolower((unsigned char)*p);

        le->offset = e->data_offset;
        le->size   = e->size_bytes;
        le->type[0] = e->type_id[0] ? (char)e->type_id[0] : '-';
        le->type[1] = e->type_id[1] ? (char)e->type_id[1] : '-';
        le->type[2] = e->type_id[2] ? (char)e->type_id[2] : '-';
        le->type[3] = e->type_id[3] ? (char)e->type_id[3] : '-';
        le->type[4] = '\0';
    }

    OL_LOG("Opened LAB '%s': %u files\n", path, lab->entry_count);
    return true;
}

void lab_close(LabArchive *lab) {
    if (lab->data) { free(lab->data); lab->data = NULL; }
    lab->entry_count = 0;
}

const LabEntry *lab_find(const LabArchive *lab, const char *name) {
    /* Build lowercase query */
    char lower[256];
    snprintf(lower, sizeof(lower), "%s", name);
    for (char *p = lower; *p; p++) *p = tolower((unsigned char)*p);

    for (u32 i = 0; i < lab->entry_count; i++) {
        if (strcmp(lab->entries[i].name, lower) == 0)
            return &lab->entries[i];
    }
    return NULL;
}

const u8 *lab_get(const LabArchive *lab, const char *name, u32 *out_size) {
    const LabEntry *e = lab_find(lab, name);
    if (!e) return NULL;
    if (out_size) *out_size = e->size;
    return lab->data + e->offset;
}

u8 *lab_extract(const LabArchive *lab, const char *name, u32 *out_size) {
    u32 size = 0;
    const u8 *src = lab_get(lab, name, &size);
    if (!src) return NULL;
    u8 *buf = malloc(size + 1);  /* +1 for text files needing null terminator */
    if (!buf) return NULL;
    memcpy(buf, src, size);
    buf[size] = '\0';
    if (out_size) *out_size = size;
    return buf;
}

void lab_list(const LabArchive *lab) {
    OL_LOG("LAB '%s' (%u entries):\n", lab->path, lab->entry_count);
    for (u32 i = 0; i < lab->entry_count; i++) {
        const LabEntry *e = &lab->entries[i];
        OL_LOG("  [%s] %8u  %s\n", e->type, e->size, e->name);
    }
}
