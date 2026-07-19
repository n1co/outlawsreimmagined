/*
 * world.c - World/level management
 */
#include "world.h"
#include "pcx.h"
#include "obt.h"
#include "wax.h"
#include "itm.h"
#include "inf.h"
#include "collision.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Archives
 * ---------------------------------------------------------------------- */

bool archives_open(Archives *arc, const char *data_dir) {
    memset(arc, 0, sizeof(*arc));
    snprintf(arc->data_dir, sizeof(arc->data_dir), "%s", data_dir);

    /* Remove trailing slash */
    int len = (int)strlen(arc->data_dir);
    if (len > 0 && (arc->data_dir[len-1] == '/' || arc->data_dir[len-1] == '\\'))
        arc->data_dir[len-1] = '\0';

    char path[1024];
    bool ok = true;

    /* Required: outlaws.lab */
    snprintf(path, sizeof(path), "%s/outlaws.lab", arc->data_dir);
    if (!lab_open(&arc->main, path)) {
        OL_WARN("Cannot open outlaws.lab (continuing without)\n"); ok = false;
    }

    /* Required: OLGEO.LAB (try both cases) */
    snprintf(path, sizeof(path), "%s/OLGEO.LAB", arc->data_dir);
    if (!lab_open(&arc->geo, path)) {
        snprintf(path, sizeof(path), "%s/olgeo.lab", arc->data_dir);
        if (!lab_open(&arc->geo, path)) {
            OL_ERR("Cannot open OLGEO.LAB - level geometry unavailable\n"); ok = false;
        }
    }

    snprintf(path, sizeof(path), "%s/oltex.lab", arc->data_dir);
    if (!lab_open(&arc->tex, path))
        OL_WARN("Cannot open oltex.lab (textures unavailable)\n");

    snprintf(path, sizeof(path), "%s/olsfx.lab", arc->data_dir);
    if (!lab_open(&arc->sfx, path))
        OL_WARN("Cannot open olsfx.lab (sound effects unavailable)\n");

    snprintf(path, sizeof(path), "%s/olobj.lab", arc->data_dir);
    if (!lab_open(&arc->obj, path))
        OL_WARN("Cannot open olobj.lab (objects unavailable)\n");

    snprintf(path, sizeof(path), "%s/olweap.lab", arc->data_dir);
    if (!lab_open(&arc->weap, path))
        OL_WARN("Cannot open olweap.lab (weapons unavailable)\n");

    /* Patch / DLC archives (olpatch1..3.lab). Optional — override base data and
     * add the later story levels + "A Handful of Missions" DLC + MP maps. */
    for (int p = 0; p < 3; p++) {
        snprintf(path, sizeof(path), "%s/olpatch%d.lab", arc->data_dir, p + 1);
        arc->patch_open[p] = lab_open(&arc->patch[p], path);
        if (arc->patch_open[p])
            OL_LOG("Patch archive olpatch%d.lab opened\n", p + 1);
    }

    /* Load global base palette.
     *
     * Original engine behaviour (confirmed by Ghidra RE):
     *   1. At startup: syspal.pcx sets ALL 256 hardware palette entries.
     *      Indices 0-15 are EGA-like base colors (e.g. dark-blue, green, grays).
     *   2. On every level load: the level's PCX overwrites ONLY indices 16-255.
     *      Indices 1-15 therefore always come from syspal, NOT the level palette.
     *
     * Sprites heavily use indices 1-15 for body colors.  If we use olpal.pcx
     * (which has magenta at 1-15) as the base, those pixels become transparent →
     * "thin strip" rendering bug.
     *
     * Fix: load syspal.pcx first for all 256 entries; fall back to olpal.pcx if
     * syspal is absent; then for each level only replace indices 16-255.
     */
    {
        /* Helper lambda-like: try to load a PCX palette into dest */
        bool loaded = false;
        const char *syspal_names[] = { "SYSPAL.PCX", "syspal.pcx" };
        for (int i = 0; i < 2 && !loaded; i++) {
            u32 psize = 0;
            const u8 *pdata = lab_get(&arc->main, syspal_names[i], &psize);
            if (!pdata) pdata = lab_get(&arc->geo, syspal_names[i], &psize);
            if (pdata && psize > 769 && pdata[psize - 769] == 0x0C) {
                const u8 *pal = pdata + psize - 769 + 1;
                for (int c = 0; c < 256; c++) {
                    arc->base_palette[c][0] = pal[c*3 + 0];
                    arc->base_palette[c][1] = pal[c*3 + 1];
                    arc->base_palette[c][2] = pal[c*3 + 2];
                }
                loaded = true;
                OL_LOG("Base palette (syspal) loaded from: %s\n", syspal_names[i]);
            }
        }

        /* If syspal not found, bootstrap from olpal.pcx */
        if (!loaded) {
            const char *pal_pcx[] = { "OLPAL.PCX", "olpal.pcx" };
            for (int i = 0; i < 2 && !loaded; i++) {
                u32 psize = 0;
                const u8 *pdata = lab_get(&arc->main, pal_pcx[i], &psize);
                if (!pdata) pdata = lab_get(&arc->geo, pal_pcx[i], &psize);
                if (pdata && psize > 769 && pdata[psize - 769] == 0x0C) {
                    const u8 *pal = pdata + psize - 769 + 1;
                    for (int c = 0; c < 256; c++) {
                        arc->base_palette[c][0] = pal[c*3 + 0];
                        arc->base_palette[c][1] = pal[c*3 + 1];
                        arc->base_palette[c][2] = pal[c*3 + 2];
                    }
                    loaded = true;
                    OL_LOG("Base palette (olpal fallback) loaded from: %s\n", pal_pcx[i]);
                }
            }
        }

        if (loaded) {
            memcpy(arc->palette, arc->base_palette, sizeof(arc->palette));
            arc->palette_loaded = true;
        } else {
            OL_WARN("No syspal/olpal found, using grayscale fallback\n");
        }
    }
    /* Load the master UI palette (OLPAL.PCX) for weapons/HUD/ammo sprites.
     * These are authored against olpal, so decoding them with the per-level
     * palette (as world sprites use) gives wrong colors (e.g. a red pistol). */
    {
        const char *names[] = { "OLPAL.PCX", "olpal.pcx" };
        for (int i = 0; i < 2 && !arc->hud_palette_loaded; i++) {
            u32 psize = 0;
            const u8 *pdata = lab_get(&arc->main, names[i], &psize);
            if (!pdata) pdata = lab_get(&arc->obj, names[i], &psize);
            if (pdata && psize > 769 && pdata[psize - 769] == 0x0C) {
                const u8 *pal = pdata + psize - 769 + 1;
                for (int c = 0; c < 256; c++) {
                    arc->hud_palette[c][0] = pal[c*3 + 0];
                    arc->hud_palette[c][1] = pal[c*3 + 1];
                    arc->hud_palette[c][2] = pal[c*3 + 2];
                }
                arc->hud_palette_loaded = true;
                OL_LOG("HUD/weapon palette (olpal) loaded from: %s\n", names[i]);
            }
        }
        /* Indices 0-15 come from syspal on real hardware; olpal stores magenta
         * there. Copy the syspal base colors over so shared UI body colors
         * (skin, metal shadow) that reference 1-15 render correctly. */
        if (arc->hud_palette_loaded && arc->palette_loaded) {
            for (int c = 0; c < 16; c++) {
                arc->hud_palette[c][0] = arc->base_palette[c][0];
                arc->hud_palette[c][1] = arc->base_palette[c][1];
                arc->hud_palette[c][2] = arc->base_palette[c][2];
            }
        }
        if (!arc->hud_palette_loaded) {
            /* Fall back to the level base palette. */
            memcpy(arc->hud_palette, arc->base_palette, sizeof(arc->hud_palette));
        }
    }

    if (!arc->palette_loaded) {
        /* Fallback: grayscale ramp */
        for (int c = 0; c < 256; c++)
            arc->base_palette[c][0] = arc->base_palette[c][1] =
            arc->base_palette[c][2] = arc->palette[c][0] =
            arc->palette[c][1] = arc->palette[c][2] = (u8)c;
        OL_WARN("No palette found, using grayscale fallback\n");
    }

    arc->opened = true;
    return ok;
}

