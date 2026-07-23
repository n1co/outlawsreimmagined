/*
 * lvb.c - Binary .lvb level loader (see lvb.h).
 */
#include "lvb.h"
#include <string.h>
#include <stdlib.h>

/* ---- Chunk stream cursor -------------------------------------------------- */
typedef struct { const u8 *d; u32 size, pos; } Lvb;

static u32 rd_u32(Lvb *b) {
    if (b->pos + 4 > b->size) { b->pos = b->size; return 0; }
    u32 v = (u32)b->d[b->pos] | ((u32)b->d[b->pos+1] << 8) |
            ((u32)b->d[b->pos+2] << 16) | ((u32)b->d[b->pos+3] << 24);
    b->pos += 4;
    return v;
}
static f32 rd_f32(Lvb *b) { u32 v = rd_u32(b); f32 f; memcpy(&f, &v, 4); return f; }
static i32 rd_i32(Lvb *b) { return (i32)rd_u32(b); }

/* Read a null-terminated string field (advances past the terminator). */
static void rd_str(Lvb *b, char *out, u32 cap) {
    u32 i = 0;
    while (b->pos < b->size) {
        u8 c = b->d[b->pos++];
        if (c == 0) break;
        if (out && i + 1 < cap) out[i++] = (char)c;
    }
    if (out && cap) out[i] = '\0';
}
static void skip_str(Lvb *b) { rd_str(b, NULL, 0); }

/* Peek the next chunk's tag (u32) without consuming; -1 at EOF. */
static i32 peek_tag(const Lvb *b) {
    if (b->pos + 5 > b->size) return -1;   /* need tag(4) + count(1) */
    return (i32)((u32)b->d[b->pos] | ((u32)b->d[b->pos+1] << 8) |
                 ((u32)b->d[b->pos+2] << 16) | ((u32)b->d[b->pos+3] << 24));
}

/* If the next chunk is `tag`, consume tag+count and return field_count; else -1
 * (no advance) — matches the engine's Tokenize_GetToken(parser, tag). */
#include <stdio.h>
static int g_lvb_dbg = 0;
static int want(Lvb *b, i32 tag) {
    i32 pk = peek_tag(b);
    if (g_lvb_dbg) fprintf(stderr, "  want(%d) @%u peek=%d\n", tag, b->pos, pk);
    if (pk != tag) return -1;
    b->pos += 4;                 /* tag */
    int count = b->d[b->pos++];  /* field count */
    return count;
}

/* Consume N 4-byte fields (numeric). */
static void skip_num(Lvb *b, int n) { for (int i = 0; i < n; i++) (void)rd_u32(b); }

bool lvb_is_binary(const u8 *data, u32 size) {
    if (!data || size < 8) return false;
    u32 lead = (u32)data[0] | (data[1]<<8) | (data[2]<<16) | ((u32)data[3]<<24);
    if (lead != 0) return false;
    /* second u32 is the ".LVT" magic (bytes '.','L','V','T'). */
    return data[4] == '.' && data[5] == 'L' && data[6] == 'V' && data[7] == 'T';
}

bool obb_is_binary(const u8 *data, u32 size) {
    if (!data || size < 8) return false;
    u32 lead = (u32)data[0] | (data[1]<<8) | (data[2]<<16) | ((u32)data[3]<<24);
    return lead == 0 && data[4]=='.' && data[5]=='O' && data[6]=='B' && data[7]=='T';
}

bool obb_parse(ObtTable *table, const u8 *data, u32 size) {
    if (!obb_is_binary(data, size)) return false;
    memset(table, 0, sizeof(*table));
    Lvb b = { data, size, 8 };
    g_lvb_dbg = getenv("OL_LVBDEBUG") ? 1 : 0;

    if (want(&b, 0) < 0) return false;              /* version */
    skip_num(&b, 2);
    { int c = want(&b, 1); if (c >= 1) { rd_str(&b, table->level_name, sizeof(table->level_name));
        for (int k=1;k<c;k++) skip_str(&b); } }     /* LEVELNAME */
    int nobj = 0;
    if (want(&b, 2) >= 0) nobj = rd_i32(&b);        /* object count */
    if (nobj <= 0) return true;                     /* empty is valid */
    if (nobj > OBT_MAX_OBJECTS) nobj = OBT_MAX_OBJECTS;
    table->objects = (ObtObject *)calloc((u32)nobj, sizeof(ObtObject));
    if (!table->objects) return false;

    for (int i = 0; i < nobj; i++) {
        int c = want(&b, 3);                        /* OBJECT: NAME %s ID %x SECTOR %x XYZ PYR FLAGS %u %u */
        if (c < 1) break;
        ObtObject *o = &table->objects[table->object_count];
        rd_str(&b, o->type, OBT_NAME_LEN);
        /* remaining 10 numeric fields (count is 11 when full). */
        int rem = c - 1;
        o->id        = rem-- > 0 ? rd_u32(&b) : 0;
        o->sector_id = rem-- > 0 ? rd_u32(&b) : 0;
        o->pos.x     = rem-- > 0 ? rd_f32(&b) : 0;
        o->pos.y     = rem-- > 0 ? rd_f32(&b) : 0;
        o->pos.z     = rem-- > 0 ? rd_f32(&b) : 0;
        o->pitch     = rem-- > 0 ? rd_f32(&b) : 0;
        o->yaw       = rem-- > 0 ? rd_f32(&b) : 0;
        o->roll      = rem-- > 0 ? rd_f32(&b) : 0;
        o->flags[0]  = rem-- > 0 ? rd_u32(&b) : 0;
        o->flags[1]  = rem-- > 0 ? rd_u32(&b) : 0;
        while (rem-- > 0) (void)rd_u32(&b);         /* skip any extra */
        table->object_count++;
        if (b.pos >= b.size) break;
    }
    return true;
}

