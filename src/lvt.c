/*
 * lvt.c - Outlaws LVT level format parser
 *
 * The LVT format is a text-based sector/wall level description.
 * Each sector is a convex polygon in the XZ plane (Y is up).
 *
 * Example sector block:
 *   SECTOR 9CB20008
 *     NAME  open_area
 *     AMBIENT  13
 *     FLOOR Y  0.00  11  0.00  0.00 0
 *     CEILING Y  160.00  2  0.00  0.00 0
 *     VERTICES 8
 *       X: -107.50  Z: 936.00  # 0
 *       ...
 *     WALLS 8
 *       WALL: id V1: i  V2: j  MID: t u v  TOP: t u v  BOT: t u v
 *             OVERLAY: t u v  ADJOIN: s  MIRROR: w  FLAGS: f  LIGHT: l
 */
#include "lvt.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>

/* -------------------------------------------------------------------------
 * Text parser utilities
 * ---------------------------------------------------------------------- */

typedef struct {
    const char *p;    /* Current position */
    const char *end;  /* End of buffer */
    int line;         /* Current line number */
} Parser;

static void parser_init(Parser *p, const char *text, u32 len) {
    p->p    = text;
    p->end  = text + len;
    p->line = 1;
}

/* Skip to end of line */
static void skip_line(Parser *p) {
    while (p->p < p->end && *p->p != '\n') p->p++;
    if (p->p < p->end) { p->p++; p->line++; }
}

/* Skip blank lines and comments (# ...) */
static void skip_whitespace(Parser *p) {
    while (p->p < p->end) {
        if (*p->p == '#') { skip_line(p); continue; }
        if (*p->p == '\n') { p->p++; p->line++; continue; }
        if (*p->p == '\r') { p->p++; continue; }
        if (*p->p == ' ' || *p->p == '\t') { p->p++; continue; }
        break;
    }
}

/* Read next word (token), return length. Returns 0 at end. */
static int read_token(Parser *p, char *buf, int buf_size) {
    skip_whitespace(p);
    if (p->p >= p->end) return 0;

    /* Comment or newline: skip */
    if (*p->p == '#') { skip_line(p); return read_token(p, buf, buf_size); }

    int len = 0;
    while (p->p < p->end && len < buf_size - 1) {
        char c = *p->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') break;
        if (c == '#') { skip_line(p); break; }
        buf[len++] = c;
        p->p++;
    }
    buf[len] = '\0';
    return len;
}

/* Read next token and return as integer */
static i32 read_int(Parser *p) {
    char tok[64]; read_token(p, tok, sizeof(tok));
    return (i32)strtol(tok, NULL, 10);
}

/* Read next token and return as float */
static f32 read_float(Parser *p) {
    char tok[64]; read_token(p, tok, sizeof(tok));
    return strtof(tok, NULL);
}

/* Read a hex ID like "9CB20008" */
static u32 read_hex_id(Parser *p) {
    char tok[64]; read_token(p, tok, sizeof(tok));
    return (u32)strtoul(tok, NULL, 16);
}

/* -------------------------------------------------------------------------
 * Parse wall texture slot: tex_id  u_off  v_off
 * ---------------------------------------------------------------------- */
static WallTex parse_wall_tex(Parser *p) {
    WallTex wt;
    wt.tex_id = read_int(p);
    wt.offset.u = read_float(p);
    wt.offset.v = read_float(p);
    return wt;
}

/* -------------------------------------------------------------------------
 * Parse a WALL line
 * Format: WALL: id V1: i V2: j MID: t u v TOP: t u v BOT: t u v
 *         OVERLAY: t u v ADJOIN: s MIRROR: w DADJOIN: da DMIRROR: dm
 *         FLAGS: f1 f2 LIGHT: l
 * ---------------------------------------------------------------------- */
