/*
 * savegame.c - Story campaign save/load (see savegame.h)
 */
#include "savegame.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define SAVE_MAGIC   0x5641534Fu   /* "OSAV" */
#define SAVE_VERSION 1u

static void save_path(int slot, char *out, int cap) {
    snprintf(out, cap, "saves/save%02d.osv", slot);
}

static void ensure_dir(void) {
    mkdir("saves", 0755);   /* ignore EEXIST */
}

bool savegame_write(int slot, const SaveGame *sg) {
    if (slot < 0 || slot >= SAVE_SLOTS) return false;
    ensure_dir();
    char path[64]; save_path(slot, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    SaveGame out = *sg;
    out.magic = SAVE_MAGIC;
    out.version = SAVE_VERSION;
    size_t n = fwrite(&out, sizeof(out), 1, f);
    fclose(f);
    return n == 1;
}

bool savegame_read(int slot, SaveGame *sg) {
    if (slot < 0 || slot >= SAVE_SLOTS) return false;
    char path[64]; save_path(slot, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    SaveGame in;
    size_t n = fread(&in, sizeof(in), 1, f);
    fclose(f);
    if (n != 1 || in.magic != SAVE_MAGIC || in.version != SAVE_VERSION) return false;
    in.level[sizeof(in.level)-1] = '\0';
    in.label[sizeof(in.label)-1] = '\0';
    *sg = in;
    return true;
}

bool savegame_peek(int slot, char *out_label, int cap) {
    SaveGame sg;
    if (!savegame_read(slot, &sg)) return false;
    snprintf(out_label, cap, "%s", sg.label);
    return true;
}