bool lvb_parse(LvtLevel *level, const u8 *data, u32 size) {
    if (!lvb_is_binary(data, size)) return false;
    memset(level, 0, sizeof(*level));
    level->parallax_x = 1024.0f; level->parallax_y = 1024.0f;

    Lvb b = { data, size, 8 };   /* skip leading 0 + ".LVT" magic */
    char tmp[256];
    g_lvb_dbg = getenv("OL_LVBDEBUG") ? 1 : 0;

    /* ---- Header (level_ParseLVTHeader) ---- */
    if (want(&b, 0) < 0) return false;           /* VERSION %d %d */
    skip_num(&b, 2);
    if (want(&b, 1) >= 0) rd_str(&b, level->name, sizeof(level->name)); /* LEVELNAME %s */
    if (want(&b, 2) >= 0) skip_num(&b, 2);       /* optional acad bounds */
    int npal = 0;
    if (want(&b, 3) >= 0) npal = rd_i32(&b);     /* PALETTES %d */
    for (int i = 0; i < npal; i++) {             /* PALETTE: %s */
        if (want(&b, 4) < 0) break;
        rd_str(&b, tmp, sizeof(tmp));
        if (i == 0 && !level->palette_name[0]) {
            /* strip any extension */
            char *dot = strrchr(tmp, '.'); if (dot) *dot = '\0';
            snprintf(level->palette_name, LVT_MAX_NAME, "%s", tmp);
        }
    }
    int ncmap = 0;
    if (want(&b, 5) >= 0) ncmap = rd_i32(&b);    /* CMAPS %d */
    for (int i = 0; i < ncmap; i++) { if (want(&b, 6) < 0) break; skip_str(&b); } /* CMAP: %s */
    if (want(&b, 7) >= 0) rd_str(&b, level->music_file, sizeof(level->music_file)); /* MUSIC %s */
    if (want(&b, 8) >= 0) skip_num(&b, 2);       /* bounds / acad id */
    if (want(&b, 9) >= 0) skip_num(&b, 4);       /* LIGHT SOURCE %f x4 */
    int nshade = 0;
    if (want(&b, 10) >= 0) nshade = rd_i32(&b);  /* SHADES %d */
    for (int i = 0; i < nshade; i++) {           /* SHADE: %d %d %d %d %d %c */
        if (want(&b, 11) < 0) break;
        skip_num(&b, 5); skip_str(&b);           /* 5 ints + 1 char-string */
    }

    /* ---- Textures (Level_Load) ---- */
    int ntex = 0;
    if (want(&b, 0xc) >= 0) ntex = rd_i32(&b);   /* TEXTURES %d */
    for (int i = 0; i < ntex; i++) {             /* TEXTURE: %s */
        if (want(&b, 0xd) < 0) break;
        rd_str(&b, tmp, sizeof(tmp));
        if (level->texture_count < LVT_MAX_TEXTURES)
            snprintf(level->textures[level->texture_count++], LVT_MAX_NAME, "%s", tmp);
    }

    /* ---- Sectors (level_LoadSectors) ---- */
    if (want(&b, 0xe) < 0) return false;         /* NUMSECTORS marker */
    int nsec = rd_i32(&b);
    if (nsec <= 0 || nsec > LVT_MAX_SECTORS) return false;
    level->sectors = (LvtSector *)calloc((u32)nsec, sizeof(LvtSector));
    if (!level->sectors) return false;

    for (int s = 0; s < nsec; s++) {
        LvtSector *sec = &level->sectors[s];
        sec->ambient = 31; sec->friction = 1.0f; sec->gravity = -60.0f;
        sec->elasticity = 0.3f; sec->floor_tex = -1; sec->ceil_tex = -1;

        int c = want(&b, 0xf);                    /* SECTOR %x */
        if (c < 0) { level->sector_count = (u32)s; break; }
        sec->id = rd_u32(&b);

        { int c = want(&b, 0x10);                                     /* NAME %s (count may be 0) */
          if (c >= 1) { rd_str(&b, sec->name, LVT_MAX_NAME); for (int k=1;k<c;k++) skip_str(&b); } }
        if (want(&b, 0x11) >= 0) sec->ambient = rd_i32(&b);            /* AMBIENT %d */
        if (want(&b, 0x12) >= 0) (void)rd_i32(&b);                     /* PALETTE %d */
        if (want(&b, 0x13) >= 0) (void)rd_i32(&b);                     /* CMAP %d */
        if (want(&b, 0x14) >= 0) sec->friction = rd_f32(&b);           /* FRICTION %f */
        if (want(&b, 0x15) >= 0) sec->gravity = rd_f32(&b);            /* GRAVITY %f */
        if (want(&b, 0x16) >= 0) sec->elasticity = rd_f32(&b);         /* ELASTICITY %f */
        if (want(&b, 0x17) >= 0) skip_num(&b, 3);                      /* VELOCITY %f x3 */
        if (want(&b, 0x18) >= 0) (void)rd_i32(&b);                     /* VADJOIN %d */
        { int c = want(&b, 0x19); for (int k=0;k<c;k++) skip_str(&b); } /* FLOOR SOUND %s (count may be 0) */

        if (want(&b, 0x1a) >= 0) {                                     /* FLOOR Y %f %d %f %f %f */
            sec->floor_y = rd_f32(&b); sec->floor_tex = rd_i32(&b);
            sec->floor_offset.u = rd_f32(&b); sec->floor_offset.v = rd_f32(&b);
            sec->floor_rot_deg = rd_f32(&b);
        }
        if (want(&b, 0x1b) >= 0) {                                     /* CEILING Y ... */
            sec->ceil_y = rd_f32(&b); sec->ceil_tex = rd_i32(&b);
            sec->ceil_offset.u = rd_f32(&b); sec->ceil_offset.v = rd_f32(&b);
            sec->ceil_rot_deg = rd_f32(&b);
        }
        if (want(&b, 0x1c) >= 0) skip_num(&b, 4);                      /* F_OVERLAY %d %f %f %f */
        if (want(&b, 0x1d) >= 0) skip_num(&b, 4);                      /* C_OVERLAY */

        int noff = 0;
        if (want(&b, 0x1e) >= 0) noff = rd_i32(&b);                    /* FLOOR OFFSETS %d */
        for (int i = 0; i < noff; i++) { if (want(&b, 0x1f) < 0) break; skip_num(&b, 6); }

        if (want(&b, 0x20) >= 0) { sec->flags = rd_u32(&b); sec->flags2 = rd_u32(&b); } /* FLAGS %u %u */
        if (want(&b, 0x21) >= 0) {                                     /* SLOPEDFLOOR %d %d %f */
            sec->has_slope_floor = true;
            sec->slope_floor_wall = rd_i32(&b); (void)rd_i32(&b);
            sec->slope_floor_angle = (i32)rd_f32(&b);
        }
        if (want(&b, 0x22) >= 0) {                                     /* SLOPEDCEILING */
            sec->has_slope_ceil = true;
            sec->slope_ceil_wall = rd_i32(&b); (void)rd_i32(&b);
            sec->slope_ceil_angle = (i32)rd_f32(&b);
        }
        if (want(&b, 0x23) >= 0) sec->layer = (i32)rd_f32(&b);         /* LAYER %f */

        int nvert = 0;
        if (want(&b, 0x24) >= 0) nvert = rd_i32(&b);                   /* VERTICES %d */
        if (nvert > LVT_MAX_VERTICES) nvert = LVT_MAX_VERTICES;
        for (int i = 0; i < nvert; i++) {                             /* X: %f Z: %f */
            if (want(&b, 0x25) < 0) break;
            f32 x = rd_f32(&b), z = rd_f32(&b);
            sec->vertices[i].x = x; sec->vertices[i].y = z;
        }
        sec->vertex_count = (u32)nvert;

        int nwall = 0;
        if (want(&b, 0x26) >= 0) nwall = rd_i32(&b);                   /* WALLS %d */
        if (nwall > LVT_MAX_WALLS) nwall = LVT_MAX_WALLS;
        for (int i = 0; i < nwall; i++) {                             /* WALL: ... (22 fields) */
            if (want(&b, 0x27) < 0) break;
            LvtWall *w = &sec->walls[i];
            w->id      = rd_u32(&b);
            w->v1      = rd_i32(&b);
            w->v2      = rd_i32(&b);
            w->mid.tex_id = rd_i32(&b); w->mid.offset.u = rd_f32(&b); w->mid.offset.v = rd_f32(&b);
            w->top.tex_id = rd_i32(&b); w->top.offset.u = rd_f32(&b); w->top.offset.v = rd_f32(&b);
            w->bot.tex_id = rd_i32(&b); w->bot.offset.u = rd_f32(&b); w->bot.offset.v = rd_f32(&b);
            w->overlay.tex_id = rd_i32(&b); w->overlay.offset.u = rd_f32(&b); w->overlay.offset.v = rd_f32(&b);
            w->adjoin  = rd_i32(&b);
            w->mirror  = rd_i32(&b);
            w->dadjoin = rd_i32(&b);
            w->dmirror = rd_i32(&b);
            w->flags   = rd_u32(&b);
            w->flags2  = rd_u32(&b);
            w->light   = rd_i32(&b);
        }
        sec->wall_count = (u32)nwall;
        level->sector_count = (u32)(s + 1);

        if (b.pos >= b.size && s + 1 < nsec) break;   /* truncated */
    }

    return level->sector_count > 0;
}