static void parse_wall(Parser *p, LvtWall *wall) {
    char tok[128];

    /* WALL: id */
    read_token(p, tok, sizeof(tok)); /* "WALL:" */
    wall->id = read_hex_id(p);

    /* V1: i */
    read_token(p, tok, sizeof(tok)); /* "V1:" */
    wall->v1 = read_int(p);

    /* V2: j */
    read_token(p, tok, sizeof(tok)); /* "V2:" */
    wall->v2 = read_int(p);

    /* MID: t u v */
    read_token(p, tok, sizeof(tok)); /* "MID:" */
    wall->mid = parse_wall_tex(p);

    /* TOP: t u v */
    read_token(p, tok, sizeof(tok)); /* "TOP:" */
    wall->top = parse_wall_tex(p);

    /* BOT: t u v */
    read_token(p, tok, sizeof(tok)); /* "BOT:" */
    wall->bot = parse_wall_tex(p);

    /* OVERLAY: t u v */
    read_token(p, tok, sizeof(tok)); /* "OVERLAY:" */
    wall->overlay = parse_wall_tex(p);

    /* ADJOIN: sector_id */
    read_token(p, tok, sizeof(tok)); /* "ADJOIN:" */
    wall->adjoin = read_int(p);      /* -1 = solid, else raw sector index (resolved later) */

    /* MIRROR: wall_idx */
    read_token(p, tok, sizeof(tok)); /* "MIRROR:" */
    wall->mirror = read_int(p);

    /* DADJOIN: dual adjoin (3-sector portal for ADJOIN_MID walls) */
    read_token(p, tok, sizeof(tok)); /* "DADJOIN:" */
    wall->dadjoin = read_int(p);

    /* DMIRROR: dual mirror wall index */
    read_token(p, tok, sizeof(tok)); /* "DMIRROR:" */
    wall->dmirror = read_int(p);

    /* FLAGS: f1 f2 */
    read_token(p, tok, sizeof(tok)); /* "FLAGS:" */
    wall->flags = (u32)read_int(p);
    wall->flags2 = (u32)read_int(p);

    /* LIGHT: l */
    read_token(p, tok, sizeof(tok)); /* "LIGHT:" */
    wall->light = read_int(p);
}

/* -------------------------------------------------------------------------
 * Parse a SECTOR block
 * ---------------------------------------------------------------------- */