/* Try one archive with case variants (as-is, lower, UPPER). */
static const u8 *lab_get_ci(const LabArchive *lab, const char *name, u32 *size) {
    const u8 *d = lab_get(lab, name, size);
    if (d) return d;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", name);
    for (char *p = buf; *p; p++) *p = (char)tolower((unsigned char)*p);
    d = lab_get(lab, buf, size);
    if (d) return d;
    snprintf(buf, sizeof(buf), "%s", name);
    for (char *p = buf; *p; p++) *p = (char)toupper((unsigned char)*p);
    return lab_get(lab, buf, size);
}

/* Resolve a VFAT 8.3 short-name alias (e.g. "GWTRAI~1.PCX") by matching the
 * prefix before '~' against real archive entries and picking the Nth. Handles
 * the Outlaws LVTs authored on Windows that store ~N collision aliases. */
static const u8 *archives_get_vfat(const Archives *arc, const char *name,
                                   u32 *out_size) {
    const char *tilde = strchr(name, '~');
    if (!tilde) return NULL;
    int n = atoi(tilde + 1);
    if (n <= 0) return NULL;
    char prefix[16];
    size_t plen = (size_t)(tilde - name);
    if (plen == 0 || plen >= sizeof(prefix)) return NULL;
    memcpy(prefix, name, plen); prefix[plen] = '\0';
    for (char *p = prefix; *p; p++) *p = (char)tolower((unsigned char)*p);
    const char *ext = strrchr(name, '.');

    /* Collect prefix-matching entry names (with the same extension) in sorted
     * order across all archives, patches first; pick the Nth. */
    const LabArchive *labs[9]; u32 nl = 0;
    for (int p = 2; p >= 0; p--) if (arc->patch_open[p]) labs[nl++] = &arc->patch[p];
    labs[nl++] = &arc->geo; labs[nl++] = &arc->tex; labs[nl++] = &arc->main;
    labs[nl++] = &arc->obj; labs[nl++] = &arc->weap; labs[nl++] = &arc->sfx;

    char best[9][256]; u32 nbest = 0;
    for (u32 li = 0; li < nl; li++) {
        const LabArchive *lab = labs[li];
        for (u32 e = 0; e < lab->entry_count && nbest < 9; e++) {
            const char *en = lab->entries[e].name;   /* stored lowercase */
            if (strncmp(en, prefix, plen) != 0) continue;
            if (ext && !strstr(en, ext[0]=='.'?ext+0:ext)) {
                /* match extension loosely */
                const char *ee = strrchr(en, '.');
                if (!ee || strcasecmp(ee, ext) != 0) continue;
            }
            /* insertion sort into best[] to keep deterministic order */
            u32 pos = nbest;
            while (pos > 0 && strcmp(best[pos-1], en) > 0) { memcpy(best[pos], best[pos-1], 256); pos--; }
            snprintf(best[pos], 256, "%s", en); nbest++;
        }
        if (nbest > (u32)n) break;
    }
    if ((u32)n < nbest) {
        for (u32 li = 0; li < nl; li++) {
            const u8 *d = lab_get(labs[li], best[n], out_size);
            if (d) return d;
        }
    }
    return NULL;
}

const u8 *archives_get(const Archives *arc, const char *name, u32 *out_size) {
    if (!arc || !name) return NULL;
    /* Patches first (highest index = highest precedence), then base archives. */
    for (int p = 2; p >= 0; p--)
        if (arc->patch_open[p]) {
            const u8 *d = lab_get_ci(&arc->patch[p], name, out_size);
            if (d) return d;
        }
    const LabArchive *bases[] = { &arc->geo, &arc->main, &arc->tex,
                                  &arc->obj, &arc->weap, &arc->sfx };
    for (u32 i = 0; i < sizeof(bases)/sizeof(bases[0]); i++) {
        const u8 *d = lab_get_ci(bases[i], name, out_size);
        if (d) return d;
    }
    /* Last resort: VFAT ~N short-name alias resolution. */
    if (strchr(name, '~')) return archives_get_vfat(arc, name, out_size);
    return NULL;
}

void archives_close(Archives *arc) {
    if (!arc->opened) return;
    lab_close(&arc->main);
    lab_close(&arc->geo);
    lab_close(&arc->tex);
    lab_close(&arc->sfx);
    lab_close(&arc->obj);
    lab_close(&arc->weap);
    for (int p = 0; p < 3; p++) if (arc->patch_open[p]) lab_close(&arc->patch[p]);
    arc->opened = false;
}

/* -------------------------------------------------------------------------
 * Texture upload
 * ---------------------------------------------------------------------- */

/* Counts level textures that couldn't be found (level health check). */
static u32 g_missing_tex = 0;

/* Jedi-engine texel convention (TFE rtexture/rwallFloat, Outlaws same engine):
 * texel (U=0,V=0) is the image's BOTTOM-left; V grows upward. PCX decodes rows
 * top-down, so LEVEL textures (walls/flats/sky) are flipped vertically at
 * upload so GL v=0 lands on the image's bottom row. The renderer's wall V
 * (bottom-anchored, growing up) and flat V ((worldZ-offset)*8) then map 1:1 to
 * the engine's texel space. HUD/menu/sprite uploads are untouched (their draw
 * paths use top-down UVs). */
static void flip_rgba_rows(u8 *rgba, u32 w, u32 h) {
    if (!rgba || h < 2) return;
    u32 stride = w * 4;
    u8 *tmp = malloc(stride);
    if (!tmp) return;
    for (u32 y = 0; y < h / 2; y++) {
        u8 *a = rgba + (size_t)y * stride;
        u8 *b = rgba + (size_t)(h - 1 - y) * stride;
        memcpy(tmp, a, stride);
        memcpy(a, b, stride);
        memcpy(b, tmp, stride);
    }
    free(tmp);
}

