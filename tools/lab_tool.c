/*
 * lab_tool.c - Standalone LAB archive tool for Outlaws
 *
 * Usage:
 *   lab_tool list <archive.lab>
 *   lab_tool extract <archive.lab> <output_dir> [pattern]
 *   lab_tool get <archive.lab> <filename> <output_file>
 *
 * This tool allows you to inspect and extract files from LucasArts
 * LAB archives without needing to run the full game engine.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>

typedef uint8_t  u8;
typedef uint32_t u32;
typedef uint64_t u64;

/* --- LAB format structures ------------------------------------------ */
#pragma pack(push, 1)
typedef struct { u8 id[4]; u32 unknown; u32 file_count; u32 name_list_len; } LabHeader;
typedef struct { u32 name_offset; u32 data_offset; u32 size_bytes; u8 type_id[4]; } LabFileEntry;
#pragma pack(pop)

typedef struct { char name[256]; u32 offset; u32 size; char type[5]; } Entry;
typedef struct { u8 *data; u64 data_size; Entry *entries; u32 entry_count; } Lab;

static int lab_open(Lab *lab, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 0; }
    fseek(f, 0, SEEK_END); lab->data_size = (u64)ftell(f); rewind(f);
    lab->data = malloc(lab->data_size);
    fread(lab->data, 1, lab->data_size, f); fclose(f);
    const LabHeader *hdr = (const LabHeader *)lab->data;
    if (memcmp(hdr->id, "LABN", 4) != 0) { fprintf(stderr, "Not a LAB file\n"); return 0; }
    lab->entries = calloc(hdr->file_count, sizeof(Entry));
    lab->entry_count = 0;
    const LabFileEntry *raw = (const LabFileEntry *)(hdr + 1);
    const char *names = (const char *)(raw + hdr->file_count);
    for (u32 i = 0; i < hdr->file_count; i++) {
        if (raw[i].name_offset >= hdr->name_list_len) continue;
        if (raw[i].data_offset >= lab->data_size) continue;
        Entry *e = &lab->entries[lab->entry_count++];
        strncpy(e->name, names + raw[i].name_offset, 255);
        e->offset = raw[i].data_offset;
        e->size   = raw[i].size_bytes;
        e->type[0] = raw[i].type_id[0] ? (char)raw[i].type_id[0] : '-';
        e->type[1] = raw[i].type_id[1] ? (char)raw[i].type_id[1] : '-';
        e->type[2] = raw[i].type_id[2] ? (char)raw[i].type_id[2] : '-';
        e->type[3] = raw[i].type_id[3] ? (char)raw[i].type_id[3] : '-';
        e->type[4] = '\0';
    }
    return 1;
}

static void lab_close(Lab *lab) { free(lab->data); free(lab->entries); }

static int make_dirs(const char *path) {
    char tmp[4096]; strncpy(tmp, path, sizeof(tmp)-1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

static int name_matches(const char *name, const char *pattern) {
    if (!pattern || pattern[0] == '\0') return 1;
    /* Simple glob: case-insensitive extension match */
    if (pattern[0] == '*' && pattern[1] == '.') {
        const char *ext = strrchr(name, '.');
        if (!ext) return 0;
        return strcasecmp(ext + 1, pattern + 2) == 0;
    }
    return strcasecmp(name, pattern) == 0;
}

static void cmd_list(const char *lab_path) {
    Lab lab = {0};
    if (!lab_open(&lab, lab_path)) return;
    printf("LAB: %s (%u files)\n", lab_path, lab.entry_count);
    printf("%-5s  %-10s  %s\n", "TYPE", "SIZE", "FILENAME");
    printf("-----  ----------  --------\n");

    /* Count by extension */
    u64 total = 0;
    for (u32 i = 0; i < lab.entry_count; i++) {
        printf("[%s]  %10u  %s\n", lab.entries[i].type,
               lab.entries[i].size, lab.entries[i].name);
        total += lab.entries[i].size;
    }
    printf("\nTotal: %llu bytes in %u files\n", (unsigned long long)total, lab.entry_count);
    lab_close(&lab);
}

static void cmd_extract(const char *lab_path, const char *out_dir, const char *pattern) {
    Lab lab = {0};
    if (!lab_open(&lab, lab_path)) return;

    /* Ensure output dir exists */
    char dir_buf[4096];
    snprintf(dir_buf, sizeof(dir_buf), "%s/.", out_dir);
    make_dirs(dir_buf);

    int extracted = 0, skipped = 0;
    for (u32 i = 0; i < lab.entry_count; i++) {
        if (!name_matches(lab.entries[i].name, pattern)) { skipped++; continue; }

        char out_path[4096];
        snprintf(out_path, sizeof(out_path), "%s/%s", out_dir, lab.entries[i].name);

        FILE *out = fopen(out_path, "wb");
        if (!out) { fprintf(stderr, "Cannot write: %s\n", out_path); continue; }
        fwrite(lab.data + lab.entries[i].offset, 1, lab.entries[i].size, out);
        fclose(out);
        printf("Extracted: %s (%u bytes)\n", lab.entries[i].name, lab.entries[i].size);
        extracted++;
    }
    printf("\nExtracted %d files", extracted);
    if (skipped) printf(" (%d skipped by filter)", skipped);
    printf("\n");
    lab_close(&lab);
}

static void cmd_get(const char *lab_path, const char *filename, const char *out_path) {
    Lab lab = {0};
    if (!lab_open(&lab, lab_path)) return;
    for (u32 i = 0; i < lab.entry_count; i++) {
        if (strcasecmp(lab.entries[i].name, filename) == 0) {
            FILE *out = fopen(out_path, "wb");
            if (!out) { perror(out_path); lab_close(&lab); return; }
            fwrite(lab.data + lab.entries[i].offset, 1, lab.entries[i].size, out);
            fclose(out);
            printf("Written: %s (%u bytes) → %s\n", filename, lab.entries[i].size, out_path);
            lab_close(&lab);
            return;
        }
    }
    fprintf(stderr, "File not found in archive: %s\n", filename);
    lab_close(&lab);
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s list <archive.lab>\n", prog);
    fprintf(stderr, "  %s extract <archive.lab> <output_dir> [pattern]\n", prog);
    fprintf(stderr, "    pattern: '*.pcx' for all PCX, 'file.wav' for exact match\n");
    fprintf(stderr, "  %s get <archive.lab> <filename> <output_file>\n", prog);
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(argv[0]); return 1; }
    if (strcmp(argv[1], "list") == 0 && argc >= 3) {
        cmd_list(argv[2]);
    } else if (strcmp(argv[1], "extract") == 0 && argc >= 4) {
        cmd_extract(argv[2], argv[3], argc >= 5 ? argv[4] : NULL);
    } else if (strcmp(argv[1], "get") == 0 && argc >= 5) {
        cmd_get(argv[2], argv[3], argv[4]);
    } else {
        usage(argv[0]); return 1;
    }
    return 0;
}