static bool parse_sector(Parser *p, LvtSector *sec) {
    char tok[128];

    /* Read sector ID (hex) */
    sec->id = read_hex_id(p);
    sec->ambient = 31;
    sec->friction = 1.0f;
    sec->gravity = -60.0f;
    sec->elasticity = 0.3f;
    sec->floor_tex = -1;
    sec->ceil_tex  = -1;

    /* Read properties until we hit the next SECTOR or end */
    while (p->p < p->end) {
        skip_whitespace(p);
        if (p->p >= p->end) break;

        /* Peek at next token */
        const char *save = p->p;
        read_token(p, tok, sizeof(tok));

        if (strcasecmp(tok, "SECTOR") == 0) {
            p->p = save; /* Put it back */
            break;
        }

        if (strcasecmp(tok, "NAME") == 0) {
            read_token(p, sec->name, LVT_MAX_NAME);
            continue;
        }
        if (strcasecmp(tok, "AMBIENT") == 0) {
            sec->ambient = read_int(p);
            continue;
        }
        if (strcasecmp(tok, "PALETTE") == 0 || strcasecmp(tok, "CMAP") == 0) {
            read_int(p); /* skip */
            continue;
        }
        if (strcasecmp(tok, "FRICTION") == 0) {
            sec->friction = read_float(p);
            continue;
        }
        if (strcasecmp(tok, "GRAVITY") == 0) {
            sec->gravity = read_float(p);
            continue;
        }
        if (strcasecmp(tok, "ELASTICITY") == 0) {
            sec->elasticity = read_float(p);   /* bounce restitution (0.3) */
            continue;
        }
        if (strcasecmp(tok, "VELOCITY") == 0) {
            read_float(p); read_float(p); read_float(p);
            continue;
        }
        if (strcasecmp(tok, "VADJOIN") == 0) {
            read_int(p);
            continue;
        }
        if (strcasecmp(tok, "SLOPEDFLOOR") == 0) {
            /* SLOPEDFLOOR <anchorSector(==self)> <pivotWall> <angle>.
             * The FIRST value is the anchor sector id (its own index); the
             * SECOND is the pivot WALL index within the sector — reading the
             * first as the wall left it out of range → slope silently ignored
             * (flat floor), which broke ramps/stairs in render AND collision. */
            sec->has_slope_floor = true;
            read_int(p);                            /* anchor sector id (self) */
            sec->slope_floor_wall  = read_int(p);   /* pivot wall index */
            sec->slope_floor_angle = read_int(p);
            continue;
        }
        if (strcasecmp(tok, "SLOPEDCEILING") == 0) {
            sec->has_slope_ceil = true;
            read_int(p);                            /* anchor sector id (self) */
            sec->slope_ceil_wall  = read_int(p);    /* pivot wall index */
            sec->slope_ceil_angle = read_int(p);
            continue;
        }
        if (strcasecmp(tok, "FLOOR") == 0) {
            /* FLOOR SOUND <sound>  or  FLOOR Y <y> <tex> <u> <v> <flags> */
            read_token(p, tok, sizeof(tok));
            if (strcasecmp(tok, "Y") == 0) {
                sec->floor_y       = read_float(p);
                sec->floor_tex     = read_int(p);
                sec->floor_offset.u = read_float(p);   /* offX (Ghidra +0x5c) */
                sec->floor_offset.v = read_float(p);   /* offZ (Ghidra +0x6c) */
                sec->floor_rot_deg  = read_float(p);   /* rotation (→ +0x7c) */
            } else if (strcasecmp(tok, "SOUND") == 0) {
                read_token(p, tok, sizeof(tok)); /* sound name */
            } else if (strcasecmp(tok, "OFFSETS") == 0) {
                read_int(p);
            }
            continue;
        }
        if (strcasecmp(tok, "CEILING") == 0) {
            /* CEILING Y <y> <tex> <u> <v> <flags> */
            read_token(p, tok, sizeof(tok)); /* "Y" */
            sec->ceil_y       = read_float(p);
            sec->ceil_tex     = read_int(p);
            sec->ceil_offset.u = read_float(p);   /* offX (Ghidra +0x60) */
            sec->ceil_offset.v = read_float(p);   /* offZ (Ghidra +0x70) */
            sec->ceil_rot_deg  = read_float(p);   /* rotation (→ +0x7e) */
            continue;
        }
        if (strcasecmp(tok, "F_OVERLAY") == 0 || strcasecmp(tok, "C_OVERLAY") == 0) {
            read_int(p); read_float(p); read_float(p); read_int(p);
            continue;
        }
        if (strcasecmp(tok, "FLAGS") == 0) {
            sec->flags = (u32)read_int(p);
            read_int(p); /* second flags */
            continue;
        }
        if (strcasecmp(tok, "LAYER") == 0) {
            sec->layer = read_int(p);
            continue;
        }
        if (strcasecmp(tok, "VERTICES") == 0) {
            /* Read vertex count, then X: Z: pairs */
            int count = read_int(p);
            if (count > LVT_MAX_VERTICES) {
                OL_WARN("Sector %08X: too many vertices (%d), clamping\n", sec->id, count);
                count = LVT_MAX_VERTICES;
            }
            sec->vertex_count = 0;
            for (int i = 0; i < count; i++) {
                /* X: val # comment */
                read_token(p, tok, sizeof(tok)); /* "X:" */
                f32 x = read_float(p);
                /* Z: val # comment */
                read_token(p, tok, sizeof(tok)); /* "Z:" */
                f32 z = read_float(p);
                /* Skip "# N" comment */
                skip_whitespace(p);
                if (p->p < p->end && *p->p == '#') skip_line(p);
                if ((u32)i < (u32)LVT_MAX_VERTICES) {
                    sec->vertices[sec->vertex_count].x = x;
                    sec->vertices[sec->vertex_count].y = z;
                    sec->vertex_count++;
                }
            }
            continue;
        }
        if (strcasecmp(tok, "WALLS") == 0) {
            int count = read_int(p);
            if (count > LVT_MAX_WALLS) count = LVT_MAX_WALLS;
            sec->wall_count = 0;
            for (int i = 0; i < count; i++) {
                parse_wall(p, &sec->walls[sec->wall_count++]);
            }
            continue;
        }
        /* Skip any unknown keyword line */
        skip_line(p);
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */
bool lvt_parse(LvtLevel *level, const char *text, u32 text_len) {
    memset(level, 0, sizeof(*level));

    Parser p;
    parser_init(&p, text, text_len);
    char tok[256];

    /* First line: "LVT 1.x" */
    read_token(&p, tok, sizeof(tok));
    if (strcasecmp(tok, "LVT") != 0) {
        OL_ERR("Not an LVT file (got: '%s')\n", tok);
        return false;
    }
    read_token(&p, tok, sizeof(tok)); /* version */

    /* Parse header fields before SECTOR blocks */
    u32 num_sectors = 0;
    bool in_header = true;

    while (in_header && p.p < p.end) {
        skip_whitespace(&p);
        if (p.p >= p.end) break;

        read_token(&p, tok, sizeof(tok));

        if (strcasecmp(tok, "LEVELNAME") == 0) {
            read_token(&p, level->name, sizeof(level->name));
        } else if (strcasecmp(tok, "VERSION") == 0) {
            read_token(&p, tok, sizeof(tok)); /* skip */
        } else if (strcasecmp(tok, "PALETTES") == 0) {
            int n = read_int(&p);
            for (int i = 0; i < n; i++) {
                read_token(&p, tok, sizeof(tok)); /* "PALETTE:" */
                read_token(&p, tok, sizeof(tok)); /* name */
                /* Store first palette name for level-specific color lookup */
                if (i == 0 && !level->palette_name[0]) {
                    strncpy(level->palette_name, tok, sizeof(level->palette_name)-1);
                    level->palette_name[sizeof(level->palette_name)-1] = '\0';
                }
            }
        } else if (strcasecmp(tok, "CMAPS") == 0) {
            int n = read_int(&p);
            for (int i = 0; i < n; i++) {
                read_token(&p, tok, sizeof(tok)); /* "CMAP:" */
                read_token(&p, tok, sizeof(tok)); /* name */
            }
        } else if (strcasecmp(tok, "MUSIC") == 0) {
            read_token(&p, level->music_file, sizeof(level->music_file));
        } else if (strcasecmp(tok, "PARALLAX") == 0) {
            level->parallax_x = read_float(&p);
            level->parallax_y = read_float(&p);
        } else if (strcasecmp(tok, "LIGHT") == 0) {
            read_token(&p, tok, sizeof(tok)); /* "SOURCE" */
            read_float(&p); read_float(&p); read_float(&p); read_float(&p);
        } else if (strcasecmp(tok, "SHADES") == 0) {
            int n = read_int(&p);
            for (int i = 0; i < n; i++) {
                /* SHADE: id r g b ambient type */
                read_token(&p, tok, sizeof(tok)); /* "SHADE:" */
                read_int(&p); read_float(&p); read_float(&p); read_float(&p);
                read_int(&p); read_token(&p, tok, sizeof(tok));
            }
        } else if (strcasecmp(tok, "TEXTURES") == 0) {
            int n = read_int(&p);
            if (n > LVT_MAX_TEXTURES) n = LVT_MAX_TEXTURES;
            level->texture_count = 0;
            for (int i = 0; i < n; i++) {
                read_token(&p, tok, sizeof(tok)); /* "TEXTURE:" */
                if (level->texture_count < LVT_MAX_TEXTURES) {
                    char tname[LVT_MAX_NAME];
                    read_token(&p, tname, LVT_MAX_NAME);
                    /* Skip "# N" comment */
                    skip_whitespace(&p);
                    if (p.p < p.end && *p.p == '#') skip_line(&p);
                    snprintf(level->textures[level->texture_count++],
                             LVT_MAX_NAME, "%s", tname);
                }
            }
        } else if (strcasecmp(tok, "NUMSECTORS") == 0) {
            num_sectors = (u32)read_int(&p);
            in_header = false; /* Next we expect SECTOR blocks */
        } else {
            /* Unknown header token, skip line */
            skip_line(&p);
        }
    }

    /* Allocate sectors */
    if (num_sectors == 0) {
        OL_WARN("LVT: no NUMSECTORS found, will count on the fly\n");
        num_sectors = LVT_MAX_SECTORS;
    }
    if (num_sectors > LVT_MAX_SECTORS) num_sectors = LVT_MAX_SECTORS;
    level->sectors = calloc(num_sectors, sizeof(LvtSector));
    if (!level->sectors) { OL_ERR("OOM allocating sectors\n"); return false; }

    /* Parse sector blocks */
    while (p.p < p.end && level->sector_count < num_sectors) {
        skip_whitespace(&p);
        if (p.p >= p.end) break;

        read_token(&p, tok, sizeof(tok));

        if (strcasecmp(tok, "SECTOR") == 0) {
            LvtSector *sec = &level->sectors[level->sector_count];
            if (parse_sector(&p, sec)) {
                level->sector_count++;
            }
        } else {
            /* Ignore anything between sectors */
            skip_line(&p);
        }
    }

    OL_LOG("LVT '%s': %u sectors, %u textures\n",
           level->name, level->sector_count, level->texture_count);
    return true;
}

void lvt_free(LvtLevel *level) {
    if (level->sectors) { free(level->sectors); level->sectors = NULL; }
    level->sector_count = 0;
}

i32 lvt_find_sector(const LvtLevel *level, u32 id) {
    for (u32 i = 0; i < level->sector_count; i++)
        if (level->sectors[i].id == id) return (i32)i;
    return -1;
}

i32 lvt_find_sector_by_name(const LvtLevel *level, const char *name) {
    if (!name || !name[0]) return -1;
    for (u32 i = 0; i < level->sector_count; i++)
        if (strcasecmp(level->sectors[i].name, name) == 0) return (i32)i;
    return -1;
}

bool lvt_is_default_tex(const LvtLevel *level, i32 tex_id) {
    if (tex_id < 0 || (u32)tex_id >= level->texture_count) return false;
    return strcasecmp(level->textures[tex_id], "DEFAULT.PCX") == 0;
}

/*
 * Height of a sloped floor/ceiling plane at (x,z), pivoting on wall `wall_idx`,
 * with the plane raised by `angle` about that hinge. Implements the exact
 * Outlaws slope math from The Force Engine (TFE editGeometry.cpp
 * slope_calculatePlane / slope_getHeightAtXZ): with the wall vertices v0=idx[0],
 * v1=idx[1], the hinge is at v1, vect1 = v0-v1, and the plane is raised in the
 * +(-vect1.z, vect1.x) direction. Getting this direction right (it is the
 * OPPOSITE of a naive left-hand perpendicular) is what makes ramps climb the
 * correct way and roofs peak OUTWARD instead of caving in. angle is 14-bit fixed.
 */
f32 lvt_slope_height(const LvtSector *s, f32 base, i32 wall_idx,
                     i32 angle_fixed, f32 x, f32 z) {
    if (wall_idx < 0 || wall_idx >= (i32)s->wall_count) return base;
    const LvtWall *pw = &s->walls[wall_idx];
    if (pw->v1 < 0 || pw->v1 >= (i32)s->vertex_count ||
        pw->v2 < 0 || pw->v2 >= (i32)s->vertex_count) return base;
    /* v0 = idx[0] = wall v1 ; v1(hinge/base) = idx[1] = wall v2 */
    f32 x0 = s->vertices[pw->v1].x, z0 = s->vertices[pw->v1].y;
    f32 x1 = s->vertices[pw->v2].x, z1 = s->vertices[pw->v2].y;
    f32 a = x0 - x1, b = z0 - z1;                 /* vect1 = v0 - v1 (XZ) */
    f32 wallLen = sqrtf(a*a + b*b);
    if (wallLen < 1e-6f) return base;
    f32 t = tanf((f32)angle_fixed * (3.14159265f / 16384.0f));
    /* plane normal (normalized so normal.y = 1): (b*t/len, 1, -a*t/len) */
    f32 nX = b * t / wallLen;
    f32 nZ = -a * t / wallLen;
    /* height = dp - x*nX - z*nZ, dp = v1.x*nX + base + v1.z*nZ */
    return base + (x1 - x) * nX + (z1 - z) * nZ;
}

static f32 lvt_slope_at(const LvtSector *s, f32 base, i32 wall_idx,
                        i32 angle_fixed, f32 x, f32 z) {
    return lvt_slope_height(s, base, wall_idx, angle_fixed, x, z);
}

f32 lvt_floor_at(const LvtSector *s, f32 x, f32 z) {
    if (!s->has_slope_floor) return s->floor_y;
    return lvt_slope_at(s, s->floor_y, s->slope_floor_wall, s->slope_floor_angle, x, z);
}

f32 lvt_ceil_at(const LvtSector *s, f32 x, f32 z) {
    if (!s->has_slope_ceil) return s->ceil_y;
    return lvt_slope_at(s, s->ceil_y, s->slope_ceil_wall, s->slope_ceil_angle, x, z);
}