static void upload_level_textures(const LvtLevel *level,
                                   const Archives *arc,
                                   Renderer *r,
                                   const u8 palette[256][3]) {
    for (u32 i = 0; i < level->texture_count; i++) {
        const char *name = level->textures[i];
        if (!name[0]) continue;
        /* Blank / placeholder slot (e.g. ".PCX" with no basename) — not a real
         * texture reference; skip without counting it as missing. */
        if (name[0] == '.' || strlen(name) < 5) continue;
        if (renderer_find_texture(r, name)) continue;

        u32 size = 0;
        const u8 *data = archives_get(arc, name, &size);   /* patches override */
        if (!data) {
            OL_WARN("Texture not found: %s\n", name);
            g_missing_tex++;
            continue;
        }

        const char *ext = strrchr(name, '.');
        if (!ext) continue;

        char lower_name[LVT_MAX_NAME];
        snprintf(lower_name, sizeof(lower_name), "%s", name);
        for (char *p = lower_name; *p; p++) *p = tolower((unsigned char)*p);

        /* Glass window textures (name contains "WIN") render their low palette
         * indices (1-15) as translucent glass. */
        bool is_glass = (strstr(lower_name, "win") != NULL);

        if (strcasecmp(ext, ".pcx") == 0) {
            u32 w = 0, h = 0;
            u8 *rgba;
            if (palette && is_glass)
                rgba = pcx_decode_rgba_glass(data, size, &w, &h, palette);
            else
                rgba = palette ? pcx_decode_rgba_pal(data, size, &w, &h, palette)
                                : pcx_decode_rgba(data, size, &w, &h);
            if (rgba) {
                flip_rgba_rows(rgba, w, h);
                renderer_upload_texture(r, lower_name, rgba, w, h);
                free(rgba);
            }
        } else if (strcasecmp(ext, ".atx") == 0) {
            /*
             * ATX animated texture: parse RATE and all TEXTURE lines.
             * Format: "ATX 1.0\nINST N\n  RATE fps\n  TEXTURE frame.PCX\n  ..."
             * We collect all unique TEXTURE names (in order) and upload each.
             * The base texture slot (named by the ATX filename) swaps GL handles
             * each frame via renderer_update_anim_textures().
             */
            char atx_text[4096];
            u32 copy_len = (size < sizeof(atx_text)-1) ? size : sizeof(atx_text)-1;
            memcpy(atx_text, data, copy_len);
            atx_text[copy_len] = '\0';

            /* Parse framerate */
            f32 atx_fps = 12.0f;
            {
                const char *rp = atx_text;
                while (*rp) {
                    while (*rp == ' ' || *rp == '\t' || *rp == '\r' || *rp == '\n') rp++;
                    if (strncasecmp(rp, "RATE", 4) == 0 && (rp[4]==' '||rp[4]=='\t')) {
                        float v = 0;
                        if (sscanf(rp + 4, "%f", &v) == 1 && v > 0)
                            atx_fps = v;
                        break;
                    }
                    while (*rp && *rp != '\n') rp++;
                }
            }

            /* Parse the ATX program (a small state machine), collecting the
             * PASSIVE animation only — the frames from the start up to the
             * first STOP or GOTO:
             *   - Terminated by STOP  -> static texture (holds frame 0). The
             *     frames after the STOP are a triggered sequence (e.g. a window
             *     breaking when shot) and are NOT looped passively.
             *   - Terminated by GOTO  -> loops the collected frames. loop_start
             *     is the GOTO target's frame index.
             * (Ignoring this and looping every TEXTURE line made windows appear
             * to break on a loop and made many textures animate wrongly.) */
            char frame_names[R_MAX_ANIM_FRAMES][64];
            u32  frame_count = 0;
            bool atx_loop = false;
            u32  atx_loop_start = 0;
            const char *p = atx_text;
            bool seen_rate = false; /* RATE is instruction 0; frames follow */
            /* Map: instruction index -> frame index (for GOTO target). */
            while (*p && frame_count < R_MAX_ANIM_FRAMES) {
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
                if (!*p) break;
                if (*p == '#') { while (*p && *p != '\n') p++; continue; }

                if (strncasecmp(p, "RATE", 4) == 0) {
                    seen_rate = true;
                } else if (strncasecmp(p, "TEXTURE", 7) == 0 &&
                           (p[7] == ' ' || p[7] == '\t')) {
                    char fname[64] = {0};
                    sscanf(p + 8, "%63s", fname);
                    for (char *q = fname; *q; q++) { if (*q == '#') { *q = '\0'; break; } }
                    if (fname[0]) snprintf(frame_names[frame_count++], 64, "%s", fname);
                } else if (strncasecmp(p, "STOP", 4) == 0) {
                    /* End of passive sequence: static (no loop). */
                    break;
                } else if (strncasecmp(p, "GOTO", 4) == 0) {
                    /* GOTO [STOP] <instr_index> — loop back. Take the last number
                     * on the line as the target instruction; frame index =
                     * instr_index - 1 (RATE is instruction 0). */
                    int target = 1;
                    const char *q = p + 4;
                    const char *last_num = NULL;
                    while (*q && *q != '\n') {
                        if ((*q >= '0' && *q <= '9')) { last_num = q; while (*q>='0'&&*q<='9') q++; }
                        else q++;
                    }
                    if (last_num) target = atoi(last_num);
                    (void)seen_rate;
                    atx_loop = true;
                    atx_loop_start = (target >= 1) ? (u32)(target - 1) : 0;
                    if (atx_loop_start >= frame_count)
                        atx_loop_start = 0;
                    break;
                }
                while (*p && *p != '\n') p++;
            }

            if (frame_count == 0) goto atx_done;

            /* Upload each frame PCX and collect GL handles */
            GLuint handles[R_MAX_ANIM_FRAMES] = {0};
            u32    uploaded = 0;
            bool   base_registered = false;

            for (u32 f = 0; f < frame_count; f++) {
                u32 fs = 0;
                const u8 *fd = lab_get(&arc->tex, frame_names[f], &fs);
                if (!fd) fd = lab_get(&arc->geo, frame_names[f], &fs);
                if (!fd || !fs) continue;

                u32 w = 0, h = 0;
                u8 *rgba;
                if (palette && is_glass)
                    rgba = pcx_decode_rgba_glass(fd, fs, &w, &h, palette);
                else
                    rgba = palette ? pcx_decode_rgba_pal(fd, fs, &w, &h, palette)
                                   : pcx_decode_rgba(fd, fs, &w, &h);
                if (!rgba) continue;
                flip_rgba_rows(rgba, w, h);

                /* Name: frame 0 uses the ATX name (for mesh lookup), others get _fN suffix */
                char tex_name[128];
                if (!base_registered) {
                    snprintf(tex_name, sizeof(tex_name), "%s", lower_name);
                    base_registered = true;
                } else {
                    snprintf(tex_name, sizeof(tex_name), "%s_f%u", lower_name, uploaded);
                }

                u32 tid = renderer_upload_texture(r, tex_name, rgba, w, h);
                free(rgba);
                if (tid && tid <= R_MAX_TEXTURES)
                    handles[uploaded++] = r->textures[tid - 1].handle;
            }

            /* Register the animated texture. Looping textures (GOTO) animate;
             * STOP-terminated ones stay static on frame 0 (already uploaded as
             * the base texture). Only register when there is something to do. */
            if (uploaded >= 1) {
                u32 base_tid = renderer_find_texture(r, lower_name);
                if (base_tid && (atx_loop && uploaded > 1)) {
                    u32 ls = (atx_loop_start < uploaded) ? atx_loop_start : 0;
                    renderer_add_anim_texture(r, base_tid, handles, uploaded,
                                              atx_fps, true, ls);
                }
            }
            atx_done:;
        }
    }
}

/* -------------------------------------------------------------------------
 * WAX sprite loading for entities
 * ---------------------------------------------------------------------- */

/*
 * Try to load WAX sprite data for an entity type.
 * Returns a decoded WaxSprite on success (caller must call wax_free).
 * Sets tex_base_name to the lowercase type name base (e.g. "bgy1.nwx").
 */
static bool load_entity_wax(const char *type_name, const Archives *arc,
                             WaxSprite *sprite, char tex_base[64]) {
    /* Build canonical texture base name */
    snprintf(tex_base, 64, "%s.nwx", type_name);
    for (char *p = tex_base; *p; p++) *p = tolower((unsigned char)*p);

    /* Try <TYPE>.WAX then <TYPE>.NWX in olobj.lab */
    char wax_name[64];
    snprintf(wax_name, sizeof(wax_name), "%s.WAX", type_name);

    u32 wsize = 0;
    const u8 *wdata = lab_get(&arc->obj, wax_name, &wsize);
    if (!wdata) {
        for (char *p = wax_name; *p; p++) *p = tolower((unsigned char)*p);
        wdata = lab_get(&arc->obj, wax_name, &wsize);
    }
    if (!wdata) {
        snprintf(wax_name, sizeof(wax_name), "%s.NWX", type_name);
        wdata = lab_get(&arc->obj, wax_name, &wsize);
        if (!wdata) {
            for (char *p = wax_name; *p; p++) *p = tolower((unsigned char)*p);
            wdata = lab_get(&arc->obj, wax_name, &wsize);
        }
    }
    if (!wdata) return false;

    return wax_decode(sprite, wdata, wsize, arc->palette);
}

/* Scale factor: NWX pixel dimensions → world units.
 * Calibrated from ITM: BGY1 enemy HEIGHT=6.0 wu, idle-standing CHOT dir-0 height=103 px → 6.0/103 ≈ 0.0583 */
#define NWX_PIXEL_TO_WORLD 0.0583f

/*
 * Load all 8 directional textures for an entity type, plus animation cells.
 * Populates dir_tex[8] (per-direction sprites) and optionally anim_tex[8]
 * (cyclic animation frames for non-directional animated decorations).
 * Returns the primary texture ID or 0 on failure.
 */
static u32 load_entity_sprite_dirs(const char *type_name, const Archives *arc,
                                    Renderer *r, u32 dir_tex[8],
                                    f32 *out_w, f32 *out_h,
                                    u32 anim_tex[8], u32 *anim_count,
                                    u32 *anim_dt_ms) {
    /* Initialize optional animation outputs */
    if (anim_tex)   for (int i = 0; i < 8; i++) anim_tex[i] = 0;
    if (anim_count) *anim_count = 0;
    if (anim_dt_ms) *anim_dt_ms = 100;

    /* Check if already uploaded (reuse) */
    char tex_base[64];
    snprintf(tex_base, 64, "%s.nwx", type_name);
    for (char *p = tex_base; *p; p++) *p = tolower((unsigned char)*p);

    /* Look for existing dir_0 texture */
    char dir0_name[72];
    snprintf(dir0_name, sizeof(dir0_name), "%s_d0", tex_base);
    u32 existing = renderer_find_texture(r, dir0_name);
    if (existing) {
        /* Rebuild dir_tex from existing uploads */
        for (int d = 0; d < 8; d++) {
            char dn[72];
            snprintf(dn, sizeof(dn), "%s_d%d", tex_base, d);
            dir_tex[d] = renderer_find_texture(r, dn);
        }
        /* Rebuild anim_tex from existing uploads */
        if (anim_tex && anim_count) {
            for (int f = 0; f < 8; f++) {
                char fn[72];
                snprintf(fn, sizeof(fn), "%s_f%d", tex_base, f);
                u32 ft = renderer_find_texture(r, fn);
                if (!ft) break;
                anim_tex[(*anim_count)++] = ft;
            }
        }
        /* Retrieve cached dimensions from GPU texture for out_w/out_h */
        if (existing <= r->texture_count) {
            if (out_w) *out_w = r->textures[existing-1].width  * NWX_PIXEL_TO_WORLD;
            if (out_h) *out_h = r->textures[existing-1].height * NWX_PIXEL_TO_WORLD;
        }
        return dir_tex[0];
    }

    WaxSprite sprite;
    if (!load_entity_wax(type_name, arc, &sprite, tex_base)) {
        for (int d = 0; d < 8; d++) dir_tex[d] = 0;
        if (out_w) *out_w = 0;
        if (out_h) *out_h = 0;
        return 0;
    }

    /* Export world-space dimensions from first FRMT frame */
    if (out_w) *out_w = sprite.first_w * NWX_PIXEL_TO_WORLD;
    if (out_h) *out_h = sprite.first_h * NWX_PIXEL_TO_WORLD;

    u32 primary = 0;
    for (int d = 0; d < 8; d++) {
        dir_tex[d] = 0;
        u32 cidx = sprite.dir_cell[d];
        if (cidx < sprite.cell_count && sprite.cells[cidx].pixels &&
            sprite.cells[cidx].width > 0 && sprite.cells[cidx].height > 0) {
            char dn[72];
            snprintf(dn, sizeof(dn), "%s_d%d", tex_base, d);
            const WaxCell *cell = &sprite.cells[cidx];
            dir_tex[d] = renderer_upload_texture(r, dn, cell->pixels,
                                                  cell->width, cell->height);
            if (d == 0) primary = dir_tex[d];
        }
    }

    /* Fallback: if dir 0 empty, use any valid cell */
    if (!primary && sprite.cell_count > 0) {
        for (u32 c = 0; c < sprite.cell_count && !primary; c++) {
            const WaxCell *cell = &sprite.cells[c];
            if (cell->pixels && cell->width > 0 && cell->height > 0) {
                char dn[72];
                snprintf(dn, sizeof(dn), "%s_d0", tex_base);
                primary = renderer_upload_texture(r, dn, cell->pixels,
                                                   cell->width, cell->height);
                for (int d = 0; d < 8; d++)
                    if (!dir_tex[d]) dir_tex[d] = primary;
            }
        }
    }

    /*
     * Load animation cells: cells NOT referenced by any dir_cell[],
     * uploaded as _f0, _f1, ... for cyclic animation.
     * Only relevant when all 8 dir_cells point to the same cell (non-directional).
     */
    if (anim_tex && anim_count && sprite.cell_count > 0) {
        /* Check if all directions use the same cell (non-directional sprite) */
        bool all_same_dir = true;
        u32  dir0_cidx = sprite.dir_cell[0];
        for (int d = 1; d < 8; d++) {
            if (sprite.dir_cell[d] != dir0_cidx) { all_same_dir = false; break; }
        }

        if (all_same_dir && sprite.cell_count > 1) {
            /* Collect average dt from FRMT frames with dt > 0 */
            u32 total_dt = 0, dt_count = 0;
            for (u32 f = 0; f < sprite.frame_count; f++) {
                if (sprite.frames[f].dt > 0) {
                    total_dt += sprite.frames[f].dt;
                    dt_count++;
                }
            }
            u32 avg_dt = (dt_count > 0) ? (total_dt / dt_count) : 150u;
            if (anim_dt_ms) *anim_dt_ms = (avg_dt > 0 && avg_dt < 2000u) ? avg_dt : 150u;

            /* Upload each cell as an animation frame */
            for (u32 c = 0; c < sprite.cell_count && *anim_count < 8; c++) {
                const WaxCell *cell = &sprite.cells[c];
                if (!cell->pixels || cell->width == 0 || cell->height == 0) continue;
                char fn[72];
                snprintf(fn, sizeof(fn), "%s_f%u", tex_base, *anim_count);
                u32 ftid = renderer_upload_texture(r, fn, cell->pixels,
                                                    cell->width, cell->height);
                if (ftid) anim_tex[(*anim_count)++] = ftid;
            }
        }
    }

    wax_free(&sprite);
    return primary;
}

/* -------------------------------------------------------------------------
 * Scenery (Inv_Object) chor loading — Ghidra RE of olwin.exe:
 * Misc_CreateActorFromItem@0x4726c0 loads <class>.itm; FUNC selects the
 * logic (Inv_Object@0x418da0 for destructible/nudgeable scenery); the NWX
 * chors are bytecode scripts and state 0 is the spawn state.
 * ---------------------------------------------------------------------- */

/* Analyze one chor token stream into a ScnChor: display frames (resolved to
 * uploaded cell textures), start sound (0xFFFD), and terminal opcode.
 * Token layout (Wax_NextFrame@0x44ca00): [opcode][param][extras...]. */
static void build_scn_chor(Renderer *r, const WaxSprite *sp,
                           const char *tex_base, const WaxChor *ch,
                           ScnChor *out) {
    memset(out, 0, sizeof(*out));
    out->sound_idx = -1;
    out->end   = SCN_END_STOP;
    out->dt_ms = (ch->rate > 0) ? (1000u / (u32)ch->rate) : 100u;

    const u16 *seq = ch->seq;
    u32 len = ch->seq_len;
    for (u32 t = 0; t < len; t++) {
        u16 tok = seq[t];
        if (tok < 0x8000) {
            /* Display frame: frame idx -> cell -> texture */
            if (tok < sp->frame_count && out->nframes < SCN_MAX_FRAMES) {
                u32 ci = sp->frames[tok].cell_idx;
                if (ci < sp->cell_count && sp->cells[ci].pixels &&
                    sp->cells[ci].width > 0 && sp->cells[ci].height > 0) {
                    char fn[96];
                    snprintf(fn, sizeof(fn), "%s_c%u", tex_base, ci);
                    u32 tid = renderer_upload_texture(r, fn,
                                                      sp->cells[ci].pixels,
                                                      sp->cells[ci].width,
                                                      sp->cells[ci].height);
                    if (tid) {
                        u32 n = out->nframes++;
                        out->tex[n] = tid;
                        out->fw[n]  = sp->cells[ci].width  * NWX_PIXEL_TO_WORLD;
                        out->fh[n]  = sp->cells[ci].height * NWX_PIXEL_TO_WORLD;
                    }
                }
            }
            continue;
        }
        u16 param = (t + 1 < len) ? seq[t + 1] : 0;
        t++;                                     /* consume param word */
        switch (tok) {
        case 0xFFFF: out->end = SCN_END_STOP;      return;
        case 0xFFFE: out->end = SCN_END_LOOP;      return;
        case 0xFFF8: out->end = SCN_END_TERMINATE; return;
        case 0xFFF9: out->end = SCN_END_STOP;      return; /* pause/hold */
        case 0xFFFC: {                             /* SETSTATE <chor> */
            u16 st = (t + 1 < len) ? seq[t + 1] : 0; t++;
            out->end = SCN_END_SETSTATE;
            out->end_state = (u8)st;
            return;
        }
        case 0xFFFD: {                             /* PLAYSOUND <sounds.lst idx> */
            u16 sidx = (t + 1 < len) ? seq[t + 1] : 0; t++;
            if (out->sound_idx < 0) out->sound_idx = (i32)sidx;
            break;
        }
        case 0xFFFB: t += param; break;            /* CALLBACK, param extras */
        case 0xFFFA:                               /* SETFRAME <pos> */
        case 0xFFF7: t += 1; break;                /* PLAYSOUND2 */
        default: break;                            /* FFF6/FFF5 show/hide etc. */
        }
    }
}

/* Set up the Inv_Object chor state machine for an entity, from its ITM.
 * Returns true if the entity is chor-driven scenery. */
static bool load_scenery(Entity *e, const Archives *arc, Renderer *r) {
    char itm_name[96];
    snprintf(itm_name, sizeof(itm_name), "%s.itm", e->type_name);
    u32 sz = 0;
    const u8 *data = archives_get(arc, itm_name, &sz);
    if (!data || !sz) return false;

    ItmFile itm;
    if (!itm_parse(&itm, (const char *)data, sz)) return false;
    if (strcasecmp(itm.func, "Inv_Object") != 0) return false;

    /* TYPE field -> ray-solidity / reaction class (actor+0x78 semantics) */
    const char *type = itm_get_str(&itm, "TYPE", NULL);
    if (!type)                              e->scenery_type = SCENERY_PASS;
    else if (strcasecmp(type, "SHOOT") == 0) e->scenery_type = SCENERY_SHOOT;
    else                                     e->scenery_type = SCENERY_NUDGE;

    /* Costume: ANIM field (fallback: class name). Strip the extension —
     * load_entity_wax appends .nwx itself. */
    char base[64];
    if (itm.anim[0] && strcasecmp(itm.anim, "NULL") != 0)
        snprintf(base, sizeof(base), "%s", itm.anim);
    else
        snprintf(base, sizeof(base), "%s", e->type_name);
    char *dot = strrchr(base, '.');
    if (dot) {
        if (strcasecmp(dot, ".3do") == 0) return false; /* 3DO scenery: not chor-driven */
        *dot = '\0';
    }

    char tex_base[72];
    snprintf(tex_base, sizeof(tex_base), "%s.nwx", base);
    for (char *p = tex_base; *p; p++) *p = tolower((unsigned char)*p);

    WaxSprite sprite;
    if (!load_entity_wax(base, arc, &sprite, tex_base)) return false;
    if (sprite.chor_count == 0) { wax_free(&sprite); return false; }

    e->scn_count = (sprite.chor_count < SCN_MAX_CHORS)
                   ? sprite.chor_count : SCN_MAX_CHORS;
    for (u32 c = 0; c < e->scn_count; c++)
        build_scn_chor(r, &sprite, tex_base, &sprite.chors[c], &e->scn[c]);

    /* Collision cylinder: ITM RADIUS/HEIGHT; non-positive values fall back
     * to the WAX frame dimensions (Misc_CreateActorFromItem@0x472ab5). */
    f32 radius = itm_get_float(&itm, "RADIUS", -1.0f);
    f32 height = itm_get_float(&itm, "HEIGHT", -1.0f);
    e->sprite_w = (radius > 0.0f) ? radius * 2.0f
                                  : sprite.first_w * NWX_PIXEL_TO_WORLD;
    e->sprite_h = (height > 0.0f) ? height
                                  : sprite.first_h * NWX_PIXEL_TO_WORLD;

    e->is_scenery  = true;
    e->scn_state   = 0;     /* spawn state is always chor 0 */
    e->scn_frame   = 0;
    e->scn_timer   = 0.0f;
    e->scn_playing = true;
    e->anim_count  = 0;     /* disable the legacy cyclic-cells fallback */
    wax_free(&sprite);
    return true;
}

/*
 * Enemy costume choreography indices.
 *
 * All BGY (and derived) enemy sprites share one costume enum, confirmed by
 * reverse-engineering the WAX chor tables of BGY1..BGY9 (they are structurally
 * identical). The BGY_New logic (olwin.exe @0x00477e30) drives these chors:
 *   chor 0  = STAND      (idle: single idle cell, looped)
 *   chor 1  = HIT        (flinch/hurt reaction, stays upright, rate 5)
 *   chor 10 = SHOOT      (aim + fire, contains the 0xFFFB fire event)
 *   chor 21 = DIE        (death fall — ends in a WIDE lying corpse cell)
 *   chor 22 = DIE (side), chor 23 = DIE (back) — alternate fall directions
 * The death chors (21-23) are the ones ending in horizontal "on-ground" cells;
 * they were verified by rendering the final frames of every BGY chor. There is
 * no distinct walk cycle for BGY shooters — they stand/aim while repositioning,
 * so WALK reuses the idle stand pose.
 */
enum {
    ENEMY_CHOR_STAND = 0,
    ENEMY_CHOR_HIT   = 1,
    ENEMY_CHOR_SHOOT = 10,
    ENEMY_CHOR_DIE   = 21,
    ENEMY_CHOR_WALK  = 0,
};

/*
 * Build one EntityAnimSeq (8 directions x N frames) from a costume chor.
 * For each of the 8 render directions we expand the chor's per-direction token
 * stream (via wax_chor_frames_dir), resolve each display frame to its cell, and
 * upload the cell texture. The chor's rate sets the per-frame duration.
 * Returns the number of animation frames (0 if the chor is absent/empty).
 */
static u32 build_enemy_seq(Renderer *r, const WaxSprite *sp, const char *tex_base,
                           u32 chor, EntityAnimSeq *as) {
    memset(as, 0, sizeof(*as));
    if (chor >= sp->chor_count) return 0;

    /* Direction 0 defines the frame count / timing for the whole sequence. */
    u32 f0[WAX_MAX_SEQ], dt0[WAX_MAX_SEQ];
    u32 nf = wax_chor_frames_dir(sp, chor, 0, f0, dt0, WAX_MAX_SEQ);
    if (nf == 0) return 0;
    if (nf > ENTITY_MAX_SEQ_FRAMES) nf = ENTITY_MAX_SEQ_FRAMES;

    as->frame_count = nf;
    as->dt_ms       = (nf > 0 && dt0[0] > 0) ? dt0[0] : 150u;

    for (u32 d = 0; d < 8; d++) {
        /* Render direction d -> CHOT direction slot (matches dir_cell sampling
         * in wax.c: slot = d * ndirs / 8). */
        u32 ndirs = sp->chors[chor].ndirs ? sp->chors[chor].ndirs : 8;
        u32 slot  = (u32)((u64)d * ndirs / 8u);

        u32 fd[WAX_MAX_SEQ];
        u32 nfd = wax_chor_frames_dir(sp, chor, slot, fd, NULL, WAX_MAX_SEQ);

        for (u32 af = 0; af < nf; af++) {
            u32 fi = (af < nfd) ? fd[af] : (af < nf ? f0[af] : 0);
            u32 cidx = (fi < sp->frame_count) ? sp->frames[fi].cell_idx : 0;
            u32 tid = 0;
            if (cidx < sp->cell_count && sp->cells[cidx].pixels &&
                sp->cells[cidx].width > 0 && sp->cells[cidx].height > 0) {
                const WaxCell *cell = &sp->cells[cidx];
                char tn[96];
                snprintf(tn, sizeof(tn), "%s_c%u_d%u_f%u", tex_base, chor, d, af);
                tid = renderer_find_texture(r, tn);
                if (!tid)
                    tid = renderer_upload_texture(r, tn, cell->pixels,
                                                  cell->width, cell->height);
                as->fw[d][af] = (f32)cell->width  * NWX_PIXEL_TO_WORLD;
                as->fh[d][af] = (f32)cell->height * NWX_PIXEL_TO_WORLD;
            }
            as->dir_frames[d][af] = tid;
        }
    }
    return nf;
}

/*
 * Load per-AI-state animation sequences for an enemy from its WAX costume.
 * Uses the reverse-engineered BGY chor indices to build IDLE/WALK/ATTACK/DIE,
 * with 8-directional frames. DEAD (corpse) holds the last DIE frame.
 */
static void load_entity_anim_seqs(const char *type_name, const Archives *arc,
                                   Renderer *r, Entity *e) {
    if (e->kind != ENTITY_ENEMY) return;

    char tex_base[64];
    WaxSprite sprite;
    if (!load_entity_wax(type_name, arc, &sprite, tex_base))
        return;

    build_enemy_seq(r, &sprite, tex_base, ENEMY_CHOR_STAND, &e->anim_seqs[ANIM_IDLE]);
    build_enemy_seq(r, &sprite, tex_base, ENEMY_CHOR_WALK,  &e->anim_seqs[ANIM_WALK]);
    build_enemy_seq(r, &sprite, tex_base, ENEMY_CHOR_SHOOT, &e->anim_seqs[ANIM_ATTACK]);
    build_enemy_seq(r, &sprite, tex_base, ENEMY_CHOR_DIE,   &e->anim_seqs[ANIM_DIE]);
    /* HIT: flinch/hurt reaction played briefly when the enemy takes non-fatal
     * damage (chor 1, stays upright). */
    build_enemy_seq(r, &sprite, tex_base, ENEMY_CHOR_HIT,   &e->anim_seqs[ANIM_PAIN]);
    if (e->anim_seqs[ANIM_PAIN].frame_count == 0 &&
        e->anim_seqs[ANIM_IDLE].frame_count > 0) {
        /* Fallback: flash the idle frame if no hit chor. */
        EntityAnimSeq *pain = &e->anim_seqs[ANIM_PAIN];
        EntityAnimSeq *idle = &e->anim_seqs[ANIM_IDLE];
        pain->frame_count = 1; pain->dt_ms = 150;
        for (int d = 0; d < 8; d++) {
            pain->dir_frames[d][0] = idle->dir_frames[d][0];
            pain->fw[d][0] = idle->fw[d][0];
            pain->fh[d][0] = idle->fh[d][0];
        }
    }

    /* IDLE fallback: use the directional idle sprite if the chor was empty. */
    if (e->anim_seqs[ANIM_IDLE].frame_count == 0) {
        EntityAnimSeq *idle = &e->anim_seqs[ANIM_IDLE];
        idle->frame_count = 1;
        idle->dt_ms = 200;
        for (int d = 0; d < 8; d++)
            idle->dir_frames[d][0] = e->sprite_dir_tex[d];
    }

    /* DEAD (corpse): hold the last DIE frame — copy its texture AND per-frame
     * world size so the wide lying-body cell keeps its aspect (otherwise it
     * falls back to the tall idle size and the corpse looks stretched). */
    if (e->anim_seqs[ANIM_DEAD].frame_count == 0 &&
        e->anim_seqs[ANIM_DIE].frame_count > 0) {
        EntityAnimSeq *dead = &e->anim_seqs[ANIM_DEAD];
        EntityAnimSeq *die  = &e->anim_seqs[ANIM_DIE];
        dead->frame_count = 1;
        dead->dt_ms = 1000;
        u32 last = die->frame_count - 1;
        for (int d = 0; d < 8; d++) {
            dead->dir_frames[d][0] = die->dir_frames[d][last];
            dead->fw[d][0]         = die->fw[d][last];
            dead->fh[d][0]         = die->fh[d][last];
        }
    }

    e->has_anim_seqs = (e->anim_seqs[ANIM_WALK].frame_count > 0 ||
                        e->anim_seqs[ANIM_ATTACK].frame_count > 0 ||
                        e->anim_seqs[ANIM_DIE].frame_count > 0);

    wax_free(&sprite);
}

/* -------------------------------------------------------------------------
 * World loading
 * ---------------------------------------------------------------------- */

bool world_load(World *world, Archives *arc,
                Renderer *r, const char *level_name) {
    if (world->loaded) world_free(world);
    memset(world, 0, sizeof(*world));
    entity_list_init(&world->entities);
    inf_init(&world->inf);
    msg_init(&world->messages);

    /* Load the LOCAL.MSG string table (USER_MSG text, key/lock hints). Shared
     * across all levels; lives in outlaws.lab. */
    {
        u32 msg_size = 0;
        const u8 *msg_data = lab_get(&arc->main, "LOCAL.MSG", &msg_size);
        if (!msg_data) msg_data = lab_get(&arc->main, "local.msg", &msg_size);
        if (msg_data && msg_size > 0) {
            u32 n = msg_load(&world->messages, (const char *)msg_data, msg_size);
            OL_LOG("LOCAL.MSG loaded: %u messages\n", n);
        }
    }

    /* Build filenames */
    char lvt_name[128], obt_name[128];
    snprintf(lvt_name, sizeof(lvt_name), "%s.LVT", level_name);
    snprintf(obt_name, sizeof(obt_name), "%s.OBT", level_name);

    OL_LOG("Loading level: %s\n", level_name);

    /* Load LVT — patches/DLC override the base archive (archives_get). */
    u32 lvt_size = 0;
    const u8 *lvt_data = archives_get(arc, lvt_name, &lvt_size);
    if (!lvt_data) {
        OL_ERR("LVT not found: %s\n", lvt_name);
        return false;
    }

    if (!lvt_parse(&world->lvt, (const char *)lvt_data, lvt_size)) {
        OL_ERR("Failed to parse LVT: %s\n", lvt_name);
        return false;
    }

    /* Copy music filename from LVT header */
    snprintf(world->music_file, sizeof(world->music_file), "%s", world->lvt.music_file);

    /* Build the working palette for this level.
     *
     * Original engine (Ghidra-confirmed):
     *   syspal.pcx is loaded at startup → sets ALL 256 HW palette entries.
     *   Level load calls Level_ChangePalette which uses:
     *     FUN_00497d10(level_pal_ptr, 0x10, 0xF0)
     *   = overwrites indices 16 through 255 from the level's PCX.
     *   Indices 0-15 are NEVER changed by levels (they stay as syspal values).
     *
     * We replicate this exactly:
     *   Start from base_palette (= syspal.pcx).
     *   Overwrite indices 16-255 with the level PCX. */
    memcpy(arc->palette, arc->base_palette, sizeof(arc->palette));

    if (world->lvt.palette_name[0]) {
        u8 lvl_pal[256][3] = {0};
        bool lvl_pal_loaded = false;

        /* Try "<palname>.pcx" */
        {
            char name[LVT_MAX_NAME + 8];
            snprintf(name, sizeof(name), "%s.pcx", world->lvt.palette_name);
            u32 psize = 0;
            const u8 *pdata = lab_get(&arc->main, name, &psize);
            if (!pdata) pdata = lab_get(&arc->tex, name, &psize);
            if (!pdata) pdata = lab_get(&arc->geo, name, &psize);
            if (pdata && psize > 769 && pdata[psize - 769] == 0x0C) {
                const u8 *pal = pdata + psize - 769 + 1;
                for (int c = 0; c < 256; c++) {
                    lvl_pal[c][0] = pal[c*3 + 0];
                    lvl_pal[c][1] = pal[c*3 + 1];
                    lvl_pal[c][2] = pal[c*3 + 2];
                }
                lvl_pal_loaded = true;
                OL_LOG("Level palette (PCX) loaded: %s\n", name);
            }
        }

        /* Try "<palname>.pal" — raw 768-byte RGB */
        if (!lvl_pal_loaded) {
            char name[LVT_MAX_NAME + 8];
            snprintf(name, sizeof(name), "%s.pal", world->lvt.palette_name);
            u32 psize = 0;
            const u8 *pdata = lab_get(&arc->main, name, &psize);
            if (!pdata) pdata = lab_get(&arc->tex, name, &psize);
            if (!pdata) pdata = lab_get(&arc->geo, name, &psize);
            if (pdata && psize >= 768) {
                for (int c = 0; c < 256; c++) {
                    lvl_pal[c][0] = pdata[c*3 + 0];
                    lvl_pal[c][1] = pdata[c*3 + 1];
                    lvl_pal[c][2] = pdata[c*3 + 2];
                }
                lvl_pal_loaded = true;
                OL_LOG("Level palette (PAL) loaded: %s\n", name);
            }
        }

        if (lvl_pal_loaded) {
            /* Overwrite indices 16-255 with level palette (mirror original engine) */
            for (int c = 16; c < 256; c++) {
                arc->palette[c][0] = lvl_pal[c][0];
                arc->palette[c][1] = lvl_pal[c][1];
                arc->palette[c][2] = lvl_pal[c][2];
            }
            OL_LOG("Palette: indices 16-255 set from level palette '%s'\n",
                   world->lvt.palette_name);
        } else {
            OL_LOG("Level palette '%s' not found; using syspal base for all 256\n",
                   world->lvt.palette_name);
        }
    }

    /* Load INF (interactive scripting) before building GPU geometry so that
     * scroll-floor sectors are known at mesh build time. */
    {
        char inf_name[128];
        snprintf(inf_name, sizeof(inf_name), "%s.INF", level_name);
        u32 inf_size = 0;
        const u8 *inf_data = archives_get(arc, inf_name, &inf_size);
        if (inf_data && inf_size > 0) {
            inf_load(&world->inf, (const char *)inf_data, inf_size);
            inf_resolve_sectors(&world->inf, &world->lvt);
            OL_LOG("INF loaded: %u elevators/doors\n", world->inf.count);
        }
    }
    if (getenv("OL_FINDXZ")) {
        f32 qx=0, qz=0; sscanf(getenv("OL_FINDXZ"), "%f,%f", &qx, &qz);
        for (u32 i=0;i<world->lvt.sector_count;i++){
            LvtSector *s=&world->lvt.sectors[i];
            f32 minx=1e9,maxx=-1e9,minz=1e9,maxz=-1e9;
            for(u32 v=0;v<s->vertex_count;v++){f32 X=s->vertices[v].x,Z=s->vertices[v].y;
                if(X<minx)minx=X;
                if(X>maxx)maxx=X;
                if(Z<minz)minz=Z;
                if(Z>maxz)maxz=Z;}
            /* ray-cast point in polygon */
            bool in=false;
            for(u32 a=0,b=s->wall_count?s->walls[s->wall_count-1].v2:0;a<s->wall_count;a++){
                i32 v1=s->walls[a].v1,v2=s->walls[a].v2; (void)b;
                if(v1<0||v2<0||v1>=(i32)s->vertex_count||v2>=(i32)s->vertex_count)continue;
                f32 x0=s->vertices[v1].x,z0=s->vertices[v1].y,x1=s->vertices[v2].x,z1=s->vertices[v2].y;
                if(((z0<=qz&&qz<z1)||(z1<=qz&&qz<z0))&&qx<x0+(qz-z0)/(z1-z0)*(x1-x0))in=!in;
            }
            if(in) OL_LOG("FINDXZ sec %u name=%s floor=%.2f ceil=%.2f bbox=[%.0f,%.0f]x[%.0f,%.0f]\n",
                          i,s->name,s->floor_y,s->ceil_y,minx,maxx,minz,maxz);
        }
    }
    if (getenv("OL_INSPECT")) {
        i32 si = atoi(getenv("OL_INSPECT"));
        if (si >= 0 && si < (i32)world->lvt.sector_count) {
            LvtSector *s = &world->lvt.sectors[si];
            f32 cx=0,cz=0; for(u32 v=0;v<s->vertex_count;v++){cx+=s->vertices[v].x;cz+=s->vertices[v].y;}
            if(s->vertex_count){cx/=s->vertex_count;cz/=s->vertex_count;}
            OL_LOG("INSPECT sec %d name=%s floor=%.2f ceil=%.2f C=(%.1f,%.1f) walls=%u\n",
                   si, s->name, s->floor_y, s->ceil_y, cx, cz, s->wall_count);
            for (u32 w=0; w<s->wall_count; w++){
                LvtWall *wl=&s->walls[w];
                f32 af=-99,ac=-99; const char *an="";
                if(wl->adjoin>=0&&wl->adjoin<(i32)world->lvt.sector_count){
                    LvtSector *a=&world->lvt.sectors[wl->adjoin]; af=a->floor_y; ac=a->ceil_y; an=a->name;}
                OL_LOG("  wall %u adjoin=%d(%s f=%.2f c=%.2f) dadjoin=%d flags=0x%x flags2=0x%x\n",
                       w, wl->adjoin, an, af, ac, wl->dadjoin, wl->flags, wl->flags2);
            }
        }
    }

    /* Upload textures using level palette for accurate colour mapping */
    OL_LOG("Uploading textures for %s...\n", level_name);
    g_missing_tex = 0;
    upload_level_textures(&world->lvt, arc, r,
                          arc->palette_loaded ? arc->palette : NULL);
    world->missing_textures = g_missing_tex;
    /* Count INF sectors that never resolved to a real sector. */
    world->inf_unresolved = 0;
    for (u32 i = 0; i < world->inf.count; i++)
        if (world->inf.elevs[i].sector_name[0] &&
            world->inf.elevs[i].sector_idx == 0xFFFFFFFFu)
            world->inf_unresolved++;
    for (u32 i = 0; i < world->inf.trigger_count; i++)
        if (world->inf.triggers[i].sector_name[0] &&
            world->inf.triggers[i].sector_idx == 0xFFFFFFFFu)
            world->inf_unresolved++;

    /* Mark breakable glass windows. In Outlaws, only glass with a BREAK
     * animation shatters when shot — those are .ATX animated textures (the
     * shatter is a STOP-terminated frame sequence). Static painted windows
     * (.PCX, e.g. GWWIN2.PCX on the HIDEOUT house) are part of the wall and
     * are NOT breakable. So require a mask portal wall (flag bit 0x01) whose
     * MID texture is an animated (.ATX) window — not merely a "WIN" name. */
    {
        u32 nwin = 0;
        for (u32 si = 0; si < world->lvt.sector_count; si++) {
            LvtSector *sec = &world->lvt.sectors[si];
            for (u32 wi = 0; wi < sec->wall_count; wi++) {
                LvtWall *w = &sec->walls[wi];
                if (w->adjoin < 0 || !(w->flags & 0x01u)) continue;
                i32 t = w->mid.tex_id;
                if (t < 0 || t >= (i32)world->lvt.texture_count) continue;
                const char *tn = world->lvt.textures[t];
                const char *ext = strrchr(tn, '.');
                bool is_atx = ext && strcasecmp(ext, ".atx") == 0;
                bool has_win = strcasestr(tn, "win") != NULL;
                if (is_atx && has_win) { w->is_window = true; nwin++; }
            }
        }
        OL_LOG("Breakable glass windows: %u\n", nwin);
    }

    /* Build GPU geometry */
    OL_LOG("Building GPU geometry...\n");
    if (!renderer_build_level(r, &world->lvt, &world->inf)) {
        OL_ERR("Failed to build level geometry\n");
        return false;
    }

    /* Find the sky panorama texture the way the original engine does: the sky
     * IS the ceiling texture of a sky-flagged sector (flags bit 0; Ghidra
     * sky_DrawCeiling@0x4b6190 reads sector->ceil_tex_handle). Use the first
     * sky-ceiling sector's ceiling texture (a level's sky sectors share it).
     * Fallback: a sky-floor (pit) sector's floor texture; then archive name
     * patterns for safety. */
    {
        u32 sky_tid = 0;

        for (u32 si = 0; si < world->lvt.sector_count && !sky_tid; si++) {
            const LvtSector *s = &world->lvt.sectors[si];
            i32 tex = -1;
            if (s->flags & LVT_SEC_FLAG_SKY_CEIL)       tex = s->ceil_tex;
            else if (s->flags & LVT_SEC_FLAG_SKY_FLOOR) tex = s->floor_tex;
            if (tex >= 0 && (u32)tex < world->lvt.texture_count)
                sky_tid = renderer_find_texture(r, world->lvt.textures[tex]);
        }
        if (sky_tid) {
            OL_LOG("Sky panorama: sky-sector ceiling texture ('%s' %ux%u)\n",
                   r->textures[sky_tid - 1].name,
                   r->textures[sky_tid - 1].width, r->textures[sky_tid - 1].height);
        }

        /* Strategy 2: try common name patterns in archives */
        if (!sky_tid) {
            char sky_names[4][64];
            snprintf(sky_names[0], 64, "%sSKY.PCX", level_name);
            snprintf(sky_names[1], 64, "%ssky.pcx", level_name);
            snprintf(sky_names[2], 64, "SKY.PCX");
            snprintf(sky_names[3], 64, "sky.pcx");
            for (int si = 0; si < 4 && !sky_tid; si++) {
                u32 sky_size = 0;
                const u8 *sky_data = lab_get(&arc->tex, sky_names[si], &sky_size);
                if (!sky_data) sky_data = lab_get(&arc->geo, sky_names[si], &sky_size);
                if (!sky_data) sky_data = lab_get(&arc->main, sky_names[si], &sky_size);
                if (sky_data && sky_size > 0) {
                    u32 sw = 0, sh = 0;
                    u8 *rgba = arc->palette_loaded
                               ? pcx_decode_rgba_pal(sky_data, sky_size, &sw, &sh, arc->palette)
                               : pcx_decode_rgba(sky_data, sky_size, &sw, &sh);
                    if (rgba) {
                        flip_rgba_rows(rgba, sw, sh);
                        sky_tid = renderer_upload_texture(r, "sky_panorama", rgba, sw, sh);
                        free(rgba);
                        OL_LOG("Sky panorama loaded from archive: %ux%u\n", sw, sh);
                    }
                }
            }
        }

        renderer_set_sky(r, sky_tid, world->lvt.parallax_x, world->lvt.parallax_y);
        if (!sky_tid) OL_LOG("No sky panorama found; using clear color\n");
    }

    /* Load OBT (object placement) */
    world->player_start = (Vec3){ 0, 0, 0 };
    world->player_start_yaw = 0.0f;

    u32 obt_size = 0;
    const u8 *obt_data = archives_get(arc, obt_name, &obt_size);

    if (obt_data) {
        ObtTable obt = {0};
        if (obt_parse(&obt, (const char *)obt_data, obt_size)) {
            /* Extract player start */
            ObtObject *starts[4];
            u32 n = obt_find_by_type(&obt, "PLAYER", starts, 4);
            if (n > 0) {
                world->player_start = starts[0]->pos;
                world->player_start_yaw = starts[0]->yaw * OL_DEG2RAD;
                /* Snap player Y to the sector floor at the spawn position. Use the
                 * Y-GATED GLOBAL search (hint = -1) seeded with the object's own Y
                 * so stacked/multi-storey sectors are disambiguated correctly — a
                 * plain 2D find (or a bogus hint) can pick a lower overlapping
                 * sector and drop the player under the map (e.g. HIDEOUT). */
                {
                    f32 oy = world->player_start.y;
                    int ps = collision_find_sector_y(&world->lvt,
                                    world->player_start.x, world->player_start.z,
                                    oy, 0.0f, -1);
                    if (ps < 0)
                        ps = collision_find_sector(&world->lvt,
                                    world->player_start.x, world->player_start.z, 0);
                    if (ps >= 0 && ps < (i32)world->lvt.sector_count)
                        world->player_start.y = world->lvt.sectors[ps].floor_y;
                    OL_LOG("Player start: (%.1f, obt_y=%.1f -> floor=%.1f, %.1f) "
                           "sector=%d yaw=%.1f°\n",
                           world->player_start.x, oy, world->player_start.y,
                           world->player_start.z, ps, starts[0]->yaw);
                }
            }

            /* Load all entities */
            for (u32 i = 0; i < obt.object_count; i++) {
                int idx = entity_add(&world->entities, &obt.objects[i]);
                if (idx < 0) continue;

                Entity *e = &world->entities.entities[idx];

                /* Try to load directional sprite textures for this entity type */
                f32 nwx_w = 0, nwx_h = 0;
                u32 tex = load_entity_sprite_dirs(e->type_name, arc, r,
                                                   e->sprite_dir_tex, &nwx_w, &nwx_h,
                                                   e->anim_tex, &e->anim_count,
                                                   &e->anim_dt_ms);
                if (!tex) {
                    /* Try name variants by progressively stripping suffix */
                    char variant[64];
                    snprintf(variant, sizeof(variant), "%s", e->type_name);
                    int vl = (int)strlen(variant);
                    while (vl > 1 && !tex) {
                        variant[--vl] = '\0';
                        tex = load_entity_sprite_dirs(variant, arc, r,
                                                       e->sprite_dir_tex, &nwx_w, &nwx_h,
                                                       e->anim_tex, &e->anim_count,
                                                       &e->anim_dt_ms);
                    }
                }
                e->sprite_tex = tex;
                /* Use NWX-derived world-space dimensions for rendering */
                if (nwx_w > 0 && nwx_h > 0) {
                    e->render_w = nwx_w;
                    e->render_h = nwx_h;
                }

                /* Inv_Object scenery (bottles, lanterns, pots...): chor-driven
                 * state machine — spawns in chor 0, breaks/reacts on hit or
                 * nudge per its ITM TYPE. Replaces the cyclic-cells fallback
                 * (which wrongly looped break animations). */
                if (e->kind == ENTITY_DECORATION || e->kind == ENTITY_NONE)
                    load_scenery(e, arc, r);

                /* Load per-AI-state animation sequences for enemies */
                if (e->kind == ENTITY_ENEMY) {
                    load_entity_anim_seqs(e->type_name, arc, r, e);
                    /* Also try stripped variant if primary load found nothing */
                    if (!e->has_anim_seqs) {
                        char variant2[64];
                        snprintf(variant2, sizeof(variant2), "%s", e->type_name);
                        int vl2 = (int)strlen(variant2);
                        while (vl2 > 1 && !e->has_anim_seqs) {
                            variant2[--vl2] = '\0';
                            load_entity_anim_seqs(variant2, arc, r, e);
                        }
                    }
                    /* Initialize animation state */
                    e->cur_anim = ANIM_IDLE;
                    e->cur_anim_frame = 0;
                    e->cur_anim_timer = 0.0f;
                    e->anim_loop = true;
                }
            }

            OL_LOG("Loaded %u entities\n", world->entities.count);
            obt_free(&obt);
        }
    }

    /* Mission / boss objective (Sanchez in TOWN). */
    mission_init(&world->mission, &world->entities);
    if (world->mission.has_boss)
        OL_LOG("Mission: boss level, %d regular enemies\n",
               world->mission.total_enemies);

    /* Fallback player start: center of first sector */
    if (world->player_start.x == 0.0f && world->player_start.z == 0.0f &&
        world->lvt.sector_count > 0) {
        const LvtSector *s = &world->lvt.sectors[0];
        f32 ax = 0, az = 0;
        for (u32 i = 0; i < s->vertex_count; i++) {
            ax += s->vertices[i].x;
            az += s->vertices[i].y;
        }
        if (s->vertex_count > 0) { ax /= s->vertex_count; az /= s->vertex_count; }
        world->player_start = (Vec3){ ax, s->floor_y + 1.0f, az };
    }

    world->loaded = true;
    OL_LOG("Level '%s' loaded (%u sectors, %u entities)\n",
           level_name, world->lvt.sector_count, world->entities.count);
    return true;
}

void world_free(World *world) {
    if (!world->loaded) return;
    lvt_free(&world->lvt);
    entity_list_clear(&world->entities);
    world->loaded = false;
}
