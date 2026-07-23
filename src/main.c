/*
 * main.c - Outlaws engine entry point
 *
 * Usage: outlaws [--data <path>] [--level <name>]
 *
 * Defaults:
 *   --data  ./data/
 *   --level CANYON
 */
#include "engine.h"
#include "renderer.h"
#include "input.h"
#include "player.h"
#include "world.h"
#include "pcx.h"
#include "collision.h"
#include "entity.h"
#include "weapon.h"
#include "audio.h"
#include "inf.h"
#include "wax.h"
#include "debug_ui.h"
#include "menu.h"
#include "settings.h"
#include "savegame.h"
#include "projectile.h"
#include "tdo.h"
#include "smush.h"

#include <SDL2/SDL.h>
#include <math.h>
#include <ctype.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------- */
#define DEFAULT_WIDTH   1280
#define DEFAULT_HEIGHT  720
#define DEFAULT_FOV     90.0f
#define DEFAULT_DATA    "data"
#define DEFAULT_LEVEL   "CANYON"
#define TARGET_FPS      60
#define FRAME_TIME_MS   (1000 / TARGET_FPS)

/* Respawn delay (seconds) */
#define RESPAWN_DELAY   3.0f

/* Window shatter animation length (~6 break frames @ ~12fps + margin). */
#define WINDOW_BREAK_DURATION 0.6f

/* -------------------------------------------------------------------------
 * Application state
 * ---------------------------------------------------------------------- */
typedef struct {
    Renderer    renderer;
    InputState  input;
    Player      player;
    World       world;
    AudioSystem audio;
    Archives    archives;

    char  data_dir[512];
    char  level_name[64];
    bool  running;

    /* Headless screenshot capture: run N frames, save a PNG, then exit. */
    char  screenshot_path[512];
    bool  check_mode;            /* --check: load level, print health line, exit */
    bool  check_doors_mode;      /* --check-doors: test every door, exit */
    int   screenshot_frames;   /* 0 = disabled */
    bool  screenshot_firing;   /* hold primary fire while capturing */
    int   screenshot_weapon;   /* weapon slot to give/select for capture */
    bool  screenshot_weapon_set;
    bool  screenshot_near_enemy; /* teleport player in front of nearest enemy */
    bool  near_enemy_done;
    bool  force_open_doors;      /* debug: force all morph doors open */
    bool  spawn_set;             /* debug: spawn at fixed coords */
    f32   spawn_x, spawn_z, spawn_yaw_deg;
    bool  spawn_done;
    bool  force_crouch;          /* debug: hold crouch */
    bool  force_use;             /* debug: press E every frame */
    bool  force_walk;            /* debug: walk forward every frame */
    bool  give_all_keys;         /* debug: grant every key/tool at spawn */
    f32   spawn_y;               /* debug: explicit spawn Y (upper floors) */
    bool  spawn_y_set;
    bool  show_map;              /* automap overlay (TAB) */
    Menu  menu;                  /* front-end menu system */
    bool  in_menu;               /* true while the front-end menu is showing */
    bool  level_from_cli;        /* --level given → skip menu */
    bool  force_menu;            /* --menu → always start at the menu */
    char  cutscene_cli[64];      /* --cutscene <name>: play one .SAN and exit */

    /* Story campaign progression + in-game pause. */
    char  campaign[16][64];      /* level order parsed from OUTLAWS.RCS */
    char  campaign_movie[16][32];/* MOVIE/CREDITS .SAN played AFTER campaign[i] ("" = none) */
    char  campaign_open_movie[32];/* opening cinematic (op_cr.san) at story start */
    int   campaign_count;
    bool  campaign_active;       /* STORY: auto-advance through the level order */
    int   campaign_idx;          /* current index into campaign[] */
    bool  paused;                /* in-game pause overlay active */
    bool  level_done;            /* level complete → auto-advance pending */
    f32   level_done_timer;      /* seconds left on the "MISSION COMPLETE" card */

    /* For player_hurt callback (damage from enemies) */
    i32   pending_player_damage;

    /* INF scripting: previous player sector (for enter/leave triggers) */
    i32   prev_sector;
    /* On-screen message (USER_MSG / system): text + remaining seconds. */
    char  message_text[128];
    f32   message_timer;

    /* Landing detection for LAND.WAV */
    bool  was_airborne;
    bool  in_water_prev;   /* was the player swimming last frame (splash on entry) */
    f32   window_anim_timer; /* >0 while a shot window plays its shatter frames */

    /* Previous weapon for switch-sound detection */
    WeaponType prev_weapon;

    /* Sound effect IDs (0 = not loaded) */
    u32   sfx_weapon[WEAPON_COUNT];  /* Weapon fire sounds */
    u32   sfx_weapon_alt[WEAPON_COUNT]; /* FIRE_SOUND_2 (fan, jab, throw) */
    u32   sfx_dry[WEAPON_COUNT];     /* NO_AMMO click sounds */
    u32   sfx_reload_w[WEAPON_COUNT];/* RELOAD_SOUND per weapon */
    u32   sfx_explode, sfx_fuse;     /* Dynamite */

    /* Thrown projectiles (knife / dynamite) + explosion FX */
    ProjectileSystem projectiles;
    u32   tex_knife_proj, tex_tnt_proj;
    f32   knife_pw, knife_ph, tnt_pw, tnt_ph;
    u32   fx_boom[16]; u32 fx_boom_n; f32 fx_boom_dt, boom_w, boom_h;
    int   tdo_knife, tdo_tnt;        /* 3DO models for thrown projectiles */
    bool  lmb_prev, rmb_prev;        /* Fire button edge detection */
    u32   sfx_player_hurt;           /* Player takes damage */
    u32   sfx_player_land;           /* Player lands from jump */
    u32   sfx_enemy_shot;            /* Enemy fires at player */
    u32   sfx_pickup;                /* Pick up an item */
    u32   sfx_weapon_switch;         /* Switch weapon */
    u32   sfx_bgy_hit[10];           /* BGY0-9 hit sounds */
    u32   sfx_bgy_die[10];           /* BGY0-9 die sounds */
    u32   sfx_fire;                  /* Ambient fire loop sound */
    u32   sfx_drip;                  /* Ambient drip loop sound */
    u32   sfx_water;                 /* Ambient water loop sound */
    u32   sfx_wind;                  /* Ambient wind loop sound */
    u32   sfx_windtunl;              /* Wind tunnel loop */
    u32   sfx_vulture;               /* Vulture loop */
    u32   sfx_glass_break;           /* Glass window shatter */
    u32   sfx_locked;                /* Locked door (no key) */
    u32   sfx_unlock;                /* Door unlocked with key */

    int   difficulty;                /* 1..4 (Easy/Medium/Hard/Hardest); default 2 */
    u32   loading_bg_tex;            /* MM220.PCX loading-screen background */
} App;

static App g_app;

/* Called by entity AI when an enemy shoots the player */
/* Build a newline-separated list of held inventory items for the HUD readout.
 * Returns NULL when the player carries nothing. */
static const char *build_inventory_text(int keys) {
    static char buf[256];
    struct { int bit; const char *name; } items[] = {
        { 1 << INF_KEY_STEEL,   "STEEL KEY" },
        { 1 << INF_KEY_IRON,    "IRON KEY" },
        { 1 << INF_KEY_BRASS,   "BRASS KEY" },
        { 1 << INF_KEY_ROUND,   "ROUND STONE KEY" },
        { 1 << INF_KEY_SQUARE,  "SQUARE STONE KEY" },
        { 1 << INF_KEY_CROWBAR, "CROWBAR" },
        { 1 << INF_KEY_SHOVEL,  "SHOVEL" },
        { 1 << INF_KEY_BADGE,   "SHERIFFS BADGE" },
    };
    int n = 0;
    for (int i = 0; i < 8; i++) {
        if ((keys & items[i].bit) && n < (int)sizeof(buf) - 1)
            n += snprintf(buf + n, sizeof(buf) - n, "%s%s",
                          n ? "\n" : "", items[i].name);
    }
    return (n > 0) ? buf : NULL;
}

/* Top-left HUD text: current objective (if any) followed by held inventory. */
static const char *build_hud_left_text(const char *objective, int keys) {
    static char buf[384];
    const char *inv = build_inventory_text(keys);
    int n = 0;
    if (objective && objective[0])
        n += snprintf(buf + n, sizeof(buf) - n, "OBJECTIVE: %s", objective);
    if (inv)
        n += snprintf(buf + n, sizeof(buf) - n, "%s%s", n ? "\n\n" : "", inv);
    return (n > 0) ? buf : NULL;
}

static void on_player_hurt(i32 damage) {
    g_app.pending_player_damage += damage;
    audio_play(&g_app.audio, g_app.sfx_player_hurt);
    audio_play(&g_app.audio, g_app.sfx_enemy_shot);
}

/* -------------------------------------------------------------------------
 * HUD asset loading (first-person weapon sprites, face portrait)
 * ---------------------------------------------------------------------- */

/* James (player) weapon NWX names in olweap.lab, indexed by WeaponType */
static const char *WEAPON_NWX_NAMES[WEAPON_COUNT] = {
    "ijfist.nwx",      /* WEAPON_FIST */
    "ijpistol.nwx",    /* WEAPON_PISTOL */
    "ijrifle.nwx",     /* WEAPON_RIFLE */
    "ijshtgun.nwx",    /* WEAPON_SHOTGUN */
    "ijdbsgun.nwx",    /* WEAPON_DBL_SHOTGUN */
    "ijsawgun.nwx",    /* WEAPON_SAW_GUN */
    "ijdynam.nwx",     /* WEAPON_DYNAMITE */
    "ijknife.nwx",     /* WEAPON_KNIFE */
    "igatgun.nwx",     /* WEAPON_GATLING */
};

static u32 load_nwx_as_texture(Renderer *r, const Archives *arc,
                                const LabArchive *lab, const char *name,
                                const char *tex_name) {
    u32 sz = 0;
    const u8 *data = lab_get(lab, name, &sz);
    if (!data) return 0;

    WaxSprite sprite;
    /* HUD/UI sprites are authored against the master OLPAL palette. */
    if (!wax_decode(&sprite, data, sz, arc->hud_palette)) return 0;

    u32 tex = 0;
    if (sprite.cell_count > 0) {
        /* Use the CHOT-indicated idle cell (dir_cell[0] = front-facing idle),
         * NOT cell 0 which may be a different animation frame (e.g. pistol
         * cell 0 is a 189x76 muzzle flash mask filled with palette index 3). */
        u32 idle_idx = sprite.dir_cell[0];
        if (idle_idx >= sprite.cell_count) idle_idx = 0;
        const WaxCell *cell = &sprite.cells[idle_idx];
        if (cell->pixels && cell->width > 0 && cell->height > 0)
            tex = renderer_upload_texture(r, tex_name, cell->pixels,
                                          cell->width, cell->height);
    }
    wax_free(&sprite);
    return tex;
}

static void upload_cell(Renderer *r, const WaxSprite *sp, u32 cidx,
                         const char *name, u32 *out) {
    *out = 0;
    if (cidx < sp->cell_count) {
        const WaxCell *c = &sp->cells[cidx];
        if (c->pixels && c->width > 0 && c->height > 0)
            *out = renderer_upload_texture(r, name, c->pixels, c->width, c->height);
    }
}

/* Upload one animation state (a CHOT choreography) of a weapon sprite into the
 * given renderer frame arrays. Resolves the chor's direction-0 token stream to
 * an ordered list of FRMT display frames, uploads each frame's cell (with its
 * anchor offset and pixel size), and records per-frame durations.
 * Returns the number of frames uploaded. */
static u32 upload_weapon_chor(Renderer *r, const WaxSprite *sp, int wi,
                              const char *tag, i32 chor,
                              u32 *frames_out, u32 *count_out, u32 *dt_out,
                              i32 *ox_out, i32 *oy_out, u32 *w_out, u32 *h_out,
                              u8 *trans_out) {
    *count_out = 0;
    if (chor < 0 || (u32)chor >= sp->chor_count) return 0;

    u32 fidx[WAX_MAX_SEQ], fdt[WAX_MAX_SEQ];
    u32 n = wax_chor_frames(sp, (u32)chor, fidx, fdt, WAX_MAX_SEQ);
    if (n == 0) return 0;
    if (n > WEAPON_MAX_ANIM_FRAMES) n = WEAPON_MAX_ANIM_FRAMES;

    u32 out = 0;
    for (u32 f = 0; f < n; f++) {
        const WaxFrame *fr = &sp->frames[fidx[f]];
        u32 cidx = fr->cell_idx;
        if (cidx >= sp->cell_count) continue;
        const WaxCell *cell = &sp->cells[cidx];
        if (!cell->pixels || cell->width == 0 || cell->height == 0) continue;

        char tn[80];
        snprintf(tn, sizeof(tn), "hud_weap_%d_%s_%u", wi, tag, out);
        u32 tid = renderer_find_texture(r, tn);
        if (!tid)
            tid = renderer_upload_texture(r, tn, cell->pixels,
                                          cell->width, cell->height);
        frames_out[out] = tid;
        dt_out[out]     = fdt[f];
        ox_out[out]     = fr->off_x;
        oy_out[out]     = fr->off_y;
        w_out[out]      = cell->width;
        h_out[out]      = cell->height;
        if (trans_out)
            trans_out[out] = (cell->flags & WAX_CELL_TRANSLUCENT) ? 1 : 0;
        out++;
    }
    *count_out = out;
    return out;
}

static void load_hud_assets(Renderer *r, const Archives *arc) {
    /* First-person weapon sprites.
     *
     * Ghidra RE of the WAX system (wax_LoadCostume / wax_NextFrame) and the ITM
     * loader shows the ITM *_CHOR fields are CHOT choreography indices, NOT cell
     * indices. Each choreography's direction-0 token stream lists FRMT frame
     * indices to display in order (opcodes 0xFFxx mark loop/stop/events). The
     * idle pose is REST_CHOR, the fire animation is FIRE_CHOR_1/2, reload is
     * RELOAD_CHOR. Cartridge icons come from the weapon's own choreographies 11
     * (chor index) — a bullet/shell sprite baked into the weapon NWX.
     *
     * These sprites are authored against OLPAL.PCX (arc->hud_palette), not the
     * per-level palette. */
    {
        /* Ammo cartridge choreography index inside each weapon NWX.
         * (HUD_WeaponChange builds cartridge widgets from weapon chor 11.) */
        const int AMMO_CHOR = 11;

        for (int i = 0; i < WEAPON_COUNT; i++) {
            r->weapon_hud_tex[i] = 0;
            r->weapon_fire_frame_count[i] = 0;
            r->weapon_fire2_frame_count[i] = 0;
            r->weapon_reload_frame_count[i] = 0;
            r->weapon_ammo_tex[i] = 0;
            if (!WEAPON_NWX_NAMES[i]) continue;

            u32 sz = 0;
            const u8 *data = lab_get(&arc->weap, WEAPON_NWX_NAMES[i], &sz);
            if (!data) continue;

            WaxSprite sprite;
            if (!wax_decode(&sprite, data, sz, arc->hud_palette)) continue;

            const WeaponDef *def = &g_weapon_defs[i];
            char tn[80];

            /* Idle: first display frame of REST_CHOR (choreography rest_chor). */
            {
                u32 fidx[WAX_MAX_SEQ], fdt[WAX_MAX_SEQ];
                u32 n = wax_chor_frames(&sprite, (u32)def->rest_chor,
                                        fidx, fdt, WAX_MAX_SEQ);
                u32 idle_cidx;
                i32 idle_ox = 0, idle_oy = 0;
                if (n > 0) {
                    const WaxFrame *fr = &sprite.frames[fidx[0]];
                    idle_cidx = fr->cell_idx;
                    idle_ox = fr->off_x; idle_oy = fr->off_y;
                } else {
                    idle_cidx = sprite.dir_cell[0];
                }
                if (idle_cidx >= sprite.cell_count) idle_cidx = 0;
                snprintf(tn, sizeof(tn), "hud_weap_%d", i);
                upload_cell(r, &sprite, idle_cidx, tn, &r->weapon_hud_tex[i]);
                r->weapon_idle_ox[i] = idle_ox;
                r->weapon_idle_oy[i] = idle_oy;
                if (idle_cidx < sprite.cell_count) {
                    r->weapon_idle_w[i] = sprite.cells[idle_cidx].width;
                    r->weapon_idle_h[i] = sprite.cells[idle_cidx].height;
                }
            }

            /* Primary fire animation (FIRE_CHOR_1). */
            upload_weapon_chor(r, &sprite, i, "fire", def->fire_chor_1,
                               r->weapon_fire_frames[i], &r->weapon_fire_frame_count[i],
                               r->weapon_fire_dt[i], r->weapon_fire_ox[i],
                               r->weapon_fire_oy[i], r->weapon_fire_w[i],
                               r->weapon_fire_h[i], r->weapon_fire_trans[i]);

            /* Alt fire animation (FIRE_CHOR_2). */
            if (def->fire_chor_2 != def->fire_chor_1)
                upload_weapon_chor(r, &sprite, i, "fire2", def->fire_chor_2,
                                   r->weapon_fire2_frames[i], &r->weapon_fire2_frame_count[i],
                                   r->weapon_fire2_dt[i], r->weapon_fire2_ox[i],
                                   r->weapon_fire2_oy[i], r->weapon_fire2_w[i],
                                   r->weapon_fire2_h[i], r->weapon_fire2_trans[i]);

            /* Reload animation (RELOAD_CHOR). */
            if (def->reload_chor > 0)
                upload_weapon_chor(r, &sprite, i, "reload", def->reload_chor,
                                   r->weapon_reload_frames[i], &r->weapon_reload_frame_count[i],
                                   r->weapon_reload_dt[i], r->weapon_reload_ox[i],
                                   r->weapon_reload_oy[i], r->weapon_reload_w[i],
                                   r->weapon_reload_h[i], NULL);

            /* Ammo cartridge icon: first display frame of choreography 11. */
            {
                u32 fidx[WAX_MAX_SEQ];
                u32 n = wax_chor_frames(&sprite, AMMO_CHOR, fidx, NULL, WAX_MAX_SEQ);
                if (n > 0) {
                    u32 cidx = sprite.frames[fidx[0]].cell_idx;
                    snprintf(tn, sizeof(tn), "hud_weap_%d_ammo", i);
                    upload_cell(r, &sprite, cidx, tn, &r->weapon_ammo_tex[i]);
                }
            }

            OL_LOG("HUD weapon[%d]: %s chors=%u idle_ox=%d fire=%u reload=%u ammo=%s\n",
                   i, WEAPON_NWX_NAMES[i], sprite.chor_count, r->weapon_idle_ox[i],
                   r->weapon_fire_frame_count[i], r->weapon_reload_frame_count[i],
                   r->weapon_ammo_tex[i] ? "yes" : "no");

            wax_free(&sprite);
        }
    }

    /* Player face portrait - ATIM.NWX = James's face for status bar */
    r->face_hud_tex = load_nwx_as_texture(
        r, arc, &arc->obj, "ATIM.NWX", "hud_face");
    if (!r->face_hud_tex)
        r->face_hud_tex = load_nwx_as_texture(
            r, arc, &arc->obj, "atim.nwx", "hud_face");
    if (r->face_hud_tex)
        OL_LOG("HUD face portrait loaded\n");

    /* HUD panel background: INTERFAC.NWX cell 0 */
    r->hud_panel_tex = load_nwx_as_texture(
        r, arc, &arc->obj, "INTERFAC.NWX", "hud_panel");
    if (!r->hud_panel_tex)
        r->hud_panel_tex = load_nwx_as_texture(
            r, arc, &arc->obj, "interfac.nwx", "hud_panel");
    if (r->hud_panel_tex)
        OL_LOG("HUD panel (INTERFAC) loaded\n");

    /* Heart icon: INTHEART.NWX cell 0 */
    r->hud_heart_tex = load_nwx_as_texture(
        r, arc, &arc->obj, "INTHEART.NWX", "hud_heart");
    if (!r->hud_heart_tex)
        r->hud_heart_tex = load_nwx_as_texture(
            r, arc, &arc->obj, "intheart.nwx", "hud_heart");
    if (r->hud_heart_tex)
        OL_LOG("HUD heart icon loaded\n");

    /* Energy indicator: INTNERGY.NWX - load all cells (22 health states) */
    {
        u32 sz = 0;
        const u8 *ndata = lab_get(&arc->obj, "INTNERGY.NWX", &sz);
        if (!ndata) ndata = lab_get(&arc->obj, "intnergy.nwx", &sz);
        if (ndata && sz > 0) {
            WaxSprite nergy;
            if (wax_decode(&nergy, ndata, sz, arc->hud_palette)) {
                u32 loaded = 0;
                for (u32 c = 0; c < nergy.cell_count && c < 32; c++) {
                    const WaxCell *cell = &nergy.cells[c];
                    if (cell->pixels && cell->width > 0 && cell->height > 0) {
                        char tname[24];
                        snprintf(tname, sizeof(tname), "hud_nergy_%u", c);
                        r->hud_energy_cells[c] = renderer_upload_texture(
                            r, tname, cell->pixels, cell->width, cell->height);
                        if (r->hud_energy_cells[c]) loaded++;
                    }
                }
                r->hud_energy_cell_count = nergy.cell_count;
                wax_free(&nergy);
                OL_LOG("HUD energy cells loaded: %u\n", loaded);
            }
        } else {
            OL_WARN("INTNERGY.NWX not found\n");
        }
    }

    /* Number digit textures from NUMBERS.NWX
     * Cells 0-9 = digits '0'-'9', each 34x66 pixels */
    {
        u32 sz = 0;
        const u8 *data = lab_get(&arc->obj, "NUMBERS.NWX", &sz);
        if (!data) data = lab_get(&arc->obj, "numbers.nwx", &sz);
        if (data && sz > 0) {
            WaxSprite nums;
            if (wax_decode(&nums, data, sz, arc->hud_palette)) {
                for (int d = 0; d < 10 && d < (int)nums.cell_count; d++) {
                    const WaxCell *cell = &nums.cells[d];
                    if (cell->pixels && cell->width > 0 && cell->height > 0) {
                        char tname[16];
                        snprintf(tname, sizeof(tname), "hud_num_%d", d);
                        r->digit_tex[d] = renderer_upload_texture(
                            r, tname, cell->pixels, cell->width, cell->height);
                    }
                }
                u32 dw = (nums.cell_count > 0) ? nums.cells[0].width  : 0;
                u32 dh = (nums.cell_count > 0) ? nums.cells[0].height : 0;
                wax_free(&nums);
                OL_LOG("HUD digit textures loaded (%ux%u)\n", dw, dh);
            }
        } else {
            OL_WARN("NUMBERS.NWX not found (digit HUD unavailable)\n");
        }
    }

    /* Ammo cartridge icons are loaded per weapon above (weapon NWX chor 11). */
}

/* -------------------------------------------------------------------------
 * Sound effect loading
 * ---------------------------------------------------------------------- */

/* Try to load a WAV from olsfx.lab by name; return sound ID or 0 */
static u32 sfx_load(const char *name) {
    if (!g_app.audio.initialized) return 0;
    u32 sz = 0;
    const u8 *data = lab_get(&g_app.archives.sfx, name, &sz);
    if (!data || sz == 0) return 0;
    u32 id = audio_load_wav(&g_app.audio, name, data, sz);
    if (id) OL_LOG("SFX loaded: %s\n", name);
    return id;
}

/* sounds.lst (outlaws.lab): positional sound table for scenery chor
 * PLAYSOUND events (Ghidra: loaded @0x429bd0 into DAT_00531480; chor opcode
 * 0xFFFD carries an index into it). Format: header lines then
 * "NUM  PRIORITY  NAME" rows. */
#define MAX_LST_SOUNDS 64
static u32 g_lst_sounds[MAX_LST_SOUNDS];

static void load_sounds_lst(void) {
    u32 sz = 0;
    const u8 *data = archives_get(&g_app.archives, "sounds.lst", &sz);
    if (!data || !sz) return;
    char buf[4096];
    u32 n = (sz < sizeof(buf) - 1) ? sz : sizeof(buf) - 1;
    memcpy(buf, data, n); buf[n] = '\0';
    char *save = NULL;
    for (char *line = strtok_r(buf, "\r\n", &save); line;
         line = strtok_r(NULL, "\r\n", &save)) {
        int idx; char prio[8], wav[64];
        if (sscanf(line, " %d %7s %63s", &idx, prio, wav) == 3 &&
            idx >= 0 && idx < MAX_LST_SOUNDS && strstr(wav, "."))
            g_lst_sounds[idx] = sfx_load(wav);
    }
}

/* Scenery chor sound hook (entity_set_sfx_callback). */
static void scenery_play_sfx(i32 sounds_lst_idx, Vec3 pos) {
    (void)pos; /* TODO: positional attenuation */
    if (sounds_lst_idx >= 0 && sounds_lst_idx < MAX_LST_SOUNDS &&
        g_lst_sounds[sounds_lst_idx])
        audio_play(&g_app.audio, g_lst_sounds[sounds_lst_idx]);
}

static void load_sfx(void) {
    /* Weapon fire sounds (ITM FIRE_SOUND_1/2 names, from olsfx.lab):
     * fist: ACTION throw(whoosh) + FIRE connect(on hit only);
     * sawed: FIRE_SOUND_1 sawed, _2 double; TNT: _1 null, _2 throw;
     * knife: _1 throw, _2 jab. */
    g_app.sfx_weapon[WEAPON_FIST]        = sfx_load("THROW.WAV");
    g_app.sfx_weapon[WEAPON_PISTOL]      = sfx_load("PISTOL.WAV");
    g_app.sfx_weapon[WEAPON_RIFLE]       = sfx_load("RIFLE.WAV");
    g_app.sfx_weapon[WEAPON_SHOTGUN]     = sfx_load("SINGLE.WAV");
    g_app.sfx_weapon[WEAPON_DBL_SHOTGUN] = sfx_load("DOUBLE.WAV");
    g_app.sfx_weapon[WEAPON_SAW_GUN]     = sfx_load("SAWED.WAV");
    g_app.sfx_weapon[WEAPON_DYNAMITE]    = 0;             /* FIRE_SOUND_1 null */
    g_app.sfx_weapon[WEAPON_KNIFE]       = sfx_load("THROW.WAV");
    g_app.sfx_weapon[WEAPON_GATLING]     = sfx_load("GatlShot.WAV");

    g_app.sfx_weapon_alt[WEAPON_FIST]        = sfx_load("connect.wav");
    g_app.sfx_weapon_alt[WEAPON_PISTOL]      = g_app.sfx_weapon[WEAPON_PISTOL];
    g_app.sfx_weapon_alt[WEAPON_RIFLE]       = g_app.sfx_weapon[WEAPON_RIFLE];
    g_app.sfx_weapon_alt[WEAPON_SHOTGUN]     = g_app.sfx_weapon[WEAPON_SHOTGUN];
    g_app.sfx_weapon_alt[WEAPON_DBL_SHOTGUN] = g_app.sfx_weapon[WEAPON_DBL_SHOTGUN];
    g_app.sfx_weapon_alt[WEAPON_SAW_GUN]     = sfx_load("DOUBLE.WAV");
    g_app.sfx_weapon_alt[WEAPON_DYNAMITE]    = sfx_load("THROW.WAV");
    g_app.sfx_weapon_alt[WEAPON_KNIFE]       = sfx_load("jab.wav");
    g_app.sfx_weapon_alt[WEAPON_GATLING]     = g_app.sfx_weapon[WEAPON_GATLING];

    /* Dry-fire clicks (NO_AMMO_SOUND) and per-round reload sounds */
    g_app.sfx_dry[WEAPON_PISTOL]       = sfx_load("pistout.wav");
    g_app.sfx_dry[WEAPON_RIFLE]        = sfx_load("rifleout.wav");
    g_app.sfx_dry[WEAPON_SHOTGUN]      = sfx_load("shotout.wav");
    g_app.sfx_dry[WEAPON_DBL_SHOTGUN]  = g_app.sfx_dry[WEAPON_SHOTGUN];
    g_app.sfx_dry[WEAPON_SAW_GUN]      = g_app.sfx_dry[WEAPON_SHOTGUN];
    g_app.sfx_dry[WEAPON_GATLING]      = sfx_load("gatLTAIL.wav");
    g_app.sfx_reload_w[WEAPON_PISTOL]      = sfx_load("preload.wav");
    g_app.sfx_reload_w[WEAPON_RIFLE]       = sfx_load("rreload.wav");
    g_app.sfx_reload_w[WEAPON_SHOTGUN]     = sfx_load("sreload.wav");
    g_app.sfx_reload_w[WEAPON_DBL_SHOTGUN] = g_app.sfx_reload_w[WEAPON_SHOTGUN];
    g_app.sfx_reload_w[WEAPON_SAW_GUN]     = g_app.sfx_reload_w[WEAPON_SHOTGUN];
    g_app.sfx_reload_w[WEAPON_GATLING]     = sfx_load("weapgrab.WAV");

    g_app.sfx_explode = sfx_load("explode.wav");
    g_app.sfx_fuse    = sfx_load("fuse.wav");
}

static void proj_on_explode(Vec3 pos);
static void proj_play_sfx(const char *name, Vec3 pos);

/* Upload the direction-0 cell of an NWX as a texture (projectile billboards).
 * 0.0583 = NWX pixel → world unit (calibrated, see world.c). */
static u32 load_wax_cell_tex(const char *nwx_name, const char *tex_name,
                             f32 *out_w, f32 *out_h) {
    u32 sz = 0;
    const u8 *data = archives_get(&g_app.archives, nwx_name, &sz);
    if (!data || !sz) return 0;
    static WaxSprite sp;
    if (!wax_decode(&sp, data, sz,
                    g_app.archives.palette_loaded ? g_app.archives.palette : NULL))
        return 0;
    u32 tid = 0;
    u32 ci = sp.dir_cell[0];
    if (ci < sp.cell_count && sp.cells[ci].pixels) {
        tid = renderer_upload_texture(&g_app.renderer, tex_name,
                                      sp.cells[ci].pixels,
                                      sp.cells[ci].width, sp.cells[ci].height);
        if (out_w) *out_w = sp.cells[ci].width  * 0.0583f;
        if (out_h) *out_h = sp.cells[ci].height * 0.0583f;
    }
    wax_free(&sp);
    return tid;
}

/* Projectile visuals: knife/dynamite billboards + TNTBOOM explosion frames.
 * The world-object 3DOs (gknife/gdynam) are untextured models; we use the
 * weapon NWX ammo-cartridge cells (chor 11) already loaded by the renderer —
 * a bare dynamite stick / knife image — as billboards. */
static void load_projectile_assets(void) {
    Renderer *r = &g_app.renderer;

    g_app.tex_knife_proj = r->weapon_ammo_tex[WEAPON_KNIFE];
    if (!g_app.tex_knife_proj)
        g_app.tex_knife_proj = load_wax_cell_tex("ijknife.nwx", "proj_knife",
                                                 &g_app.knife_pw, &g_app.knife_ph);
    if (g_app.tex_knife_proj && g_app.tex_knife_proj <= r->texture_count) {
        g_app.knife_pw = r->textures[g_app.tex_knife_proj-1].width  * 0.0583f;
        g_app.knife_ph = r->textures[g_app.tex_knife_proj-1].height * 0.0583f;
    }
    if (g_app.knife_pw > 2.5f) { g_app.knife_ph *= 2.5f / g_app.knife_pw; g_app.knife_pw = 2.5f; }

    g_app.tex_tnt_proj = r->weapon_ammo_tex[WEAPON_DYNAMITE];
    if (!g_app.tex_tnt_proj)
        g_app.tex_tnt_proj = load_wax_cell_tex("ijdynam.nwx", "proj_tnt",
                                               &g_app.tnt_pw, &g_app.tnt_ph);
    if (g_app.tex_tnt_proj && g_app.tex_tnt_proj <= r->texture_count) {
        g_app.tnt_pw = r->textures[g_app.tex_tnt_proj-1].width  * 0.0583f;
        g_app.tnt_ph = r->textures[g_app.tex_tnt_proj-1].height * 0.0583f;
    }
    if (g_app.tnt_pw > 2.0f) { g_app.tnt_ph *= 2.0f / g_app.tnt_pw; g_app.tnt_pw = 2.0f; }
    OL_LOG("Projectile sprites: knife tex=%u (%.1fx%.1f) tnt tex=%u (%.1fx%.1f)\n",
           g_app.tex_knife_proj, g_app.knife_pw, g_app.knife_ph,
           g_app.tex_tnt_proj, g_app.tnt_pw, g_app.tnt_ph);

    /* Ground-object 3DO models (the original renders thrown knives and
     * dynamite as flat-shaded palette-colored models — gknife/gdynam.3do). */
    if (g_app.tdo_knife == 0 && g_app.tdo_tnt == 0) {   /* load once */
        g_app.tdo_knife = g_app.tdo_tnt = -1;
        const char *names[2] = { "gknife.3do", "gdynam.3do" };
        int *slots[2] = { &g_app.tdo_knife, &g_app.tdo_tnt };
        for (int i = 0; i < 2; i++) {
            u32 sz = 0;
            const u8 *data = archives_get(&g_app.archives, names[i], &sz);
            if (!data || !sz) continue;
            static TdoModel model;
            if (tdo_parse(&model, (const char *)data, sz)) {
                *slots[i] = renderer_upload_tdo(&g_app.renderer, &model,
                    g_app.archives.palette_loaded ? g_app.archives.palette : NULL);
            }
        }
        OL_LOG("Projectile 3DOs: knife=%d tnt=%d\n", g_app.tdo_knife, g_app.tdo_tnt);
    }

    /* TNTBOOM.NWX (olobj.lab): explosion animation frames */
    u32 sz = 0;
    const u8 *data = archives_get(&g_app.archives, "TNTBOOM.NWX", &sz);
    if (data && sz) {
        static WaxSprite sp;
        if (wax_decode(&sp, data, sz,
                       g_app.archives.palette_loaded ? g_app.archives.palette : NULL)) {
            g_app.fx_boom_n = 0;
            for (u32 c = 0; c < sp.cell_count && g_app.fx_boom_n < 16; c++) {
                if (!sp.cells[c].pixels) continue;
                char tn[32]; snprintf(tn, sizeof(tn), "fx_boom_%u", c);
                u32 t = renderer_upload_texture(&g_app.renderer, tn,
                                                sp.cells[c].pixels,
                                                sp.cells[c].width, sp.cells[c].height);
                if (t) {
                    if (g_app.fx_boom_n == 0) {
                        g_app.boom_w = sp.cells[c].width  * 0.0583f * 2.0f;
                        g_app.boom_h = sp.cells[c].height * 0.0583f * 2.0f;
                    }
                    g_app.fx_boom[g_app.fx_boom_n++] = t;
                }
            }
            g_app.fx_boom_dt = 0.07f;
            wax_free(&sp);
            OL_LOG("Explosion FX: %u frames\n", g_app.fx_boom_n);
        }
    }
}

/* Player/world sound effects (second half of SFX loading). */
static void load_sfx_rest(void) {
    /* Player sounds */
    g_app.sfx_player_hurt = sfx_load("gotshot.wav");
    g_app.sfx_player_land = sfx_load("LAND.WAV");
    g_app.sfx_enemy_shot  = sfx_load("ENMYSHOT.WAV");
    g_app.sfx_pickup        = sfx_load("Get.WAV");
    g_app.sfx_weapon_switch = sfx_load("weapgrab.WAV");
    g_app.sfx_glass_break   = sfx_load("SHATTR1.WAV");
    g_app.sfx_locked        = sfx_load("locked.wav");
    g_app.sfx_unlock        = sfx_load("UNLOCK.WAV");

    /* Enemy hit/die sounds (BGY0..BGY9) */
    for (int i = 0; i < 10; i++) {
        char name[32];
        snprintf(name, sizeof(name), "bgy%ddie.WAV", i);
        g_app.sfx_bgy_die[i] = sfx_load(name);
        snprintf(name, sizeof(name), "bgy%dhit.WAV", i);
        g_app.sfx_bgy_hit[i] = sfx_load(name);
    }

    /* Ambient looping sounds */
    g_app.sfx_fire    = sfx_load("FIRE.WAV");
    g_app.sfx_drip    = sfx_load("DRIP1.WAV");
    g_app.sfx_water   = sfx_load("WATER1.WAV");
    g_app.sfx_wind    = sfx_load("WIND1.WAV");
    g_app.sfx_windtunl = sfx_load("windtunl.WAV");
    g_app.sfx_vulture  = sfx_load("VULTURE1.WAV");
}

/*
 * Assign hit/die sound IDs to each enemy entity based on their BGY type,
 * and start looping ambient sounds for fire/water emitter entities.
 * Sounds are pre-loaded in load_sfx(); this function just assigns them and
 * starts any looping channels needed for this level's entities.
 */
/* Load each INF elevator/door's SOUND: file into its sound_id so doors, the
 * piano, the shovel-dig, train doors, etc. actually play. Sounds are cached by
 * name so a shared WAV (DOOR2.WAV on 40 doors) is only loaded once. */
static void setup_inf_sounds(void) {
    if (!g_app.audio.initialized) return;
    char names[64][64]; u32 ids[64]; u32 ncache = 0;
    for (u32 i = 0; i < g_app.world.inf.count; i++) {
        Elevator *el = &g_app.world.inf.elevs[i];
        if (!el->sound_file[0]) continue;
        u32 id = 0;
        for (u32 c = 0; c < ncache; c++)
            if (strcasecmp(names[c], el->sound_file) == 0) { id = ids[c]; break; }
        if (id == 0) {
            id = sfx_load(el->sound_file);
            if (ncache < 64) { snprintf(names[ncache], 64, "%s", el->sound_file);
                               ids[ncache++] = id; }
        }
        el->sound_id = id;
    }
}

static void setup_entity_sounds(void) {
    if (!g_app.audio.initialized) return;

    /* Track which ambient types have been started (one loop per type per level) */
    bool fire_started = false, drip_started = false;
    bool water_started = false, wind_started = false;
    bool windtl_started = false, vulutr_started = false;

    EntityList *list = &g_app.world.entities;
    for (u32 i = 0; i < list->count; i++) {
        Entity *e = &list->entities[i];
        if (!e->active) continue;
        const char *t = e->type_name;

        if (e->kind == ENTITY_ENEMY) {
            /* Extract BGY index from name: "BGY1" → 1, "BGY9SAT" → 9 */
            int bgy_idx = 0;
            if (strncasecmp(t, "BGY", 3) == 0 && t[3] >= '0' && t[3] <= '9')
                bgy_idx = t[3] - '0';
            e->sfx_hit = g_app.sfx_bgy_hit[bgy_idx];
            e->sfx_die = g_app.sfx_bgy_die[bgy_idx];
        }

        /* Ambient sound emitters: start looping on first occurrence */
        if (strncasecmp(t, "SFIRE", 5) == 0 ||
            strncasecmp(t, "FIRECIRC", 8) == 0 ||
            strncasecmp(t, "CAMFIRE", 7) == 0) {
            if (!fire_started && g_app.sfx_fire) {
                audio_play_looping(&g_app.audio, g_app.sfx_fire);
                fire_started = true;
            }
        } else if (strncasecmp(t, "SDRIP", 5) == 0) {
            if (!drip_started && g_app.sfx_drip) {
                audio_play_looping(&g_app.audio, g_app.sfx_drip);
                drip_started = true;
            }
        } else if (strncasecmp(t, "SWATER", 6) == 0 ||
                   strncasecmp(t, "SUWATER", 7) == 0) {
            if (!water_started && g_app.sfx_water) {
                audio_play_looping(&g_app.audio, g_app.sfx_water);
                water_started = true;
            }
        } else if (strncasecmp(t, "SWIND", 5) == 0) {
            if (!wind_started && g_app.sfx_wind) {
                audio_play_looping(&g_app.audio, g_app.sfx_wind);
                wind_started = true;
            }
        } else if (strncasecmp(t, "SWTUNL", 6) == 0) {
            if (!windtl_started && g_app.sfx_windtunl) {
                audio_play_looping(&g_app.audio, g_app.sfx_windtunl);
                windtl_started = true;
            }
        } else if (strncasecmp(t, "SVULTURE", 8) == 0) {
            if (!vulutr_started && g_app.sfx_vulture) {
                audio_play_looping(&g_app.audio, g_app.sfx_vulture);
                vulutr_started = true;
            }
        }
    }
}

/* Capture the headless screenshot when the target frame is reached; ends the
 * app afterwards. Called from both the menu and in-game render paths. */
static void screenshot_tick(int *frame_no) {
    if (g_app.screenshot_frames <= 0) return;
    (*frame_no)++;
    if (*frame_no < g_app.screenshot_frames) return;
    int w = g_app.renderer.cfg.width, h = g_app.renderer.cfg.height;
    u8 *px = malloc((size_t)w * h * 4);
    if (px) {
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        u8 *flip = malloc((size_t)w * h * 4);
        for (int y = 0; y < h; y++)
            memcpy(flip + (size_t)(h-1-y)*w*4, px + (size_t)y*w*4, (size_t)w*4);
        SDL_Surface *surf = SDL_CreateRGBSurfaceFrom(flip, w, h, 32, w*4,
            0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
        if (surf) { SDL_SaveBMP(surf, g_app.screenshot_path); SDL_FreeSurface(surf);
            OL_LOG("Screenshot saved: %s (%dx%d)\n", g_app.screenshot_path, w, h); }
        free(flip); free(px);
    }
    g_app.running = false;
}

/* Load (or reload) a level at runtime: geometry, entities, per-level sounds,
 * player spawn, and music. Used at startup, on F5 reload, and when a mission is
 * chosen from the menu. Returns false if the level failed to load. */
/* Resolve the player's tracked sector from their current position using the
 * Y-GATED GLOBAL find (hint = -1). Used at level load and after a respawn so the
 * player is anchored in the correct (possibly stacked/overlapping) sector rather
 * than defaulting to sector 0 and falling through the map. */
static void relocate_player_sector(void) {
    g_app.prev_sector = -1;
    int s = collision_find_sector_y(&g_app.world.lvt,
                                    g_app.player.pos.x, g_app.player.pos.z,
                                    g_app.player.pos.y, 0.0f, -1);
    if (s < 0)
        s = collision_find_sector(&g_app.world.lvt,
                                  g_app.player.pos.x, g_app.player.pos.z, 0);
    g_app.player.sector_idx = s;
}

/* Loading-screen callback: world_load calls this at each stage; draw the
 * MM220 background + green progress bar + level name, then swap. */
static void loading_progress_cb(float frac, const char *name) {
    if (g_app.screenshot_frames > 0) return;   /* headless: no interleave */
    char up[32]; int i = 0;
    for (; name && name[i] && i < 31; i++)
        up[i] = (name[i] >= 'a' && name[i] <= 'z') ? name[i] - 32 : name[i];
    up[i] = '\0';
    renderer_begin_frame(&g_app.renderer);
    renderer_draw_loading(&g_app.renderer, g_app.loading_bg_tex, frac, up);
    renderer_end_frame(&g_app.renderer);
}

static bool load_level_runtime(const char *name) {
    if (name != g_app.level_name)   /* avoid snprintf self-copy (UB) */
        snprintf(g_app.level_name, sizeof(g_app.level_name), "%s", name);
    audio_stop_all(&g_app.audio);
    if (g_app.world.loaded) world_free(&g_app.world);
    if (!world_load(&g_app.world, &g_app.archives, &g_app.renderer,
                    g_app.level_name, g_app.difficulty)) {
        OL_ERR("Failed to load level '%s'\n", g_app.level_name);
        return false;
    }
    setup_entity_sounds();
    setup_inf_sounds();
    memset(g_app.renderer.sector_scroll_u, 0, sizeof(g_app.renderer.sector_scroll_u));
    memset(g_app.renderer.sector_scroll_v, 0, sizeof(g_app.renderer.sector_scroll_v));

    /* Projectile billboards/FX need the level palette (built by world_load) */
    load_projectile_assets();
    projectile_init(&g_app.projectiles);   /* clear stale projectiles */

    player_init(&g_app.player, g_app.world.player_start);
    g_app.player.yaw        = g_app.world.player_start_yaw;
    if (g_app.give_all_keys) g_app.player.keys = 0x3FF;
    relocate_player_sector();   /* Y-gated anchor so the player doesn't fall in */

    if (getenv("OL_SLOPELIST")) {
        const LvtLevel *L = &g_app.world.lvt;
        for (u32 si = 0; si < L->sector_count; si++) {
            const LvtSector *s = &L->sectors[si];
            if (!s->has_slope_floor && !s->has_slope_ceil) continue;
            f32 cx=0,cz=0; for(u32 v=0;v<s->vertex_count;v++){cx+=s->vertices[v].x;cz+=s->vertices[v].y;}
            if(s->vertex_count){cx/=s->vertex_count;cz/=s->vertex_count;}
            OL_LOG("SLOPE sec %u '%s' at(%.0f,%.0f) floor=%.1f ceil=%.1f slF=%d(w%d a%d) slC=%d(w%d a%d)\n",
                   si, s->name, cx, cz, s->floor_y, s->ceil_y,
                   s->has_slope_floor, s->slope_floor_wall, s->slope_floor_angle,
                   s->has_slope_ceil, s->slope_ceil_wall, s->slope_ceil_angle);
        }
    }
    if (getenv("OL_SECDUMP")) {
        const LvtLevel *L = &g_app.world.lvt;
        int si = atoi(getenv("OL_SECDUMP"));
        if (si >= 0 && si < (int)L->sector_count) {
            const LvtSector *s = &L->sectors[si];
            f32 cx=0,cz=0; for(u32 v=0;v<s->vertex_count;v++){cx+=s->vertices[v].x;cz+=s->vertices[v].y;}
            if(s->vertex_count){cx/=s->vertex_count;cz/=s->vertex_count;}
            OL_LOG("SECDUMP %d '%s' floor=%.1f ceil=%.1f centroid=(%.0f,%.0f) walls=%u door_open=%d\n",
                   si, s->name, s->floor_y, s->ceil_y, cx, cz, s->wall_count, s->door_open);
            for (u32 wi=0; wi<s->wall_count; wi++){ const LvtWall*w=&s->walls[wi];
                if(w->adjoin>=0){ const LvtSector*a=&L->sectors[w->adjoin];
                    Vec2 p0=s->vertices[w->v1], p1=s->vertices[w->v2];
                    OL_LOG("   wall %u (%.0f,%.0f)-(%.0f,%.0f) mid(%.0f,%.0f) -> adjoin sec %d '%s' floor=%.1f ceil=%.1f flags=%#x flags2=%#x dadjoin=%d midtex=%d win=%d\n",
                           wi, p0.x,p0.y, p1.x,p1.y, (p0.x+p1.x)*0.5f,(p0.y+p1.y)*0.5f,
                           w->adjoin, a->name, a->floor_y, a->ceil_y, w->flags, w->flags2, w->dadjoin, w->mid.tex_id, w->is_window); } }
        }
    }
    if (getenv("OL_FINDTEX")) {
        /* Find walls whose mid/top/bot texture NAME contains the substring.
         * Print sector, wall, midpoint, and an outward normal + a suggested
         * head-on camera pos (interior side) so a sign can be viewed squarely. */
        const LvtLevel *L = &g_app.world.lvt;
        const char *want = getenv("OL_FINDTEX");
        int n=0;
        for (u32 si=0; si<L->sector_count && n<20; si++){ const LvtSector*s=&L->sectors[si];
            for (u32 wi=0; wi<s->wall_count && n<20; wi++){ const LvtWall*w=&s->walls[wi];
                int ids[3]={w->mid.tex_id,w->top.tex_id,w->bot.tex_id};
                const char*hit=NULL;
                for(int k=0;k<3;k++){ if(ids[k]>=0 && ids[k]<(i32)LVT_MAX_TEXTURES){
                    if(strcasestr(L->textures[ids[k]], want)){ hit=L->textures[ids[k]]; break; } } }
                if(!hit) continue;
                if(w->v1<0||w->v2<0) continue;
                Vec2 p0=s->vertices[w->v1], p1=s->vertices[w->v2];
                f32 mx=(p0.x+p1.x)*0.5f, mz=(p0.y+p1.y)*0.5f;
                /* wall dir & a normal; interior is inside the sector so step toward centroid */
                f32 cx=0,cz=0; for(u32 v=0;v<s->vertex_count;v++){cx+=s->vertices[v].x;cz+=s->vertices[v].y;}
                if(s->vertex_count){cx/=s->vertex_count;cz/=s->vertex_count;}
                f32 nx=cx-mx, nz=cz-mz; f32 nl=sqrtf(nx*nx+nz*nz); if(nl>0){nx/=nl;nz/=nl;}
                f32 camx=mx+nx*18.0f, camz=mz+nz*18.0f;
                f32 yaw=atan2f(-nx,-nz)*180.0f/3.14159265f; /* face the wall */
                OL_LOG("FINDTEX sec %u wall %u '%s' mid(%.0f,%.0f) v1(%.0f,%.0f) v2(%.0f,%.0f) flags=%#x -> CAM pos %.0f %.0f yaw %.0f\n",
                       si, wi, hit, mx,mz, p0.x,p0.y, p1.x,p1.y, w->flags, camx, camz, yaw);
                n++;
            } }
        OL_LOG("FINDTEX done n=%d\n", n);
    }
    if (getenv("OL_TEXLIST")) {
        const LvtLevel *L = &g_app.world.lvt;
        for (u32 i=0;i<LVT_MAX_TEXTURES;i++){ if(L->textures[i][0]) OL_LOG("TEX %u = '%s'\n", i, L->textures[i]); }
    }
    if (getenv("OL_DUMPTEX")) {
        /* Read back an uploaded GL texture by name substring -> PPM. Isolates the
         * decoder/upload from UV/rendering: if text is mirrored HERE, the PCX
         * decode is flipped; if correct here, the mirror is in the mesh UV. */
        const char *want = getenv("OL_DUMPTEX");
        const Renderer *r = &g_app.renderer;
        for (u32 i=0;i<R_MAX_TEXTURES;i++){ const GpuTexture*t=&r->textures[i];
            if(!t->handle || !strcasestr(t->name, want)) continue;
            u32 w=t->width,h=t->height; if(!w||!h) continue;
            u8 *buf = malloc((size_t)w*h*4); if(!buf) break;
            glBindTexture(GL_TEXTURE_2D, t->handle);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
            char path[256]; snprintf(path,sizeof(path),"%s/dumptex_%s.ppm", getenv("OL_DUMPDIR")?getenv("OL_DUMPDIR"):".", t->name);
            FILE*f=fopen(path,"wb");
            if(f){ fprintf(f,"P6\n%u %u\n255\n",w,h);
                for(u32 p=0;p<w*h;p++) fwrite(&buf[p*4],1,3,f);
                fclose(f); OL_LOG("DUMPTEX '%s' %ux%u -> %s\n", t->name, w, h, path); }
            free(buf);
        }
    }
    if (getenv("OL_SCANSIGN")) {
        /* Locate walls flagged WF1_ILLUM_SIGN (0x02) — the sign textures. */
        const LvtLevel *L = &g_app.world.lvt;
        int n=0;
        for (u32 si=0; si<L->sector_count; si++){ const LvtSector*s=&L->sectors[si];
            for (u32 wi=0; wi<s->wall_count; wi++){ const LvtWall*w=&s->walls[wi];
                if (!(w->flags & 0x02u)) continue;
                if (w->v1<0||w->v2<0) continue;
                Vec2 p0=s->vertices[w->v1], p1=s->vertices[w->v2];
                if (n++ < 25)
                    OL_LOG("SIGN sec %u wall %u mid(%.0f,%.0f) flags=%#x midtex=%d toptex=%d bottex=%d\n",
                           si, wi, (p0.x+p1.x)*0.5f,(p0.y+p1.y)*0.5f, w->flags,
                           w->mid.tex_id, w->top.tex_id, w->bot.tex_id);
            } }
        OL_LOG("SIGN total=%d\n", n);
    }
    if (getenv("OL_SCANMID")) {
        /* Scan ADJOIN_MID (0x2000) walls, categorize by geometry + solidity flags */
        const LvtLevel *L = &g_app.world.lvt;
        int n_open=0, n_open_solid=0, n_open_win=0, n_step=0;
        for (u32 si=0; si<L->sector_count; si++){ const LvtSector*s=&L->sectors[si];
            for (u32 wi=0; wi<s->wall_count; wi++){ const LvtWall*w=&s->walls[wi];
                if (w->adjoin<0 || !(w->flags & 0x2000u)) continue;
                const LvtSector*a=&L->sectors[w->adjoin];
                bool same_floor = (a->floor_y > s->floor_y-0.5f && a->floor_y < s->floor_y+0.5f);
                bool same_ceil  = (a->ceil_y  > s->ceil_y -0.5f && a->ceil_y  < s->ceil_y +0.5f);
                bool solid = (w->flags2 & 0x02u) != 0;
                if (same_floor && same_ceil) {
                    /* fully open span: candidate open-portal OR full-height fence */
                    n_open++;
                    if (solid) n_open_solid++;
                    if (w->is_window) n_open_win++;
                    if (n_open <= 12)
                        OL_LOG("SCANMID sec %u wall %u -> %d  fl=%.1f/%.1f cl=%.1f/%.1f flags=%#x flags2=%#x dadj=%d midtex=%d win=%d\n",
                               si, wi, w->adjoin, s->floor_y, a->floor_y, s->ceil_y, a->ceil_y,
                               w->flags, w->flags2, w->dadjoin, w->mid.tex_id, w->is_window);
                } else n_step++;
            } }
        OL_LOG("SCANMID totals: open_span=%d (solid=%d window=%d)  stepped=%d\n",
               n_open, n_open_solid, n_open_win, n_step);
    }
    if (getenv("OL_PROBE2")) {
        const LvtLevel *L = &g_app.world.lvt;
        const char *pe = getenv("OL_PROBE2");
        f32 X = 451, Z = 422; sscanf(pe, "%f,%f", &X, &Z);
        OL_LOG("PROBE2 (%.0f,%.0f):\n", X, Z);
        for (u32 si = 0; si < L->sector_count; si++) {
            const LvtSector *s = &L->sectors[si];
            bool in=false; u32 j=s->vertex_count-1;
            for (u32 k=0;k<s->vertex_count;k++){ f32 xi=s->vertices[k].x,zi=s->vertices[k].y,xj=s->vertices[j].x,zj=s->vertices[j].y;
                if(((zi>Z)!=(zj>Z))&&(X<(xj-xi)*(Z-zi)/(zj-zi)+xi)) in=!in; j=k; }
            if (in) OL_LOG("  sec %u '%s' floorY=%.1f floor@pt=%.1f ceilY=%.1f ceil@pt=%.1f slopeF=%d ang=%d slopeC=%d\n",
                           si, s->name, s->floor_y, lvt_floor_at(s,X,Z), s->ceil_y, lvt_ceil_at(s,X,Z),
                           s->has_slope_floor, s->slope_floor_angle, s->has_slope_ceil);
        }
    }

    /* Music: MSC → CD track → GOG OGG (data/MUSIC/TrackNN.ogg). */
    if (g_app.audio.initialized && g_app.world.music_file[0]) {
        u32 msc_sz = 0;
        const u8 *msc_data = archives_get(&g_app.archives, g_app.world.music_file, &msc_sz);
        int track_num = 0;
        if (msc_data && msc_sz > 0) {
            const char *p = (const char *)msc_data, *end = p + msc_sz;
            while (p < end) {
                while (p < end && (*p == ' ' || *p == '\t')) p++;
                if (strncasecmp(p, "CD_TRACK", 8) == 0) {
                    p += 8;
                    while (p < end && (*p == ' ' || *p == '\t')) p++;
                    while (p < end && *p != ' ' && *p != '\t') p++;
                    while (p < end && (*p == ' ' || *p == '\t')) p++;
                    track_num = 0;
                    while (p < end && *p >= '0' && *p <= '9') track_num = track_num*10 + (*p++ - '0');
                    break;
                }
                while (p < end && *p != '\n') p++;
                if (p < end) p++;
            }
        }
        char music_path[768];
        if (track_num >= 2 && track_num <= 99)
            snprintf(music_path, sizeof(music_path), "%s/MUSIC/Track%02d.ogg", g_app.data_dir, track_num);
        else
            snprintf(music_path, sizeof(music_path), "%s/%s", g_app.data_dir, g_app.world.music_file);
        audio_play_music(&g_app.audio, music_path);
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Story campaign
 *
 * The single-player story is a fixed level sequence. STORY starts at index 0
 * and each completed mission auto-advances to the next; the player's weapons
 * and ammo carry over between missions (health refills at each mission start),
 * matching the original.
 * ---------------------------------------------------------------------- */
/* Verified retail order (fallback if OUTLAWS.RCS can't be read). */
static const char *CAMPAIGN_FALLBACK[] = {
    "hideout", "town", "train", "canyon", "mill",
    "simms", "miner", "cliff", "ranch",
};
/* Cutscene played AFTER each fallback level (from OUTLAWS.RCS MOVIE/CREDITS). */
static const char *CAMPAIGN_FALLBACK_MOVIE[] = {
    "she_tob.san", "toe_trb.san", "tre_cab.san", "cae_sab.san", "sae_hib.san",
    "hie_mnb.san", "mne_clb.san", "cle_rab.san", "rae.san",
};

/*
 * Build the campaign level order by parsing the original story script
 * OUTLAWS.RCS (BPCR resource): every "LEVEL: <name> <disc>" line, in order.
 * This is exactly how the original sequences the story. Falls back to the
 * verified retail order if the script is missing.
 */
/* Read an entire file into a malloc'd buffer (caller frees). NULL on error. */
static u8 *read_whole_file(const char *path, u32 *out_sz) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    u8 *b = (u8 *)malloc((size_t)n);
    if (!b) { fclose(f); return NULL; }
    size_t rd = fread(b, 1, (size_t)n, f);
    fclose(f);
    if (rd != (size_t)n) { free(b); return NULL; }
    *out_sz = (u32)n;
    return b;
}

/*
 * Play a SMUSH .SAN cutscene fullscreen (letterboxed to the video's 4:3),
 * synced to its IACT audio track. Esc / Enter / Space / mouse-click / window
 * close skips it (faithful: the original skips on Esc). The .SAN files are
 * loose files in the data directory. Returns false if the user asked to quit
 * the whole application (SDL_QUIT), true otherwise.
 */
static bool cutscene_play(const char *san_name) {
    if (!san_name || !san_name[0]) return true;
    if (g_app.screenshot_frames > 0 || g_app.check_mode ||
        g_app.check_doors_mode) return true;   /* headless/automated */

    char path[1024], up[64];
    u32 fsz = 0;
    snprintf(path, sizeof path, "%s/%s", g_app.data_dir, san_name);
    u8 *fdata = read_whole_file(path, &fsz);
    if (!fdata) {   /* retry uppercase (data files are UPPERCASE on disk) */
        int i = 0;
        for (; san_name[i] && i < 63; i++) up[i] = (char)toupper((unsigned char)san_name[i]);
        up[i] = '\0';
        snprintf(path, sizeof path, "%s/%s", g_app.data_dir, up);
        fdata = read_whole_file(path, &fsz);
    }
    if (!fdata) { OL_WARN("Cutscene '%s' not found\n", san_name); return true; }

    SmushVideo *v = smush_open(fdata, fsz);
    if (!v) { OL_WARN("Cutscene '%s' failed to parse\n", san_name); free(fdata); return true; }
    int vw = smush_width(v), vh = smush_height(v);
    float fps = smush_fps(v);
    OL_LOG("Cutscene %s: %dx%d, %d frames @ %.0f fps\n",
           san_name, vw, vh, smush_frame_count(v), fps);

    /* Dedicated 22050 Hz S16 stereo device for the IACT audio stream. */
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 22050; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 2048;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev) SDL_PauseAudioDevice(dev, 0);

    SDL_bool prev_rel = SDL_GetRelativeMouseMode();
    if (prev_rel) SDL_SetRelativeMouseMode(SDL_FALSE);

    u32 vtex = 0;
    long queued_bytes = 0;
    Uint32 start = SDL_GetTicks();
    bool skip = false, quit = false;
    const u8 *rgba; const i16 *pcm; int pb; int fi = 0;

    while (smush_next(v, &rgba, &pcm, &pb)) {
        if (rgba) renderer_upload_video(&g_app.renderer, &vtex, rgba, vw, vh);
        if (dev && pcm && pb > 0) { SDL_QueueAudio(dev, pcm, (Uint32)pb); queued_bytes += pb; }

        f32 W = (f32)g_app.renderer.cfg.width, H = (f32)g_app.renderer.cfg.height;
        f32 sc = (W / (f32)vw < H / (f32)vh) ? W / (f32)vw : H / (f32)vh;
        f32 dw = vw * sc, dh = vh * sc, dx = (W - dw) * 0.5f, dy = (H - dh) * 0.5f;
        renderer_begin_frame(&g_app.renderer);
        renderer_draw_rect(&g_app.renderer, 0, 0, W, H, 0, 0, 0, 1);   /* letterbox bars */
        if (vtex) renderer_draw_image(&g_app.renderer, vtex, dx, dy, dw, dh, 1.0f, 1.0f);
        renderer_end_frame(&g_app.renderer);

        /* Present the next frame when playback (audio clock, else wall clock)
         * reaches its timestamp — keeps video synced to the audio. */
        double target = (double)(fi + 1) / (double)fps;
        for (;;) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) { quit = true; skip = true; }
                else if (e.type == SDL_KEYDOWN) {
                    SDL_Keycode k = e.key.keysym.sym;
                    if (k == SDLK_ESCAPE || k == SDLK_RETURN || k == SDLK_KP_ENTER ||
                        k == SDLK_SPACE) skip = true;
                } else if (e.type == SDL_MOUSEBUTTONDOWN) {
                    skip = true;
                } else if (e.type == SDL_WINDOWEVENT &&
                           e.window.event == SDL_WINDOWEVENT_RESIZED) {
                    renderer_resize(&g_app.renderer, e.window.data1, e.window.data2);
                }
            }
            if (skip) break;
            double now;
            if (dev && queued_bytes > 0) {
                Uint32 rem = SDL_GetQueuedAudioSize(dev);
                now = (double)(queued_bytes - (long)rem) / (4.0 * 22050.0);
            } else {
                now = (SDL_GetTicks() - start) / 1000.0;
            }
            if (now >= target) break;
            SDL_Delay(2);
        }
        if (skip) break;
        fi++;
    }

    if (dev) { SDL_PauseAudioDevice(dev, 1); SDL_ClearQueuedAudio(dev); SDL_CloseAudioDevice(dev); }
    if (prev_rel) SDL_SetRelativeMouseMode(SDL_TRUE);
    smush_close(v);
    free(fdata);
    return !quit;
}

/* Headless: decode a representative frame of a cutscene, render it through the
 * engine (video texture + letterboxed quad) and save a BMP. Verifies the whole
 * in-engine playback path without a display. */
static void cutscene_shot(const char *san_name, const char *out_bmp) {
    char path[1024], up[64]; u32 fsz = 0;
    snprintf(path, sizeof path, "%s/%s", g_app.data_dir, san_name);
    u8 *fdata = read_whole_file(path, &fsz);
    if (!fdata) {
        int i = 0; for (; san_name[i] && i < 63; i++) up[i]=(char)toupper((unsigned char)san_name[i]);
        up[i]='\0'; snprintf(path, sizeof path, "%s/%s", g_app.data_dir, up);
        fdata = read_whole_file(path, &fsz);
    }
    if (!fdata) { printf("CUTSCENE %s RESULT=FAIL (not found)\n", san_name); return; }
    SmushVideo *v = smush_open(fdata, fsz);
    if (!v) { printf("CUTSCENE %s RESULT=FAIL (parse)\n", san_name); free(fdata); return; }
    int vw = smush_width(v), vh = smush_height(v);
    int target = smush_frame_count(v) / 8; if (target < 30) target = 30;
    u32 vtex = 0; const u8 *rgba; const i16 *pcm; int pb, fi = 0; long apcm = 0;
    while (smush_next(v, &rgba, &pcm, &pb)) {
        apcm += pb;
        if (rgba) renderer_upload_video(&g_app.renderer, &vtex, rgba, vw, vh);
        if (fi >= target) break;
        fi++;
    }
    f32 W = (f32)g_app.renderer.cfg.width, H = (f32)g_app.renderer.cfg.height;
    f32 sc = (W/(f32)vw < H/(f32)vh) ? W/(f32)vw : H/(f32)vh;
    f32 dw = vw*sc, dh = vh*sc, dx = (W-dw)*0.5f, dy = (H-dh)*0.5f;
    renderer_begin_frame(&g_app.renderer);
    renderer_draw_rect(&g_app.renderer, 0, 0, W, H, 0, 0, 0, 1);
    if (vtex) renderer_draw_image(&g_app.renderer, vtex, dx, dy, dw, dh, 1.0f, 1.0f);
    glFinish();
    int w = g_app.renderer.cfg.width, h = g_app.renderer.cfg.height;
    u8 *px = (u8*)malloc((size_t)w*h*4);
    glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,px);
    u8 *fl = (u8*)malloc((size_t)w*h*4);
    for (int y=0;y<h;y++) memcpy(fl+(size_t)(h-1-y)*w*4, px+(size_t)y*w*4, (size_t)w*4);
    SDL_Surface *sf = SDL_CreateRGBSurfaceFrom(fl,w,h,32,w*4,0xFF,0xFF00,0xFF0000,0xFF000000);
    if (sf){ SDL_SaveBMP(sf, out_bmp); SDL_FreeSurface(sf); }
    free(fl); free(px);
    printf("CUTSCENE %s RESULT=OK %dx%d frames=%d audio=%.1fs -> %s\n",
           san_name, vw, vh, smush_frame_count(v), apcm/4.0/22050.0, out_bmp);
    smush_close(v); free(fdata);
}

/* Read the whitespace-delimited token at *pp into buf (advances *pp). */
static void rcs_token(const char **pp, const char *end, char *buf, int cap) {
    const char *p = *pp;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    int n = 0;
    while (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && n < cap - 1)
        buf[n++] = *p++;
    buf[n] = '\0';
    *pp = p;
}

/* Parse a STORY block (LEVEL/MOVIE/CREDITS ... END) from an RCS/RCA resource into
 * the campaign arrays. Returns the level count (0 if the resource is missing/empty).
 * Shared by the main story (OUTLAWS.RCS) and the historical missions' own .rca. */
static int campaign_parse_rcs(const char *resource) {
    g_app.campaign_count = 0;
    for (int i = 0; i < 16; i++) g_app.campaign_movie[i][0] = '\0';
    u32 sz = 0;
    const u8 *d = archives_get(&g_app.archives, resource, &sz);
    if (d && sz > 0) {
        const char *p = (const char *)d, *end = p + sz;
        int last_level = -1;
        bool ended = false;
        while (p < end && g_app.campaign_count < 16 && !ended) {
            if ((size_t)(end - p) >= 6 && strncasecmp(p, "LEVEL:", 6) == 0) {
                p += 6;
                rcs_token(&p, end, g_app.campaign[g_app.campaign_count],
                          (int)sizeof(g_app.campaign[0]));
                if (g_app.campaign[g_app.campaign_count][0]) {
                    last_level = g_app.campaign_count;
                    g_app.campaign_count++;
                }
            } else if (((size_t)(end - p) >= 6 && strncasecmp(p, "MOVIE:", 6) == 0) ||
                       ((size_t)(end - p) >= 8 && strncasecmp(p, "CREDITS:", 8) == 0)) {
                int kw = (strncasecmp(p, "MOVIE:", 6) == 0) ? 6 : 8;
                p += kw;
                char tok[32];
                rcs_token(&p, end, tok, sizeof(tok));
                if (tok[0] && last_level >= 0)
                    snprintf(g_app.campaign_movie[last_level], 32, "%s", tok);
            } else if ((size_t)(end - p) >= 3 && strncasecmp(p, "END", 3) == 0 &&
                       (p + 3 >= end || p[3] == '\r' || p[3] == '\n' ||
                        p[3] == ' ' || p[3] == '\t')) {
                if (g_app.campaign_count > 0) ended = true; else p++;
            } else {
                p++;
            }
        }
    }
    return g_app.campaign_count;
}

static void campaign_parse(void) {
    g_app.campaign_count = 0;
    g_app.campaign_open_movie[0] = '\0';
    for (int i = 0; i < 16; i++) g_app.campaign_movie[i][0] = '\0';

    u32 sz = 0;
    const u8 *d = archives_get(&g_app.archives, "OUTLAWS.RCS", &sz);
    if (d && sz > 0) {
        const char *p = (const char *)d, *end = p + sz;
        int last_level = -1;
        bool ended = false;
        /* Parse only the first campaign block (LEVEL/MOVIE/CREDITS ... END).
         * MOVIE:/CREDITS: name the .SAN played after the most recent LEVEL. */
        while (p < end && g_app.campaign_count < 16 && !ended) {
            if ((size_t)(end - p) >= 6 && strncasecmp(p, "LEVEL:", 6) == 0) {
                p += 6;
                rcs_token(&p, end, g_app.campaign[g_app.campaign_count],
                          (int)sizeof(g_app.campaign[0]));
                if (g_app.campaign[g_app.campaign_count][0]) {
                    last_level = g_app.campaign_count;
                    g_app.campaign_count++;
                }
            } else if (((size_t)(end - p) >= 6 && strncasecmp(p, "MOVIE:", 6) == 0) ||
                       ((size_t)(end - p) >= 8 && strncasecmp(p, "CREDITS:", 8) == 0)) {
                int kw = (strncasecmp(p, "MOVIE:", 6) == 0) ? 6 : 8;
                p += kw;
                char tok[32];
                rcs_token(&p, end, tok, sizeof(tok));
                if (tok[0] && last_level >= 0)
                    snprintf(g_app.campaign_movie[last_level], 32, "%s", tok);
            } else if ((size_t)(end - p) >= 3 && strncasecmp(p, "END", 3) == 0 &&
                       (p + 3 >= end || p[3] == '\r' || p[3] == '\n' ||
                        p[3] == ' ' || p[3] == '\t')) {
                if (g_app.campaign_count > 0) ended = true; else p++;
            } else {
                p++;
            }
        }
        /* Opening cinematic: the ITEM "outlaws" OPEN1 entry names op_cr.san. */
        for (const char *q = (const char *)d; q + 5 < end; q++) {
            if (strncasecmp(q, "op_cr", 5) == 0) {
                rcs_token(&q, end, g_app.campaign_open_movie,
                          (int)sizeof(g_app.campaign_open_movie));
                break;
            }
        }
    }
    if (g_app.campaign_count == 0) {
        int n = (int)(sizeof(CAMPAIGN_FALLBACK)/sizeof(CAMPAIGN_FALLBACK[0]));
        for (int i = 0; i < n; i++) {
            snprintf(g_app.campaign[i], sizeof(g_app.campaign[i]), "%s", CAMPAIGN_FALLBACK[i]);
            snprintf(g_app.campaign_movie[i], 32, "%s", CAMPAIGN_FALLBACK_MOVIE[i]);
        }
        g_app.campaign_count = n;
    }
    if (g_app.campaign_open_movie[0] == '\0')
        snprintf(g_app.campaign_open_movie, sizeof(g_app.campaign_open_movie), "op_cr.san");
    OL_LOG("Campaign: %d missions (%s ...), opening=%s\n", g_app.campaign_count,
           g_app.campaign_count ? g_app.campaign[0] : "?", g_app.campaign_open_movie);
}

/* Load campaign level `idx`. If carry, keep only the weapons + ammo (health
 * refills and level items/keys reset each mission, like the original). */
static bool campaign_load(int idx, bool carry) {
    if (idx < 0 || idx >= g_app.campaign_count) return false;
    WeaponState loadout = g_app.player.weapons;
    if (!load_level_runtime(g_app.campaign[idx])) return false;
    if (carry) g_app.player.weapons = loadout;   /* guns + ammo only */
    g_app.campaign_idx = idx;
    g_app.campaign_active = true;
    return true;
}

/* Populate the pause menu's save-slot list from disk. */
static void refresh_save_slots(void) {
    for (int i = 0; i < SAVE_SLOTS; i++)
        g_app.menu.slot_used[i] =
            savegame_peek(i, g_app.menu.slot_label[i], sizeof(g_app.menu.slot_label[i]));
}

/* Apply the resolution/fullscreen from g_settings to the live window. */
static void apply_video_settings(void) {
    SDL_Window *w = g_app.renderer.window;
    if (!w) return;
    if (g_settings.fullscreen) {
        SDL_SetWindowFullscreen(w, SDL_WINDOW_FULLSCREEN_DESKTOP);
    } else {
        SDL_SetWindowFullscreen(w, 0);
        SDL_SetWindowSize(w, g_settings.win_w, g_settings.win_h);
        SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    int ww = 0, hh = 0; SDL_GetWindowSize(w, &ww, &hh);
    if (ww > 0 && hh > 0) renderer_resize(&g_app.renderer, ww, hh);
}

/* Snapshot the current game into a save slot. */
static bool do_save(int slot) {
    SaveGame sg; memset(&sg, 0, sizeof(sg));
    snprintf(sg.level, sizeof(sg.level), "%s", g_app.level_name);
    sg.campaign_active = g_app.campaign_active ? 1 : 0;
    sg.campaign_idx    = g_app.campaign_idx;
    sg.px = g_app.player.pos.x; sg.py = g_app.player.pos.y; sg.pz = g_app.player.pos.z;
    sg.yaw = g_app.player.yaw;  sg.pitch = g_app.player.pitch;
    sg.health = g_app.player.health;
    sg.keys   = g_app.player.keys;
    sg.weapons = g_app.player.weapons;
    /* Slot label: campaign mission number or the level name. */
    if (g_app.campaign_active)
        snprintf(sg.label, sizeof(sg.label), "MISSION %d - %s",
                 g_app.campaign_idx + 1, g_app.level_name);
    else
        snprintf(sg.label, sizeof(sg.label), "%s", g_app.level_name);
    return savegame_write(slot, &sg);
}

/* Load a save slot: load its level, then restore the player state. */
static bool do_load(int slot) {
    SaveGame sg;
    if (!savegame_read(slot, &sg)) return false;
    if (!load_level_runtime(sg.level)) return false;
    g_app.campaign_active = sg.campaign_active != 0;
    g_app.campaign_idx    = sg.campaign_idx;
    g_app.player.pos = (Vec3){ sg.px, sg.py, sg.pz };
    g_app.player.yaw = sg.yaw; g_app.player.pitch = sg.pitch;
    g_app.player.health = sg.health;
    g_app.player.keys   = sg.keys;
    g_app.player.weapons = sg.weapons;
    g_app.player.dead = false; g_app.player.dead_timer = 0.0f;
    g_app.prev_sector = -1;
    g_app.player.sector_idx = collision_find_sector(&g_app.world.lvt,
                                  g_app.player.pos.x, g_app.player.pos.z, 0);
    return true;
}

/* -------------------------------------------------------------------------
 * Initialization
 * ---------------------------------------------------------------------- */
static bool app_init(int argc, char **argv) {
    snprintf(g_app.data_dir,   sizeof(g_app.data_dir),   "%s", DEFAULT_DATA);
    snprintf(g_app.level_name, sizeof(g_app.level_name), "%s", DEFAULT_LEVEL);
    g_app.difficulty = 2;   /* Medium — Outlaws campaign default */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--data") == 0 && i+1 < argc) {
            snprintf(g_app.data_dir, sizeof(g_app.data_dir), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--level") == 0 && i+1 < argc) {
            snprintf(g_app.level_name, sizeof(g_app.level_name), "%s", argv[++i]);
            g_app.level_from_cli = true;   /* explicit level → skip the menu */
        } else if (strcmp(argv[i], "--check") == 0) {
            g_app.check_mode = true;   /* load level, print health summary, exit */
        } else if (strcmp(argv[i], "--check-doors") == 0) {
            g_app.check_doors_mode = true;   /* test every door, exit */
            g_app.level_from_cli = true;
        } else if (strcmp(argv[i], "--screenshot") == 0 && i+1 < argc) {
            snprintf(g_app.screenshot_path, sizeof(g_app.screenshot_path), "%s", argv[++i]);
            if (g_app.screenshot_frames == 0) g_app.screenshot_frames = 30;
        } else if (strcmp(argv[i], "--frames") == 0 && i+1 < argc) {
            g_app.screenshot_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fire") == 0) {
            g_app.screenshot_firing = true;
        } else if (strcmp(argv[i], "--difficulty") == 0 && i+1 < argc) {
            g_app.difficulty = atoi(argv[++i]);   /* 1..4 */
        } else if (strcmp(argv[i], "--weapon") == 0 && i+1 < argc) {
            g_app.screenshot_weapon = atoi(argv[++i]);
            g_app.screenshot_weapon_set = true;
        } else if (strcmp(argv[i], "--near-enemy") == 0) {
            g_app.screenshot_near_enemy = true;
        } else if (strcmp(argv[i], "--open-doors") == 0) {
            g_app.force_open_doors = true;
        } else if (strcmp(argv[i], "--crouch") == 0) {
            g_app.force_crouch = true;
        } else if (strcmp(argv[i], "--use") == 0) {
            g_app.force_use = true;
        } else if (strcmp(argv[i], "--walk") == 0) {
            g_app.force_walk = true;
        } else if (strcmp(argv[i], "--givekeys") == 0) {
            g_app.give_all_keys = true;   /* grant every key/tool at spawn */
        } else if (strcmp(argv[i], "--map") == 0) {
            g_app.show_map = true;        /* debug: automap on at start */
        } else if (strcmp(argv[i], "--menu") == 0) {
            g_app.force_menu = true;      /* start at the front-end menu */
        } else if (strcmp(argv[i], "--cutscene") == 0 && i+1 < argc) {
            snprintf(g_app.cutscene_cli, sizeof(g_app.cutscene_cli), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--posy") == 0 && i+1 < argc) {
            g_app.spawn_y = (f32)atof(argv[++i]);
            g_app.spawn_y_set = true;     /* explicit spawn height (test upper floors) */
        } else if (strcmp(argv[i], "--pos") == 0 && i+2 < argc) {
            g_app.spawn_x = (f32)atof(argv[++i]);
            g_app.spawn_z = (f32)atof(argv[++i]);
            g_app.spawn_yaw_deg = (i+1 < argc && argv[i+1][0] != '-') ? (f32)atof(argv[++i]) : 90.0f;
            g_app.spawn_set = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: outlaws [--data <dir>] [--level <name>]\n");
            printf("  --data  <dir>   Path to game data directory (default: ./data)\n");
            printf("  --level <name>  Level name to load (default: CANYON)\n");
            printf("\nLevels: CANYON, HIDEOUT, TOWN, TRAIN, DRYGULCH, FORT,\n");
            printf("        GRANARY, RANCH, CLIFF, MILL, MINER, OFFICE, SIMMS\n");
            printf("\nControls:\n");
            printf("  WASD/Arrows: Move     Mouse: Look    Shift: Run\n");
            printf("  LMB: Fire            1-9: Switch weapon\n");
            printf("  F5: Reload level     Esc: Release mouse / quit\n");
            exit(0);
        }
    }

    OL_LOG("Outlaws Engine - Open Source Recreation\n");
    OL_LOG("Data: %s  Level: %s\n", g_app.data_dir, g_app.level_name);

    /* Open archives */
    if (!archives_open(&g_app.archives, g_app.data_dir)) {
        OL_ERR("Failed to open game archives from '%s'\n", g_app.data_dir);
        return false;
    }

    /* Load persistent settings (outlaws.cfg) — needed before any input read
     * (key bindings) and before the renderer (window resolution). Write the
     * defaults out on first run so the file exists and is discoverable. */
    if (!settings_load(&g_settings)) settings_save(&g_settings);

    /* Initialize renderer at the saved resolution. */
    RenderConfig rcfg = {
        .width      = g_settings.win_w > 0 ? g_settings.win_w : DEFAULT_WIDTH,
        .height     = g_settings.win_h > 0 ? g_settings.win_h : DEFAULT_HEIGHT,
        .fov        = DEFAULT_FOV,
        .near_plane = 1.0f,
        .far_plane  = 2048.0f,
    };
    if (!renderer_init(&g_app.renderer, &rcfg, "Outlaws - Open Source Recreation")) {
        OL_ERR("Renderer init failed\n");
        return false;
    }

    /* Screenshot mode: pin a fixed render size so a tiling WM resizing the
     * window can't change the FOV/aspect between captures (makes A/B shots
     * comparable). 800x600 fits inside any tile the WM hands us. */
    if (g_app.screenshot_frames > 0) {
        SDL_SetWindowSize(g_app.renderer.window, 800, 600);
        renderer_resize(&g_app.renderer, 800, 600);
    }

    /* Debug UI (ImGui) */
    debug_ui_init(g_app.renderer.window, g_app.renderer.gl_ctx);
    /* Post-FX off by default, but seed sensible slider values (so enabling an
     * effect manually starts from a good baseline, not zeros). */
    postfx_apply_preset(&g_debug.postfx, POST_PRESET_OFF);
    /* OL_POSTFX=1..3 applies a preset at startup (1=CRT 2=Cinematic 3=Vibrant)
     * for quick demos / headless screenshots; the debug menu overrides it. */
    {
        const char *pf = getenv("OL_POSTFX");
        int pr = pf ? atoi(pf) : 0;
        if (pr >= POST_PRESET_CRT && pr < POST_PRESET_COUNT)
            postfx_apply_preset(&g_debug.postfx, pr);
    }

    /* Audio */
    if (!audio_init(&g_app.audio))
        OL_WARN("Audio init failed - continuing without audio\n");

    /* Input */
    input_init(&g_app.input);

    /* Load HUD assets (weapon sprites, face portrait) BEFORE the level so the
     * HUD/weapon textures are guaranteed a slot even on large levels. */
    load_hud_assets(&g_app.renderer, &g_app.archives);

    /* Loading-screen background MM220.PCX (OLPAL palette) + progress callback. */
    {
        u32 sz = 0;
        const u8 *d = archives_get(&g_app.archives, "MM220.PCX", &sz);
        if (d && sz) {
            u32 w = 0, h = 0;
            /* MM220.PCX is a standalone image with its OWN embedded palette —
             * decode with that, not the level/OLPAL palette. */
            u8 *rgba = pcx_decode_rgba(d, sz, &w, &h);
            if (rgba) {
                g_app.loading_bg_tex =
                    renderer_upload_texture(&g_app.renderer, "mm220_loadscreen", rgba, w, h);
                free(rgba);
            }
        }
        world_set_loading_cb(loading_progress_cb);
    }

    /* Global (level-independent) sound effects. */
    load_sfx();
    load_sfx_rest();
    load_sounds_lst();
    entity_set_sfx_callback(scenery_play_sfx);
    projectile_init(&g_app.projectiles);
    g_app.projectiles.on_explode = proj_on_explode;
    g_app.projectiles.play_sfx   = proj_play_sfx;
    /* Projectile sprites need the level palette: loaded in load_level_runtime */

    /* Front-end menu. Start at the menu unless a level was given on the command
     * line (dev / --check), in which case go straight into that level. */
    menu_init(&g_app.menu);
    menu_load_assets(&g_app.menu, &g_app.archives, &g_app.renderer, g_app.sfx_weapon_switch);
    /* Menu navigation sounds (blip = move, SELECT = activate). */
    g_app.menu.sfx_nav    = sfx_load("blip1.wav");
    g_app.menu.sfx_select = sfx_load("SELECT.wav");
    g_app.menu.sfx_back   = sfx_load("blip3.wav");
    /* Apply persisted volumes + difficulty. */
    audio_set_music_volume(&g_app.audio, settings_gain(&g_settings, VOL_MUSIC));
    audio_set_sfx_volume(&g_app.audio,   settings_gain(&g_settings, VOL_EFFECTS));
    g_app.difficulty = g_settings.difficulty;
    campaign_parse();   /* build the story level order from OUTLAWS.RCS */
    /* Go straight into a level only when one was requested on the command line
     * (or --check/--screenshot); otherwise show the front-end menu. */
    if (!g_app.force_menu &&
        (g_app.level_from_cli || g_app.check_mode || g_app.screenshot_frames > 0)) {
        if (!load_level_runtime(g_app.level_name)) return false;
        g_app.in_menu = false;
        g_app.menu.screen = MENU_INGAME;
    } else {
        g_app.in_menu = true;   /* show the front-end menu */
    }

    g_app.running = true;
    OL_LOG("Ready. Click window to capture mouse, Esc to release/quit.\n");
    OL_LOG("WASD: move  Mouse: look  LMB: fire  1-9: weapon  hold R: reload  F5: reload level\n");
    return true;
}

/* -------------------------------------------------------------------------
 * Shoot: fire current weapon from player's eye position
 * ---------------------------------------------------------------------- */
/* Shatter any breakable glass window the shot passes through. Returns true if
 * a window was broken (and rebuilds the level mesh). The bullet continues on. */
static bool break_glass_windows(Vec3 eye, Vec3 dir) {
    LvtLevel *lvt = &g_app.world.lvt;
    f32 ox = eye.x, oz = eye.z, dx = dir.x, dz = dir.z;
    f32 best_t = 1e18f;
    LvtWall *best = NULL, *best_mirror = NULL;

    for (u32 si = 0; si < lvt->sector_count; si++) {
        LvtSector *sec = &lvt->sectors[si];
        for (u32 wi = 0; wi < sec->wall_count; wi++) {
            LvtWall *w = &sec->walls[wi];
            if (!w->is_window || w->window_broken) continue;
            if (w->v1 < 0 || w->v2 < 0 ||
                w->v1 >= (i32)sec->vertex_count || w->v2 >= (i32)sec->vertex_count)
                continue;
            f32 ax = sec->vertices[w->v1].x, az = sec->vertices[w->v1].y;
            f32 bx = sec->vertices[w->v2].x, bz = sec->vertices[w->v2].y;
            /* Ray (O + t*D) vs segment (A + s*(B-A)), solve in 2D (x,z). */
            f32 ex = bx - ax, ez = bz - az;
            f32 denom = dx * ez - dz * ex;
            if (fabsf(denom) < 1e-6f) continue;
            f32 t = ((ax - ox) * ez - (az - oz) * ex) / denom;
            f32 s = ((ax - ox) * dz - (az - oz) * dx) / denom;
            if (t <= 0.1f || t >= best_t || s < 0.0f || s > 1.0f) continue;
            /* Check hit height is within the window opening. */
            f32 hy = eye.y + dir.y * t;
            LvtSector *adj = (w->adjoin >= 0 && w->adjoin < (i32)lvt->sector_count)
                                   ? &lvt->sectors[w->adjoin] : NULL;
            f32 ob = adj ? (adj->floor_y > sec->floor_y ? adj->floor_y : sec->floor_y)
                         : sec->floor_y;
            f32 ot = adj ? (adj->ceil_y  < sec->ceil_y  ? adj->ceil_y  : sec->ceil_y)
                         : sec->ceil_y;
            if (hy < ob - 2.0f || hy > ot + 2.0f) continue;
            best_t = t; best = w; best_mirror = NULL;
            if (adj && w->mirror >= 0 && w->mirror < (i32)adj->wall_count)
                best_mirror = &adj->walls[w->mirror];
        }
    }

    if (!best) return false;
    best->window_broken = true;  best->break_time = 0.0f;
    if (best_mirror) { best_mirror->window_broken = true; best_mirror->break_time = 0.0f; }
    audio_play(&g_app.audio, g_app.sfx_glass_break);
    /* Start the shatter animation (main loop advances break_time + rebuilds). */
    g_app.window_anim_timer = WINDOW_BREAK_DURATION;
    renderer_build_level(&g_app.renderer, &g_app.world.lvt, &g_app.world.inf);
    return true;
}

/* Raycast the shot against every wall; fire SHOOT-masked INF triggers in the
 * sector whose wall the shot first hits (shootable switches / targets). */
static void fire_shoot_triggers(Vec3 eye, Vec3 dir) {
    LvtLevel *lvt = &g_app.world.lvt;
    f32 ox = eye.x, oz = eye.z, dx = dir.x, dz = dir.z;
    f32 best_t = 1e18f; i32 best_sec = -1; i32 best_adj = -1;
    for (u32 si = 0; si < lvt->sector_count; si++) {
        LvtSector *sec = &lvt->sectors[si];
        for (u32 wi = 0; wi < sec->wall_count; wi++) {
            LvtWall *w = &sec->walls[wi];
            if (w->v1 < 0 || w->v2 < 0 ||
                w->v1 >= (i32)sec->vertex_count || w->v2 >= (i32)sec->vertex_count) continue;
            f32 ax = sec->vertices[w->v1].x, az = sec->vertices[w->v1].y;
            f32 bx = sec->vertices[w->v2].x, bz = sec->vertices[w->v2].y;
            f32 ex = bx - ax, ez = bz - az;
            f32 denom = dx * ez - dz * ex;
            if (fabsf(denom) < 1e-6f) continue;
            f32 t = ((ax - ox) * ez - (az - oz) * ex) / denom;
            f32 s = ((ax - ox) * dz - (az - oz) * dx) / denom;
            if (t <= 0.1f || t >= best_t || s < 0.0f || s > 1.0f) continue;
            f32 hy = eye.y + dir.y * t;
            if (hy < sec->floor_y - 1.0f || hy > sec->ceil_y + 1.0f) continue;
            best_t = t; best_sec = (i32)si; best_adj = w->adjoin;
        }
    }
    if (best_sec >= 0) {
        inf_fire_triggers(&g_app.world.inf, (u32)best_sec, INF_EVENT_SHOOT);
        if (best_adj >= 0 && best_adj < (i32)lvt->sector_count)
            inf_fire_triggers(&g_app.world.inf, (u32)best_adj, INF_EVENT_SHOOT);
    }
}

/* Uniform random in [0,1) */
static f32 frand01(void) { return (f32)rand() / ((f32)RAND_MAX + 1.0f); }

/* Trigger the view-model fire animation for the current weapon. */
static void start_fire_anim(int mode) {
    g_app.player.fire_anim = true;
    g_app.player.fire_alt = (mode == 1);
    g_app.player.fire_anim_timer = 0.0f;
    int wi = (int)g_app.player.weapons.current;
    u32 total_ms = 0;
    if (mode == 1) {
        for (u32 f = 0; f < g_app.renderer.weapon_fire2_frame_count[wi]; f++)
            total_ms += g_app.renderer.weapon_fire2_dt[wi][f];
    } else {
        for (u32 f = 0; f < g_app.renderer.weapon_fire_frame_count[wi]; f++)
            total_ms += g_app.renderer.weapon_fire_dt[wi][f];
    }
    g_app.player.fire_anim_dur = total_ms > 0 ? (f32)total_ms / 1000.0f : 0.25f;
}

/* One hitscan trace with the exact shot semantics (Weapon_FireTick@0x471090 /
 * Shot_Execute@0x458050): per-shot damage roll ±10%, aim error, distance roll;
 * targets = entities AND resting ground dynamite (shot detonates it). */
static void fire_one_trace(Vec3 eye, f32 yaw, f32 pitch, f32 max_dist,
                           f32 dmg, bool play_hit_snd) {
    f32 cp = cosf(pitch), sp = sinf(pitch);
    Vec3 dir = { cosf(yaw)*cp, sp, sinf(yaw)*cp };

    break_glass_windows(eye, dir);
    fire_shoot_triggers(eye, dir);
    /* Shooting an OFFICE mission poster (an interaction LINE trigger) launches
     * its mission. Match any interaction bit (nudge-front/back or shoot). */
    inf_fire_line_ray(&g_app.world.inf, &g_app.world.lvt,
                      eye.x, eye.z, dir.x, dir.z, 512.0f,
                      INF_EVENT_ENTER | INF_EVENT_NUDGE | INF_EVENT_SHOOT);

    f32 de, dp;
    int ei = entity_raycast(&g_app.world.entities, &g_app.world.lvt,
                            eye, dir, max_dist, &de);
    int pi = projectile_raycast(&g_app.projectiles, eye, dir, max_dist, &dp);
    if (pi >= 0 && (ei < 0 || dp < de)) {
        projectile_damage(&g_app.projectiles, pi);   /* ground dynamite! */
        return;
    }
    if (ei >= 0) {
        const Entity *e = &g_app.world.entities.entities[ei];
        i32 idmg = (i32)(dmg + 0.5f);
        if (idmg < 1) idmg = 1;
        bool died = entity_damage(&g_app.world.entities, ei, idmg);
        if (play_hit_snd)
            audio_play(&g_app.audio, died ? e->sfx_die : e->sfx_hit);
    }
}

/* Rolled max trace length (skipped when scoped → full RANGE):
 * 1-in-8: 0.5·EFF + rand·0.5·EFF; else EFF + rand·(RANGE−EFF). */
static f32 roll_max_dist(f32 eff, f32 range) {
    if ((rand() & 7) == 0)
        return 0.5f * eff + frand01() * 0.5f * eff;
    return eff + frand01() * (range - eff);
}

/* Melee strike (Weapon_MeleeStrike @0x470d60): THREE hitscan traces at
 * yaw −S, 0, +S (S = SPREAD arc), each with the distance roll. */
static void do_melee(int mode) {
    const WeaponDef *def = &g_weapon_defs[g_app.player.weapons.current];
    Vec3 eye = player_eye_pos(&g_app.player);
    f32 dmg_base = mode ? def->damage_2 : def->damage_1;
    f32 S   = (mode ? def->spread_2 : def->spread_1) * OL_DEG2RAD;
    f32 eff = mode ? def->effrange_2 : def->effrange_1;
    f32 rng = mode ? def->range_2 : def->range_1;
    f32 offs[3] = { -S, 0.0f, S };
    for (int i = 0; i < 3; i++) {
        f32 dmg = dmg_base * (0.9f + frand01() * 0.2f);
        fire_one_trace(eye, g_app.player.yaw + offs[i], g_app.player.pitch,
                       roll_max_dist(eff, rng), dmg, i == 1);
    }
}

/* Full ranged fire for the current weapon in `mode` (0/1).
 * DB/sawed-off primary consumes 2 rounds = both barrels (2× pellets). */
static void do_player_fire(int mode) {
    WeaponState *ws = &g_app.player.weapons;
    const WeaponDef *def = &g_weapon_defs[ws->current];

    i32 rounds = 1;
    if ((ws->current == WEAPON_DBL_SHOTGUN || ws->current == WEAPON_SAW_GUN) &&
        mode == 0)
        rounds = 2;

    i32 fired = weapon_consume(ws, rounds, mode);
    if (fired <= 0) return;

    u32 snd = mode ? g_app.sfx_weapon_alt[ws->current]
                   : g_app.sfx_weapon[ws->current];
    if (snd) audio_play(&g_app.audio, snd);
    start_fire_anim(mode);

    if (def->melee) { do_melee(mode); return; }

    Vec3 eye = player_eye_pos(&g_app.player);
    bool scoped = ws->scope_active;
    f32 dmg_base = mode ? def->damage_2   : def->damage_1;
    f32 spread   = mode ? def->spread_2   : def->spread_1;
    f32 eff      = mode ? def->effrange_2 : def->effrange_1;
    f32 rng      = mode ? def->range_2    : def->range_1;
    /* Aim error grows when hurt: Veff = VARIANCE + (100−health)·0.05 deg */
    f32 var = (mode ? def->variance_2 : def->variance_1)
              + (100.0f - (f32)g_app.player.health) * 0.05f;

    for (i32 s = 0; s < fired; s++) {
        for (i32 p = 0; p < def->pellets; p++) {
            f32 yaw   = g_app.player.yaw;
            f32 pitch = g_app.player.pitch;
            if (!scoped) {
                yaw   += (frand01() * 2.0f - 1.0f) * var * OL_DEG2RAD;
                pitch += (frand01() * 2.0f - 1.0f) * var * OL_DEG2RAD;
            }
            if (spread > 0.0f) {
                yaw   += (frand01() * 2.0f - 1.0f) * spread * OL_DEG2RAD;
                pitch += (frand01() * 2.0f - 1.0f) * spread * OL_DEG2RAD;
            }
            f32 dist = scoped ? rng : roll_max_dist(eff, rng);
            f32 dmg  = dmg_base * (0.9f + frand01() * 0.2f);
            fire_one_trace(eye, yaw, pitch, dist, dmg, s == 0 && p == 0);
        }
    }
}

/* -------------------------------------------------------------------------
 * Explosion (pdynam): people RADIUS 50 / DAMAGE 12 with LOS + quadratic
 * falloff (Explosion_DamageObject @0x45a270); concussion advances scenery;
 * WALL_RADIUS 15 breaks windows and fires wall INF triggers.
 * ---------------------------------------------------------------------- */
static void explosion_at(Vec3 pos) {
    const LvtLevel *lvt = &g_app.world.lvt;

    /* Visual + spawn FX */
    if (g_app.fx_boom_n)
        projectile_spawn_fx(&g_app.projectiles,
                            (Vec3){ pos.x, pos.y - g_app.boom_h * 0.3f, pos.z },
                            g_app.fx_boom, g_app.fx_boom_n, g_app.fx_boom_dt,
                            g_app.boom_w, g_app.boom_h);

    /* Entities (enemies): LOS + falloff */
    EntityList *el = &g_app.world.entities;
    for (u32 i = 0; i < el->count; i++) {
        Entity *e = &el->entities[i];
        if (!e->active) continue;
        if (e->kind != ENTITY_ENEMY && !e->is_scenery) continue;
        Vec3 c = { e->pos.x, e->pos.y + e->sprite_h * 0.5f, e->pos.z };
        f32 dx = c.x - pos.x, dy = c.y - pos.y, dz = c.z - pos.z;
        f32 d = sqrtf(dx*dx + dy*dy + dz*dz);
        if (d > PROJ_TNT_BLAST_R) continue;
        if (!collision_has_los(lvt, pos.x, pos.z, pos.y, c.x, c.z, c.y))
            continue;
        f32 f = (d <= 0.25f * PROJ_TNT_BLAST_R) ? 1.0f
                : ((PROJ_TNT_BLAST_R - d) / (0.75f * PROJ_TNT_BLAST_R));
        if (f < 0.0f) f = 0.0f;
        f *= f;   /* quadratic falloff */
        i32 dmg = (i32)(PROJ_TNT_DMG * f * (0.9f + frand01() * 0.2f) + 0.5f);
        if (dmg > 0) entity_damage(el, (int)i, dmg);
    }

    /* Player */
    {
        Vec3 pc = player_eye_pos(&g_app.player);
        f32 dx = pc.x - pos.x, dy = pc.y - pos.y, dz = pc.z - pos.z;
        f32 d = sqrtf(dx*dx + dy*dy + dz*dz);
        if (d <= PROJ_TNT_BLAST_R &&
            collision_has_los(lvt, pos.x, pos.z, pos.y, pc.x, pc.z, pc.y)) {
            f32 f = (d <= 0.25f * PROJ_TNT_BLAST_R) ? 1.0f
                    : ((PROJ_TNT_BLAST_R - d) / (0.75f * PROJ_TNT_BLAST_R));
            if (f < 0.0f) f = 0.0f;
            f *= f;
            i32 dmg = (i32)(PROJ_TNT_DMG * f * (0.9f + frand01() * 0.2f) + 0.5f);
            if (dmg > 0) player_damage(&g_app.player, dmg);
        }
    }

    /* Concussion (msg 0x7D3): nudge scenery in the blast (no direction). */
    entity_nudge(el, pos, (Vec3){0,0,0}, PROJ_TNT_BLAST_R);

    /* WALL_RADIUS 15: break glass windows + wall INF SHOOT triggers */
    for (u32 si = 0; si < lvt->sector_count; si++) {
        LvtSector *sec = (LvtSector *)&lvt->sectors[si];
        for (u32 wi = 0; wi < sec->wall_count; wi++) {
            LvtWall *w = &sec->walls[wi];
            if (!w->is_window || w->window_broken) continue;
            if (w->v1 < 0 || w->v2 < 0) continue;
            f32 mx = (sec->vertices[w->v1].x + sec->vertices[w->v2].x) * 0.5f;
            f32 mz = (sec->vertices[w->v1].y + sec->vertices[w->v2].y) * 0.5f;
            f32 dx = mx - pos.x, dz = mz - pos.z;
            if (dx*dx + dz*dz > PROJ_TNT_WALL_R * PROJ_TNT_WALL_R) continue;
            w->window_broken = true;
            if (g_app.sfx_glass_break) audio_play(&g_app.audio, g_app.sfx_glass_break);
        }
    }
    {
        int si = collision_find_sector(lvt, pos.x, pos.z, 0);
        if (si >= 0 && si < (i32)lvt->sector_count) {
            inf_fire_triggers(&g_app.world.inf, (u32)si, INF_EVENT_SHOOT);
            const LvtSector *sec = &lvt->sectors[si];
            for (u32 wi = 0; wi < sec->wall_count; wi++) {
                i32 adj = sec->walls[wi].adjoin;
                if (adj >= 0 && adj < (i32)lvt->sector_count)
                    inf_fire_triggers(&g_app.world.inf, (u32)adj, INF_EVENT_SHOOT);
            }
        }
    }
    /* Rebuild geometry if windows broke */
    renderer_build_level(&g_app.renderer, &g_app.world.lvt, &g_app.world.inf);
}

static void proj_on_explode(Vec3 pos) { explosion_at(pos); }
static void proj_play_sfx(const char *name, Vec3 pos) {
    (void)pos;
    if (strcmp(name, "explode") == 0 && g_app.sfx_explode)
        audio_play(&g_app.audio, g_app.sfx_explode);
    else if (strcmp(name, "fuse") == 0 && g_app.sfx_fuse)
        audio_play(&g_app.audio, g_app.sfx_fuse);
}

/* Throw a projectile from the player's hands (y + 0.85·height, inherits
 * the player velocity; power = clamp(heldSeconds, 0.5, 1.0)). */
static void player_throw(ProjKind kind, f32 power, bool lit, f32 fuse) {
    Player *pl = &g_app.player;
    Vec3 origin = { pl->pos.x,
                    pl->pos.y + 0.85f * player_collision_height(pl),
                    pl->pos.z };
    Vec3 inherit = { pl->vel.x, 0.0f, pl->vel.z };
    Projectile *p = projectile_throw(&g_app.projectiles, kind, origin,
                                     pl->yaw, pl->pitch, inherit,
                                     power, lit, fuse);
    if (p) {
        if (kind == PROJ_KNIFE) {
            p->tdo = g_app.tdo_knife;
            p->tex = g_app.tex_knife_proj; p->w = g_app.knife_pw; p->h = g_app.knife_ph;
        } else {
            p->tdo = g_app.tdo_tnt;
            p->tex = g_app.tex_tnt_proj; p->w = g_app.tnt_pw; p->h = g_app.tnt_ph;
        }
        if (p->tdo >= 0) p->tex = 0;   /* model instead of billboard */
    }
}

/* Thrown-weapon input (knife & dynamite; button tables + cook mechanics —
 * Weapon_KnifeHandler @0x46d970, Weapon_TNTHandler @0x46d440). */
static void handle_thrown_weapon(bool lmb_press, bool lmb_held,
                                 bool rmb_press, f32 dt) {
    WeaponState *ws = &g_app.player.weapons;

    if (ws->current == WEAPON_KNIFE) {
        /* LMB = throw (cooked 0.5–1.0), RMB = jab (free melee) */
        if (lmb_press && !ws->cooking && ws->fire_cooldown <= 0.0f &&
            ws->ammo[WEAPON_KNIFE] > 0) {
            ws->cooking = true;
            ws->cook_time = 0.0f;
        }
        if (ws->cooking) {
            if (lmb_held) {
                ws->cook_time += dt;
            } else {
                f32 power = OL_CLAMP(ws->cook_time, 0.5f, 1.0f);
                ws->cooking = false;
                if (ws->ammo[WEAPON_KNIFE] > 0) {
                    ws->ammo[WEAPON_KNIFE]--;
                    player_throw(PROJ_KNIFE, power, false, 0.0f);
                    audio_play(&g_app.audio, g_app.sfx_weapon[WEAPON_KNIFE]);
                    start_fire_anim(0);
                    ws->fire_cooldown = 0.5f;
                    if (ws->ammo[WEAPON_KNIFE] <= 0)
                        weapon_cycle_next(ws);   /* out of knives → switch */
                }
            }
        }
        if (rmb_press && ws->fire_cooldown <= 0.0f) {
            audio_play(&g_app.audio, g_app.sfx_weapon_alt[WEAPON_KNIFE]);
            start_fire_anim(1);
            do_melee(1);
            ws->fire_cooldown = 0.5f;
        }
        return;
    }

    /* DYNAMITE — INVERTED buttons: LMB = throw (mode 1), RMB = light (mode 0) */
    if (rmb_press && !ws->holding_lit && ws->fire_cooldown <= 0.0f &&
        ws->ammo[WEAPON_DYNAMITE] > 0) {
        ws->ammo[WEAPON_DYNAMITE]--;
        ws->holding_lit = true;
        ws->lit_fuse = PROJ_TNT_FUSE;   /* 4.5 s from LIGHTING */
        if (g_app.sfx_fuse) audio_play(&g_app.audio, g_app.sfx_fuse);
        start_fire_anim(0);             /* FIRE_CHOR_1 = light animation */
        ws->fire_cooldown = 0.4f;
    }
    /* A held lit stick keeps burning — and explodes in your hand. */
    if (ws->holding_lit) {
        ws->lit_fuse -= dt;
        if (ws->lit_fuse <= 0.0f) {
            ws->holding_lit = false;
            ws->cooking = false;
            if (g_app.sfx_explode) audio_play(&g_app.audio, g_app.sfx_explode);
            explosion_at(g_app.player.pos);
            projectile_chain(&g_app.projectiles, g_app.player.pos, PROJ_TNT_BLAST_R);
        }
    }
    if (lmb_press && !ws->cooking && ws->fire_cooldown <= 0.0f &&
        (ws->holding_lit || ws->ammo[WEAPON_DYNAMITE] > 0)) {
        ws->cooking = true;
        ws->cook_time = 0.0f;
    }
    if (ws->cooking) {
        if (lmb_held) {
            ws->cook_time += dt;
        } else {
            f32 power = OL_CLAMP(ws->cook_time, 0.5f, 1.0f);
            ws->cooking = false;
            if (ws->holding_lit) {
                /* Throw the lit stick — the fuse keeps its remaining time */
                player_throw(PROJ_DYNAMITE, power, true, ws->lit_fuse);
                ws->holding_lit = false;
            } else if (ws->ammo[WEAPON_DYNAMITE] > 0) {
                ws->ammo[WEAPON_DYNAMITE]--;
                player_throw(PROJ_DYNAMITE, power, false, 0.0f);
            } else {
                return;
            }
            audio_play(&g_app.audio, g_app.sfx_weapon_alt[WEAPON_DYNAMITE]);
            start_fire_anim(1);
            ws->fire_cooldown = 0.6f;
            if (ws->ammo[WEAPON_DYNAMITE] <= 0 && !ws->holding_lit)
                weapon_cycle_next(ws);
        }
    }
}

/* Fill g_debug.look_* : describe what the crosshair points at (for the debug
 * "Looking At" panel). Walks the view ray through the sector graph to the
 * first solid wall (or a portal/door), and checks for an enemy under it. */
static void debug_compute_lookat(void) {
    const LvtLevel *lvt = &g_app.world.lvt;
    Vec3 eye = player_eye_pos(&g_app.player);
    f32 cp = cosf(g_app.player.pitch), sp = sinf(g_app.player.pitch);
    f32 cy = cosf(g_app.player.yaw),   sy = sinf(g_app.player.yaw);
    Vec3 dir = { cy*cp, sp, sy*cp };

    g_debug.look_desc[0] = '\0';
    g_debug.look_dist = 0; g_debug.look_sector = -1;
    g_debug.look_is_door = 0; g_debug.look_enemy = -1;

    /* Enemy under the crosshair? */
    f32 edist;
    int ei = entity_raycast(&g_app.world.entities, lvt, eye, dir, 512.0f, &edist);

    /* Walk the ray to the first wall hit. */
    int cur = g_app.player.sector_idx;
    if (cur < 0 || cur >= (i32)lvt->sector_count) cur = collision_find_sector(lvt, eye.x, eye.z, 0);
    f32 cx = eye.x, cz = eye.z, travelled = 0.0f;
    f32 rlen = sqrtf(dir.x*dir.x + dir.z*dir.z);
    char walldesc[160]; walldesc[0] = '\0';
    f32 walldist = 1e9f; int wallsec = -1; int wallisdoor = 0;
    if (rlen > 1e-4f) {
        for (int step = 0; step < 64 && travelled < 512.0f; step++) {
            if (cur < 0 || cur >= (i32)lvt->sector_count) break;
            const LvtSector *sec = &lvt->sectors[cur];
            f32 best_t = 1e30f; i32 best_wi = -1;
            for (u32 wi = 0; wi < sec->wall_count; wi++) {
                const LvtWall *w = &sec->walls[wi];
                if (w->v1 < 0 || w->v2 < 0) continue;
                f32 ax = sec->vertices[w->v1].x, az = sec->vertices[w->v1].y;
                f32 bx = sec->vertices[w->v2].x, bz = sec->vertices[w->v2].y;
                f32 edx = bx - ax, edz = bz - az;
                f32 den = dir.x * edz - dir.z * edx;
                if (fabsf(den) < 1e-8f) continue;
                f32 t = ((ax - cx) * edz - (az - cz) * edx) / den;
                f32 u = ((ax - cx) * dir.z - (az - cz) * dir.x) / den;
                if (t < 1e-4f || u < -0.001f || u > 1.001f) continue;
                if (t < best_t) { best_t = t; best_wi = (i32)wi; }
            }
            if (best_wi < 0) break;
            const LvtWall *bw = &sec->walls[best_wi];
            travelled = best_t * rlen;
            walldist = travelled; wallsec = cur;
            if (bw->adjoin < 0 || bw->adjoin >= (i32)lvt->sector_count) {
                snprintf(walldesc, sizeof(walldesc), "Solid wall (sector %d)", cur);
                break;
            }
            /* Is this wall a door (an INF morph/door on the adjoin sector)? */
            int nk = INF_KEY_NONE;
            const char *dn = inf_door_name_for_sector(&g_app.world.inf, (u32)bw->adjoin, &nk);
            if (dn) {
                wallisdoor = 1;
                if (nk != INF_KEY_NONE)
                    snprintf(walldesc, sizeof(walldesc), "Door: %s (needs %s)",
                             dn, inf_key_name(nk));
                else
                    snprintf(walldesc, sizeof(walldesc), "Door: %s", dn);
                break;
            }
            if (bw->is_window && !bw->window_broken) {
                snprintf(walldesc, sizeof(walldesc), "Glass window (sector %d)", cur);
                break;
            }
            /* Open portal — continue into the adjoin. */
            wallisdoor = 1;
            snprintf(walldesc, sizeof(walldesc), "Portal -> sector %d", bw->adjoin);
            cx = eye.x + dir.x * best_t; cz = eye.z + dir.z * best_t;
            cur = bw->adjoin;
        }
    }

    /* Prefer the closer of enemy vs wall. */
    if (ei >= 0 && edist < walldist) {
        const Entity *e = &g_app.world.entities.entities[ei];
        snprintf(g_debug.look_desc, sizeof(g_debug.look_desc),
                 "Enemy: %s  (hp %d)", e->type_name, e->health);
        g_debug.look_enemy = ei;
        g_debug.look_dist = edist;
        g_debug.look_sector = cur;
    } else if (wallsec >= 0) {
        snprintf(g_debug.look_desc, sizeof(g_debug.look_desc), "%s", walldesc);
        g_debug.look_dist = walldist;
        g_debug.look_sector = wallsec;
        g_debug.look_is_door = wallisdoor;
    }
}

/* -------------------------------------------------------------------------
 * Game loop
 * ---------------------------------------------------------------------- */
static void app_run(void) {
    u64 last_ticks = SDL_GetTicks64();
    int frame_no = 0;

    while (g_app.running) {
        u64 now = SDL_GetTicks64();
        f32 dt  = (f32)(now - last_ticks) / 1000.0f;
        last_ticks = now;
        if (dt > 0.1f) dt = 0.1f;

        /* Headless screenshot: fix dt for determinism while capturing. */
        if (g_app.screenshot_frames > 0) dt = 1.0f / 60.0f;

        /* ---- Input ---- */
        input_update(&g_app.input);
        if (g_app.input.quit) { g_app.running = false; break; }

        /* ---- Window resize ---- */
        if (g_app.screenshot_frames <= 0) {   /* pinned in screenshot mode */
            int w, h;
            SDL_GetWindowSize(g_app.renderer.window, &w, &h);
            if (w != g_app.renderer.cfg.width || h != g_app.renderer.cfg.height)
                renderer_resize(&g_app.renderer, w, h);
        }

        /* Live post-processing params from the debug menu → renderer. */
        g_app.renderer.post = g_debug.postfx;

        /* ---- Front-end menu (main / mission select / options) ---- */
        if (g_app.in_menu) {
            g_app.input.suppress_capture = true;        /* don't grab on click */
            input_capture_mouse(&g_app.input, false);   /* free cursor for the menu */
            /* Populate save slots so LOAD (from the main menu) lists them. */
            if (g_app.menu.screen == MENU_LOAD || g_app.menu.screen == MENU_SAVE)
                refresh_save_slots();
            renderer_begin_frame(&g_app.renderer);
            menu_frame(&g_app.menu, &g_app.input, &g_app.renderer, &g_app.audio);
            if (g_app.menu.want_quit) { g_app.running = false; break; }
            /* VIDEO options: apply a resolution / fullscreen change. */
            if (g_app.menu.apply_video) {
                g_app.menu.apply_video = false;
                apply_video_settings();
            }
            /* Persist settings the menu changed (volumes, binds, video, ...). */
            if (g_app.menu.settings_dirty) {
                g_app.menu.settings_dirty = false;
                settings_save(&g_settings);
            }
            /* LOAD chosen from the MAIN menu → load the save and enter the game. */
            if (g_app.menu.req_load_slot >= 0 && g_app.menu.load_return == MENU_MAIN) {
                int slot = g_app.menu.req_load_slot; g_app.menu.req_load_slot = -1;
                if (do_load(slot)) {
                    g_app.in_menu = false;
                    g_app.menu.screen = MENU_INGAME;
                    input_capture_mouse(&g_app.input, true);
                }
                last_ticks = SDL_GetTicks64();
                continue;
            }
            /* STORY → begin the campaign at mission 1. */
            if (g_app.menu.start_story) {
                g_app.menu.start_story = false;
                /* Re-parse the main story order — a prior historical mission may
                 * have overwritten campaign[] with its own .rca. */
                campaign_parse();
                /* Apply the difficulty chosen on the DIFFICULTY screen. */
                g_settings.difficulty = g_app.difficulty = g_app.menu.chosen_difficulty;
                settings_save(&g_settings);
                /* Opening cinematic (op_cr.san) before mission 1, Esc to skip. */
                if (!cutscene_play(g_app.campaign_open_movie)) { g_app.running = false; break; }
                if (campaign_load(0, false)) {
                    g_app.in_menu = false;
                    g_app.menu.screen = MENU_INGAME;
                    input_capture_mouse(&g_app.input, true);
                }
                last_ticks = SDL_GetTicks64();
                continue;
            }
            /* HISTORICAL MISSION → run its own .rca mini-campaign (e.g. Civil War
             * chains civlwar1 → civlwar2). Falls back to a single level if there is
             * no .rca (Marshal Training = htrain). */
            if (g_app.menu.start_historical) {
                g_app.menu.start_historical = false;
                char base[64]; snprintf(base, sizeof(base), "%s", g_app.menu.start_level);
                g_app.menu.start_level[0] = '\0';
                g_settings.difficulty = g_app.difficulty = g_app.menu.chosen_difficulty;
                settings_save(&g_settings);
                char rca[80]; snprintf(rca, sizeof(rca), "%s.rca", base);
                bool started = false;
                if (campaign_parse_rcs(rca) > 0) {           /* multi-level .rca campaign */
                    g_app.campaign_open_movie[0] = '\0';     /* historical: no opening movie */
                    if (campaign_load(0, false)) started = true;
                } else if (load_level_runtime(base)) {       /* single level (no .rca) */
                    g_app.campaign_active = false;
                    started = true;
                } else {
                    /* base was the .rca stem but no level of that name — rebuild the
                     * main story RCS so a later STORY start still works. */
                    campaign_parse();
                }
                if (started) {
                    g_app.in_menu = false;
                    g_app.menu.screen = MENU_INGAME;
                    input_capture_mouse(&g_app.input, true);
                } else {
                    campaign_parse();   /* restore main story order on failure */
                }
                last_ticks = SDL_GetTicks64();
                continue;
            }
            /* MULTIPLAYER / poster → load a single map (no campaign). */
            if (g_app.menu.start_level[0]) {
                char lvl[64]; snprintf(lvl, sizeof(lvl), "%s", g_app.menu.start_level);
                g_app.menu.start_level[0] = '\0';
                /* Apply the chosen difficulty (from the DIFFICULTY screen). */
                g_settings.difficulty = g_app.difficulty = g_app.menu.chosen_difficulty;
                settings_save(&g_settings);
                if (load_level_runtime(lvl)) {
                    g_app.campaign_active = false;
                    g_app.in_menu = false;
                    g_app.menu.screen = MENU_INGAME;
                    input_capture_mouse(&g_app.input, true);
                }
            }
            renderer_post_resolve(&g_app.renderer);  /* keep menu debug UI crisp */
            debug_ui_render();
            renderer_end_frame(&g_app.renderer);
            screenshot_tick(&frame_no);
            continue;
        }

        /* ---- Return to the front-end menu (F10) ---- */
        if (input_key_pressed(&g_app.input, SDL_SCANCODE_F10)) {
            g_app.in_menu = true;
            g_app.menu.screen = MENU_MAIN;
            g_app.menu.sel = 0;
            input_capture_mouse(&g_app.input, false);
            continue;
        }

        /* ---- Level reload (F5) ---- */
        if (input_key_pressed(&g_app.input, SDL_SCANCODE_F5)) {
            OL_LOG("Reloading level '%s'...\n", g_app.level_name);
            load_level_runtime(g_app.level_name);
        }

        /* ---- Mission-complete transition (campaign auto-advance) ---- */
        if (g_app.level_done) {
            g_app.level_done_timer -= dt;
            if (g_app.level_done_timer <= 0.0f) {
                g_app.level_done = false;
                int next = g_app.campaign_idx + 1;
                /* Transition cinematic for the level just completed (the last
                 * level's movie is the end-credits reel). Esc skips. */
                const char *mv = g_app.campaign_movie[g_app.campaign_idx];
                if (mv[0] && !cutscene_play(mv)) { g_app.running = false; break; }
                if (next < g_app.campaign_count) {
                    campaign_load(next, true);   /* carry the loadout forward */
                    last_ticks = SDL_GetTicks64();
                    continue;
                } else {
                    /* Campaign finished → return to the front-end menu. */
                    g_app.campaign_active = false;
                    g_app.in_menu = true;
                    g_app.menu.screen = MENU_MAIN;
                    g_app.menu.sel = 0;
                    input_capture_mouse(&g_app.input, false);
                    continue;
                }
            } else {
                dt = 0.0f;            /* freeze while the card is up */
                goto render_frame;
            }
        }

        /* Debug: OL_DEBUG_UI opens the debug panel once (for headless preview). */
        static bool s_dbg_once = false;
        if (!s_dbg_once && getenv("OL_DEBUG_UI")) { s_dbg_once = true; g_debug.visible = true; }

        /* ---- Pause menu (ESC) ---- */
        static bool s_test_pause = false;
        if (!g_app.paused && !s_test_pause && getenv("OL_TEST_PAUSE")) {
            s_test_pause = true;
            g_app.paused = true; menu_pause_open(&g_app.menu); refresh_save_slots();
            if (atoi(getenv("OL_TEST_PAUSE")) == 2) g_app.menu.screen = MENU_SAVE;
        }
        if (!g_app.paused &&
            input_key_pressed(&g_app.input, SDL_SCANCODE_ESCAPE)) {
            g_app.paused = true;
            menu_pause_open(&g_app.menu);
            refresh_save_slots();
            input_capture_mouse(&g_app.input, false);
            /* Eat this ESC so the pause menu doesn't see it as "resume" the same
             * frame (which made the menu flash open then close immediately). */
            input_consume_key(&g_app.input, SDL_SCANCODE_ESCAPE);
        }
        if (g_app.paused) {
            g_app.input.suppress_capture = true;       /* free cursor for the menu */
            input_capture_mouse(&g_app.input, false);
            dt = 0.0f;               /* freeze the world behind the overlay */
            goto render_frame;
        }
        g_app.input.suppress_capture = false;

        /* ---- Respawn after death ---- */
        if (g_app.player.dead) {
            if (g_app.player.dead_timer >= RESPAWN_DELAY) {
                player_respawn(&g_app.player, g_app.world.player_start,
                               g_app.world.player_start_yaw);
                relocate_player_sector();   /* anchor sector (else falls in) */
            }
            /* Still tick time even while dead */
            g_app.player.dead_timer += dt;
            /* Render dead screen and continue */
            goto render_frame;
        }

        /* ---- Player update ---- */
        g_app.pending_player_damage = 0;
        Vec3 pre_move_pos = g_app.player.pos;  /* Save position before movement */
        WeaponType cur_weap_before = g_app.player.weapons.current;
        /* While the debug UI (INSERT) is open the cursor is free for ImGui — the
         * game must NOT consume mouse-look / fire / movement, else clicking a
         * button fires the gun and dragging spins the camera. Feed player_update
         * a neutral (zeroed) input so the player is frozen. */
        static const InputState s_null_input = {0};
        const InputState *game_input = g_debug.visible ? &s_null_input : &g_app.input;
        /* Current sector's LVT friction (default 1.0) for the movement model */
        f32 sec_friction = 1.0f;
        if (g_app.player.sector_idx >= 0 &&
            g_app.player.sector_idx < (i32)g_app.world.lvt.sector_count)
            sec_friction = g_app.world.lvt.sectors[g_app.player.sector_idx].friction;
        /* Reload while R is HELD (bullet-by-bullet, faithful): weapon_update runs
         * inside player_update and loads one round per RELOAD_CHOR loop while
         * reload_held is set; releasing R stops it. */
        g_app.player.weapons.reload_held = input_key_held(game_input, (SDL_Scancode)g_settings.bind[BIND_RELOAD]);
        bool fired = player_update(&g_app.player, game_input, dt, sec_friction);
        /* Per-round reload sound (each chambered round). */
        if (g_app.player.weapons.reload_click &&
            g_app.sfx_reload_w[g_app.player.weapons.current])
            audio_play(&g_app.audio, g_app.sfx_reload_w[g_app.player.weapons.current]);
        if (g_app.force_crouch) { /* debug: force the crouch eye height */
            g_app.player.crouching = true;
            g_app.player.eye_height = PLAYER_CROUCH_EYE;
        }
        /* Screenshot mode: spawn at fixed coordinates. */
        if (g_app.spawn_set && !g_app.spawn_done) {
            g_app.spawn_done = true;
            g_app.player.pos.x = g_app.spawn_x;
            g_app.player.pos.z = g_app.spawn_z;
            if (g_app.spawn_y_set) g_app.player.pos.y = g_app.spawn_y;
            else g_app.player.pos.y += 1.0f; /* small margin; gravity settles to floor */
            g_app.player.yaw = g_app.spawn_yaw_deg * OL_DEG2RAD;
            if (getenv("OL_PITCH")) g_app.player.pitch = (f32)atof(getenv("OL_PITCH")) * OL_DEG2RAD;
            if (getenv("OL_TNTDROP")) { /* debug: toss a dynamite + knife ahead */
                g_app.player.weapons.has_weapon[WEAPON_DYNAMITE] = true;
                weapon_add_ammo(&g_app.player.weapons, WEAPON_DYNAMITE, 5);
                player_throw(PROJ_DYNAMITE, 0.5f, false, 0.0f);
                player_throw(PROJ_KNIFE, 0.6f, false, 0.0f);
            }
            if (getenv("OL_SCOPE")) { /* debug: force rifle + scope view */
                g_app.player.weapons.has_weapon[WEAPON_RIFLE] = true;
                weapon_switch(&g_app.player.weapons, WEAPON_RIFLE);
                g_app.player.weapons.scope_active = true;
                renderer_set_zoom(&g_app.renderer, 2.0f);
            }
            g_app.player.sector_idx = collision_find_sector_y(
                &g_app.world.lvt, g_app.player.pos.x, g_app.player.pos.z,
                g_app.player.pos.y, PLAYER_BODY_HEIGHT, -1);
            pre_move_pos = g_app.player.pos; /* avoid a 235u phantom sweep on spawn frame */
        }
        /* Screenshot mode: teleport in front of the nearest living enemy. */
        if (g_app.screenshot_near_enemy && !g_app.near_enemy_done) {
            g_app.near_enemy_done = true;
            Entity *best = NULL; f32 bestd = 1e18f;
            for (u32 ei = 0; ei < g_app.world.entities.count; ei++) {
                Entity *e = &g_app.world.entities.entities[ei];
                if (!e->active || e->kind != ENTITY_ENEMY) continue;
                f32 d = vec3_dist(e->pos, g_app.player.pos);
                if (d < bestd) { bestd = d; best = e; }
            }
            if (best) {
                f32 back = 42.0f;
                g_app.player.pos.x = best->pos.x - back;
                g_app.player.pos.z = best->pos.z;
                g_app.player.pos.y = best->pos.y + 6.0f;
                g_app.player.yaw = 0.0f; /* face +X toward enemy */
                g_app.player.pitch = -0.06f;
            }
        }
        /* Screenshot mode: optionally give and select a specific weapon. */
        if (g_app.screenshot_weapon_set) {
            int wsel = g_app.screenshot_weapon;
            if (wsel >= 0 && wsel < WEAPON_COUNT) {
                g_app.player.weapons.has_weapon[wsel] = true;
                if (g_app.player.weapons.ammo[wsel] == 0)
                    g_app.player.weapons.ammo[wsel] = 99;
                if (g_weapon_defs[wsel].clip_size > 0 &&
                    g_app.player.weapons.clip[wsel] == 0)
                    g_app.player.weapons.clip[wsel] = g_weapon_defs[wsel].clip_size;
                weapon_switch(&g_app.player.weapons, (WeaponType)wsel);
            }
        }
        /* Screenshot mode: force a fire event periodically so the fire
         * animation is exercised in captured frames. */
        if (g_app.screenshot_firing && !fired &&
            weapon_can_fire(&g_app.player.weapons)) {
            if (weapon_fire(&g_app.player.weapons)) fired = true;
        }
        if (g_app.player.weapons.current != cur_weap_before)
            audio_play(&g_app.audio, g_app.sfx_weapon_switch);

        /* Debug: walk forward (test approaching/passing through doors). */
        if (g_app.force_walk) {
            f32 spd = 30.0f * dt;
            g_app.player.pos.x += cosf(g_app.player.yaw) * spd;
            g_app.player.pos.z += sinf(g_app.player.yaw) * spd;
            if (getenv("OL_LOG_WALK")) { static int wf=0; if((wf++%4)==0)
                OL_LOG("WALK pos=(%.1f,%.1f,%.1f) sec=%d\n", g_app.player.pos.x,
                       g_app.player.pos.y, g_app.player.pos.z, g_app.player.sector_idx); }
        }

        /* ---- Collision detection ---- */
        {
            if (!g_debug.noclip) {
            Player *pl = &g_app.player;

            /* Resolve new position against level walls using the CURRENT
             * collision height (eye + HEAD_HEIGHT): standing 6.0, crouched
             * 2.0 — the crouch-aware fit test is what lets the player crawl
             * under low floors (Sector_CanFitRecursive @0x4e6660). */
            f32 col_h = player_collision_height(pl);
            Vec3 col_from = pre_move_pos, col_to = pl->pos;
            /* Swimming at the surface: measure wall step-ups from the water
             * surface, not the sunk-in feet. A treading player floats with the
             * feet ~4.5u below the waterline, so every shore would otherwise
             * read as an unclimbable 4.5u step and you could never get out.
             * Gated to near-surface only — a deep diver must not clip through
             * underwater walls. */
            if (g_app.in_water_prev && pl->sector_idx >= 0 &&
                pl->sector_idx < (i32)g_app.world.lvt.sector_count &&
                g_app.world.lvt.sectors[pl->sector_idx].is_water) {
                f32 wf = 0.0f, wc = 256.0f;
                collision_heights(&g_app.world.lvt, pl->sector_idx,
                                  pl->pos.x, pl->pos.z, &wf, &wc);
                const LvtSector *ws = &g_app.world.lvt.sectors[pl->sector_idx];
                f32 surf = ws->water_at_ceiling ? wc : wf;
                if (surf - pl->pos.y <= pl->eye_height) {  /* treading, not deep */
                    col_from.y = surf;
                    col_to.y   = surf;
                }
            }
            Vec3 resolved = collision_resolve(
                &g_app.world.lvt, col_from, col_to,
                PLAYER_RADIUS, col_h, &pl->sector_idx);
            pl->pos.x = resolved.x;
            pl->pos.z = resolved.z;

            /* Floor / ceiling + per-sector gravity */
            f32 floor_y = 0, ceil_y = 256;
            collision_heights(&g_app.world.lvt, pl->sector_idx,
                              pl->pos.x, pl->pos.z, &floor_y, &ceil_y);
            f32 sec_gravity = -60.0f;   /* LVT GRAVITY default */
            if (pl->sector_idx >= 0 &&
                pl->sector_idx < (i32)g_app.world.lvt.sector_count)
                sec_gravity = g_app.world.lvt.sectors[pl->sector_idx].gravity;

            /* Headroom limit for standing up (@0x4431f0):
             * maxEye = min(HEIGHT, ceil-floor) - HEAD_HEIGHT. */
            pl->max_eye = OL_MIN(PLAYER_BODY_HEIGHT, ceil_y - floor_y)
                          - PLAYER_HEAD_HEIGHT;

            bool in_water = (pl->sector_idx >= 0 &&
                             pl->sector_idx < (i32)g_app.world.lvt.sector_count &&
                             g_app.world.lvt.sectors[pl->sector_idx].is_water);

            if (in_water) {
                /* ---- Swimming ----
                 * A water sector's surface is the water-textured side (the
                 * CEILING for the underwater half, else the FLOOR); the ground
                 * is the sector floor. The player treads with the eyes at the
                 * waterline and swims up (Space) / down (Ctrl); buoyancy eases
                 * toward the float level; gravity + fall damage are suspended.
                 * To LEAVE the water you either swim into a low shore (the
                 * horizontal collision above is measured from the surface, so a
                 * bank within a step of the waterline is walked onto and the
                 * ground step-band lifts you out), OR JUMP out at the surface:
                 * pressing jump launches a self-contained gravity arc (handled
                 * right here, NOT via the old airborne state machine that caused
                 * the porpoise-down / fall-through-the-map bugs). */
                const LvtSector *wsec = &g_app.world.lvt.sectors[pl->sector_idx];
                bool underwater = wsec->water_at_ceiling;
                f32 surface = underwater ? ceil_y : floor_y;
                /* Diveable bottom = the deepest water floor under this point.
                 * The surface slab's own floor IS the waterline, so we must look
                 * through to the deep volume stacked below it; else the player
                 * would be pinned ON the surface ("walking on water"). Clamped to
                 * real geometry so diving can't go under the map. */
                f32 ground = collision_water_bottom_at(&g_app.world.lvt,
                                                       pl->pos.x, pl->pos.z, surface);
                if (ground > surface) ground = surface;
                if (!g_app.in_water_prev) {           /* entered water: splash */
                    if (g_app.sfx_water) audio_play(&g_app.audio, g_app.sfx_water);
                    pl->fall_peak = 0.0f; pl->vel_y = 0.0f;
                }
                /* Idle float level: feet low enough that the eyes sit at the
                 * waterline (but never below the ground in shallow water). */
                f32 float_feet = surface - pl->eye_height + 1.0f;
                if (float_feet < ground) float_feet = ground;

                bool sw_up = input_key_held(&g_app.input, SDL_SCANCODE_SPACE);
                bool sw_dn = input_key_held(&g_app.input, SDL_SCANCODE_LCTRL) ||
                             input_key_held(&g_app.input, SDL_SCANCODE_RCTRL) ||
                             input_key_held(&g_app.input, SDL_SCANCODE_C);

                if (pl->pos.y > surface + 0.01f) {
                    /* Breached the surface (mid leap-out): a plain gravity arc,
                     * no swim control. Uses AIR gravity (-60), NOT the buoyant
                     * water-volume gravity (-10) of the current sector — else the
                     * jump-out floats up ~20u over 2s ("low-gravity fly"). When it
                     * drops back to the waterline the swim path below resumes; if
                     * a bank was reached the sector flips to non-water and the
                     * airborne branch lands the jump. Bounded above the surface
                     * → can never drive under the map. */
                    pl->vel_y += PHY_AIR_GRAVITY * dt;
                    pl->pos.y += pl->vel_y * dt;
                    if (pl->pos.y <= surface) {   /* fell back into the water */
                        pl->pos.y = surface;
                        pl->vel_y = 0.0f;
                    }
                    pl->on_ground = false;
                    pl->want_jump = false;
                    /* g_app.was_airborne stays true so a bank landing is handled
                     * by the airborne branch once in_water turns false. */
                } else {
                    /* Jump out of the water: a tap of jump at the surface, or
                     * holding it while swimming at a shore, launches from the
                     * waterline (a jump from the treading depth ~4.5u under never
                     * even breaches). Eye compensated so the snap isn't a pop. */
                    f32 hspeed2 = pl->vel.x * pl->vel.x + pl->vel.z * pl->vel.z;
                    bool near_surf = pl->pos.y >= surface - pl->eye_height;
                    bool leap = near_surf &&
                                (pl->want_jump || (sw_up && hspeed2 > 4.0f));
                    if (leap) {
                        f32 intent = pl->running ? PHY_RUN_MULT : 1.0f;
                        pl->eye_step_ofs += (pl->pos.y - surface);
                        pl->pos.y = surface;
                        pl->vel_y = PHY_JUMP_VEL *
                                    (1.0f + (intent - 1.0f) * pl->energy * 0.01f);
                        pl->pos.y += pl->vel_y * dt;      /* begin the arc */
                        g_app.was_airborne = true;
                        pl->on_ground = false;
                        if (g_app.sfx_water) audio_play(&g_app.audio, g_app.sfx_water);
                    } else {
                        if (sw_up) {
                            pl->vel_y = PHY_SWIM_VERT;             /* swim up */
                        } else if (sw_dn) {
                            pl->vel_y = -PHY_SWIM_VERT;            /* dive */
                        } else {
                            f32 d = float_feet - pl->pos.y;        /* buoyancy */
                            pl->vel_y = OL_CLAMP(d * 4.0f, -PHY_SWIM_VERT, PHY_SWIM_VERT);
                        }
                        pl->pos.y += pl->vel_y * dt;
                        /* Surface cap: swimming tops out at the waterline. */
                        if (pl->pos.y > surface) { pl->pos.y = surface; if (pl->vel_y > 0) pl->vel_y = 0; }
                        if (pl->pos.y < ground)  { pl->pos.y = ground;  if (pl->vel_y < 0) pl->vel_y = 0; }
                        pl->on_ground = false;
                        g_app.was_airborne = false;
                    }
                    pl->want_jump = false;
                }
                /* Water drag on horizontal motion (slower swimming). */
                pl->vel.x -= pl->vel.x * PHY_SWIM_DAMP * dt;
                pl->vel.z -= pl->vel.z * PHY_SWIM_DAMP * dt;
            } else {
            /* Supporting floor = the HIGHEST floor the player's radius overlaps
             * (Sector_CanFitRecursive), not just the floor under the center. This
             * keeps the player standing on narrow wall-tops/murets and lifts them
             * onto low walls as they approach, instead of sliding off. */
            floor_y = collision_support_floor(&g_app.world.lvt, pl->pos.x, pl->pos.z,
                                              pl->pos.y, PLAYER_RADIUS, pl->sector_idx, col_h);
            /* Vertical physics (Physics_ApplyGravityStep @0x4e05e0). Two
             * regimes:
             *  - AIRBORNE (jumped, or walked off a ledge taller than a step):
             *    gravity integrates the full arc down until the feet reach the
             *    floor, THEN we land. The step band must NOT cut the descent
             *    short — otherwise a short jump (apex ~3.3u, just over the 3u
             *    band) snaps straight back to the floor the instant it starts
             *    falling ("instant return to ground at the apex").
             *  - GROUNDED: the STEP_HEIGHT band glues the player to the floor
             *    so small steps/stairs are walked (not fallen); a drop bigger
             *    than a step starts a fall. */
            if (g_app.was_airborne) {
                pl->vel_y += sec_gravity * dt;
                pl->pos.y += pl->vel_y * dt;
                if (-pl->vel_y > pl->fall_peak) pl->fall_peak = -pl->vel_y;
                if (pl->pos.y <= floor_y && pl->vel_y <= 0.0f) {
                    /* Reached the floor — land. Sounds + fall damage
                     * (Player_TakeDamage @0x445920: dmg=(speed-45)*0.2). */
                    pl->pos.y = floor_y;
                    if (pl->fall_peak >= PHY_LAND_LIGHT)
                        audio_play(&g_app.audio, g_app.sfx_player_land);
                    if (pl->fall_peak > PHY_FALL_DMG_SPEED) {
                        i32 dmg = (i32)((pl->fall_peak - PHY_FALL_DMG_SPEED)
                                        * PHY_FALL_DMG_SCALE + 0.5f);
                        if (dmg > 0) player_damage(pl, dmg);
                    }
                    pl->vel_y = 0.0f;
                    pl->on_ground = true;
                    pl->fall_peak = 0.0f;
                    g_app.was_airborne = false;
                } else {
                    pl->on_ground = false;
                }
            } else {
                f32 above = pl->pos.y - floor_y;
                if (above > PHY_STEP_HEIGHT) {
                    /* Walked off a ledge taller than a step → begin falling. */
                    pl->vel_y += sec_gravity * dt;
                    pl->pos.y += pl->vel_y * dt;
                    pl->on_ground = false;
                    g_app.was_airborne = true;
                    if (-pl->vel_y > pl->fall_peak) pl->fall_peak = -pl->vel_y;
                } else {
                    /* Stay grounded: step up/down within the band. The step
                     * delta feeds the eye smoothing (STEP_SPEED easing = the
                     * smooth stair feel, @0x444056). */
                    f32 drop = pl->pos.y - floor_y;
                    if (fabsf(drop) > 0.01f && fabsf(drop) <= PHY_STEP_HEIGHT)
                        pl->eye_step_ofs += drop;
                    pl->pos.y = floor_y;
                    pl->vel_y = 0.0f;
                    pl->on_ground = true;
                    pl->fall_peak = 0.0f;
                }
            }

            /* Ceiling bump (@0x4449e9): head = feet + eye + HEAD_HEIGHT —
             * crouch-aware automatically since the eye lowers. */
            f32 head_y = pl->pos.y + col_h;
            if (head_y > ceil_y) {
                pl->pos.y = ceil_y - col_h;
                if (pl->vel_y > 0) pl->vel_y = 0;
            }
            }  /* end land/water branch */
            g_app.in_water_prev = in_water;

            /* Jump (@0x444214): grounded && vy==0; impulse JUMP_VEL ×
             * (1 + (intent-1)·energy%) — 20 walk, 30 running. */
            if (pl->want_jump && pl->on_ground && pl->vel_y == 0.0f) {
                f32 intent = pl->running ? PHY_RUN_MULT : 1.0f;
                pl->vel_y = PHY_JUMP_VEL *
                            (1.0f + (intent - 1.0f) * pl->energy * 0.01f);
                pl->on_ground = false;
                g_app.was_airborne = true;
                pl->energy = OL_CLAMP(pl->energy + PHY_E_JUMP, 25.0f, 100.0f);
            }
            pl->want_jump = false;

            /* VELOCITY_Z: wind/push sectors — apply lateral force if in one */
            if (g_app.player.sector_idx >= 0) {
                for (u32 ei = 0; ei < g_app.world.inf.count; ei++) {
                    const Elevator *el = &g_app.world.inf.elevs[ei];
                    if (!el->active) continue;
                    if (el->type != ELEV_TYPE_VELOCITY_Z) continue;
                    if (el->sector_idx != (u32)g_app.player.sector_idx) continue;
                    f32 a = el->angle_deg * (3.14159265f / 180.0f);
                    g_app.player.pos.x += cosf(a) * el->current_y * dt;
                    g_app.player.pos.z += sinf(a) * el->current_y * dt;
                    break;
                }
            }
        } else {
            /* ---- Noclip free-fly ---- */
            Player *pl = &g_app.player;
            /* Horizontal movement already integrated by player_update. Add free
             * vertical control: Space rises, Ctrl/C descends. Look direction is
             * ignored so up/down are absolute (like the original debug fly). */
            f32 fly = PHY_MAX_VEL * (pl->running ? 2.0f : 1.0f) * dt;
            if (input_key_held(&g_app.input, SDL_SCANCODE_SPACE))
                pl->pos.y += fly;
            if (input_key_held(&g_app.input, SDL_SCANCODE_LCTRL) ||
                input_key_held(&g_app.input, SDL_SCANCODE_RCTRL) ||
                input_key_held(&g_app.input, SDL_SCANCODE_C))
                pl->pos.y -= fly;
            pl->vel_y = 0.0f;
            pl->on_ground = false;
            pl->want_jump = false;
            g_app.was_airborne = false;
            /* Track which sector we're flying through so rendering/look work. */
            i32 s = collision_find_sector(&g_app.world.lvt, pl->pos.x, pl->pos.z, pl->pos.y);
            if (s >= 0) pl->sector_idx = s;
        } /* end noclip check */
        } /* end collision block */

        /* ---- Item pickup (dispatch by kind: heal / ammo / weapon / key) ---- */
        {
            PickupResult pk = entity_try_pickup_ex(&g_app.world.entities,
                                                   g_app.player.pos,
                                                   g_app.player.health, PLAYER_MAX_HEALTH);
            if (pk.got) {
                audio_play(&g_app.audio, g_app.sfx_pickup);
                switch (pk.kind) {
                case ENTITY_ITEM_HEALTH:
                    player_damage(&g_app.player, -pk.value); /* negative = heal */
                    break;
                case ENTITY_ITEM_AMMO: {
                    /* Map the OBT ground-item type to the weapon whose ammo it
                     * refills (see BGY/ground item names). */
                    WeaponType w = WEAPON_PISTOL;
                    const char *t = pk.type_name;
                    if      (strncasecmp(t, "GAMBOXC", 7) == 0) w = WEAPON_RIFLE;
                    else if (strncasecmp(t, "GAMBOXS", 7) == 0) w = WEAPON_SHOTGUN;
                    else if (strncasecmp(t, "GOIL",    4) == 0 ||
                             strncasecmp(t, "GAMBELT", 7) == 0) w = WEAPON_GATLING;
                    else if (strncasecmp(t, "GDYNAM",  6) == 0) w = WEAPON_DYNAMITE;
                    else                                        w = WEAPON_PISTOL;
                    weapon_add_ammo(&g_app.player.weapons, w, pk.value);
                    break;
                }
                case ENTITY_ITEM_WEAPON: {
                    /* pk.value carries the WeaponType index (entity s_defs). */
                    WeaponType w = (pk.value > 0 && pk.value < WEAPON_COUNT)
                                       ? (WeaponType)pk.value : WEAPON_SHOTGUN;
                    bool had = g_app.player.weapons.has_weapon[w];
                    /* Ground weapon grants the gun + one clip of reserve ammo. */
                    i32 give = g_weapon_defs[w].clip_size;
                    if (give <= 0) give = 6;
                    weapon_pickup(&g_app.player.weapons, w, give);
                    /* Auto-switch only to a NEWLY acquired gun (a duplicate just
                     * tops up ammo, like the original). */
                    if (!had) weapon_switch(&g_app.player.weapons, w);
                    break;
                }
                case ENTITY_ITEM_KEY: {
                    /* Keys / crowbar: set the matching inventory bit and show a
                     * pickup message. Doors whose sector name encodes the key
                     * (RHSTEEL, BIRON, ...) unlock when nudged with it. */
                    const char *t = pk.type_name;
                    int kt = INF_KEY_NONE; const char *label = "KEY";
                    if      (strncasecmp(t, "GSTEEKEY", 8) == 0) { kt = INF_KEY_STEEL;   label = "STEEL KEY"; }
                    else if (strncasecmp(t, "GIRONKEY", 8) == 0) { kt = INF_KEY_IRON;    label = "IRON KEY"; }
                    else if (strncasecmp(t, "GBRSSKEY", 8) == 0) { kt = INF_KEY_BRASS;   label = "BRASS KEY"; }
                    else if (strncasecmp(t, "GSQRKEY",  7) == 0) { kt = INF_KEY_SQUARE;  label = "SQUARE KEY"; }
                    else if (strncasecmp(t, "GRKEY",    5) == 0) { kt = INF_KEY_ROUND;   label = "ROUND KEY"; }
                    else if (strncasecmp(t, "GCROWBAR", 8) == 0) { kt = INF_KEY_CROWBAR; label = "CROWBAR"; }
                    else if (strncasecmp(t, "GSHOVEL",  7) == 0) { kt = INF_KEY_SHOVEL;  label = "SHOVEL"; }
                    else if (strncasecmp(t, "GBADGE",   6) == 0) { kt = INF_KEY_BADGE;   label = "SHERIFF'S BADGE"; }
                    if (kt != INF_KEY_NONE) g_app.player.keys |= (1 << kt);
                    else g_app.player.keys |= 1; /* generic badge */
                    snprintf(g_app.message_text, sizeof(g_app.message_text),
                             "GOT THE %s", label);
                    g_app.message_timer = 2.5f;
                    break;
                }
                default: break;
                }
            }
        }

        /* ---- Weapon firing: mode routing per the weapon's button table
         * (Ghidra 0x513c28..; TNT inverted). Thrown weapons use cook/release
         * mechanics; others autofire while held at the fire-chor rate. ---- */
        (void)fired;
        if (!g_app.player.dead) {
            WeaponState *ws = &g_app.player.weapons;
            const WeaponDef *wdef = &g_weapon_defs[ws->current];
            /* Debug overlay open → clicks belong to ImGui, not the gun. Treat the
             * mouse as unpressed (but still update lmb/rmb_prev below) so closing
             * the menu with a button held doesn't fire a phantom shot. */
            bool ui_open  = g_debug.visible;
            bool lmb_held  = !ui_open && (g_app.input.mouse_buttons[0] ||
                             (g_app.screenshot_firing && g_app.screenshot_frames > 0));
            bool rmb_held  = !ui_open && g_app.input.mouse_buttons[2];
            bool lmb_press = lmb_held && !g_app.lmb_prev;
            bool rmb_press = rmb_held && !g_app.rmb_prev;
            g_app.lmb_prev = lmb_held;
            g_app.rmb_prev = rmb_held;

            /* Pressing fire interrupts a per-round reload after the current
             * round finishes loading (Weapon_ReloadStep semantics). */
            if ((lmb_press || rmb_press) && ws->reloading)
                ws->reload_interrupt = true;

            /* Weapon switch drops a held lit stick at the feet (power 0). */
            if (ws->holding_lit && ws->current != WEAPON_DYNAMITE) {
                player_throw(PROJ_DYNAMITE, 0.0f, true, ws->lit_fuse);
                ws->holding_lit = false;
            }

            if (ws->current == WEAPON_KNIFE || ws->current == WEAPON_DYNAMITE) {
                handle_thrown_weapon(lmb_press, lmb_held, rmb_press, dt);
            } else {
                int mode = -1;
                if      (lmb_held) mode = wdef->button_mode[0];
                else if (rmb_held) mode = wdef->button_mode[1];
                if (mode >= 0 && !ws->reloading && ws->fire_cooldown <= 0.0f) {
                    if (weapon_can_fire(ws)) {
                        do_player_fire(mode);
                    } else if (lmb_press || rmb_press) {
                        /* Dry fire click (NO_AMMO_SOUND) */
                        if (g_app.sfx_dry[ws->current])
                            audio_play(&g_app.audio, g_app.sfx_dry[ws->current]);
                        ws->fire_cooldown = 0.3f;
                    }
                }
            }

            /* Rifle scope toggle (V): no aim error, full-range traces, zoom. */
            if (!ui_open && input_key_pressed(&g_app.input, (SDL_Scancode)g_settings.bind[BIND_SCOPE]) && wdef->has_scope) {
                ws->scope_active = !ws->scope_active;
                renderer_set_zoom(&g_app.renderer, ws->scope_active ? 2.0f : 1.0f);
            }
            if (!wdef->has_scope && ws->scope_active) {
                ws->scope_active = false;
                renderer_set_zoom(&g_app.renderer, 1.0f);
            }
        }
        /* (Reload is driven by the reload_held flag set before player_update —
         * hold R to chamber rounds one at a time; see above.) */
        /* Automap overlay toggle (TAB) */
        if (!g_debug.visible && input_key_pressed(&g_app.input, (SDL_Scancode)g_settings.bind[BIND_MAP]))
            g_app.show_map = !g_app.show_map;
        /* Advance fire animation */
        if (g_app.player.fire_anim) {
            g_app.player.fire_anim_timer += dt;
            if (g_app.player.fire_anim_timer >= g_app.player.fire_anim_dur)
                g_app.player.fire_anim = false;
        }

        /* ---- INF (doors/elevators/scroll) ---- */
        /* ENTER-mask doors auto-open only while the player stands IN the door
         * sector; everything else waits for USE/E. --open-doors forces all. */
        inf_auto_doors(&g_app.world.inf, &g_app.world.lvt,
                       g_app.player.sector_idx,
                       g_app.force_open_doors ? 1e6f : 20.0f);
        /* INF triggers: fire ENTER/CROSS triggers when the player changes
         * sector (walk-over switches, scripted events). */
        {
            i32 si = g_app.player.sector_idx;
            if (si != g_app.prev_sector && si >= 0 &&
                si < (i32)g_app.world.lvt.sector_count) {
                inf_fire_triggers(&g_app.world.inf, (u32)si,
                                  INF_EVENT_ENTER | INF_EVENT_CROSS);
                if (g_app.prev_sector >= 0 &&
                    g_app.prev_sector < (i32)g_app.world.lvt.sector_count)
                    inf_fire_triggers(&g_app.world.inf, (u32)g_app.prev_sector,
                                      INF_EVENT_LEAVE);
                g_app.prev_sector = si;
            }
        }

        /* LINE triggers: fire when the player's movement segment crosses a
         * trigger line (OFFICE mission-hub porches → SPAWN_LEVEL, secrets). */
        inf_check_line_cross(&g_app.world.inf, pre_move_pos.x, pre_move_pos.z,
                             g_app.player.pos.x, g_app.player.pos.z);

        inf_update(&g_app.world.inf, dt, &g_app.world.lvt);
        /* Sync scroll UV offsets every frame (no rebuild needed for scroll) */
        renderer_sync_scroll(&g_app.renderer, &g_app.world.inf);
        if (g_app.world.inf.dirty)
            renderer_build_level(&g_app.renderer, &g_app.world.lvt, &g_app.world.inf);

        /* USE key (E): open the door in front (with lock/key check), fire NUDGE
         * triggers, in the current and adjacent sectors. */
        if (input_key_pressed(&g_app.input, (SDL_Scancode)g_settings.bind[BIND_USE]) || g_app.force_use) {
            int si = g_app.player.sector_idx;
            {
                int needed = INF_KEY_NONE;
                u32 keys = (u32)g_app.player.keys;

                /* Faithful USE: raycast from the eye at the wall being looked
                 * at and nudge the door across it (Actor_NudgeTrace@0x446cc0).
                 * This opens interior/upstairs doors the old proximity radius
                 * missed. Fall back to a short proximity nudge for doors you
                 * stand right against without facing. */
                Vec3 ueye = player_eye_pos(&g_app.player);
                f32 ucp = cosf(g_app.player.pitch), usp = sinf(g_app.player.pitch);
                f32 ucy = cosf(g_app.player.yaw),   usy = sinf(g_app.player.yaw);
                Vec3 udir = { ucy*ucp, usp, usy*ucp };
                InfDoorResult best = inf_nudge_door_ray(
                    &g_app.world.inf, &g_app.world.lvt,
                    ueye.x, ueye.z, ueye.y, udir.x, udir.z, udir.y,
                    64.0f, keys, &needed);
                if (best == INF_DOOR_NONE) {
                    int nk2 = INF_KEY_NONE;
                    InfDoorResult b2 = inf_nudge_door_near(
                        &g_app.world.inf, &g_app.world.lvt,
                        g_app.player.pos.x, g_app.player.pos.z, 12.0f, keys, &nk2);
                    if (b2 != INF_DOOR_NONE) { best = b2; needed = nk2; }
                }

                /* USE also nudges scenery (msg 0x7D3, Actor_NudgeTrace
                 * @0x446cc0): SHOOT and NUDGE Inv_Object types react. */
                {
                    Vec3 eye = player_eye_pos(&g_app.player);
                    f32 cp2 = cosf(g_app.player.pitch), sp2 = sinf(g_app.player.pitch);
                    f32 cy2 = cosf(g_app.player.yaw),   sy2 = sinf(g_app.player.yaw);
                    Vec3 fdir = { cy2*cp2, sp2, sy2*cp2 };
                    entity_nudge(&g_app.world.entities, eye, fdir, 12.0f);
                    /* USE a wall LINE trigger (nudge-masked switches). The
                     * OFFICE mission posters are SHOOT-masked (fired from the
                     * fire path), not USE. */
                    inf_fire_line_ray(&g_app.world.inf, &g_app.world.lvt,
                                      eye.x, eye.z, fdir.x, fdir.z, 24.0f,
                                      INF_EVENT_ENTER | INF_EVENT_NUDGE);
                }

                /* Also fire NUDGE triggers in the current + adjacent sectors. */
                if (si >= 0 && si < (i32)g_app.world.lvt.sector_count) {
                    inf_trigger(&g_app.world.inf, (u32)si);
                    inf_fire_triggers(&g_app.world.inf, (u32)si, INF_EVENT_NUDGE);
                    const LvtSector *sec = &g_app.world.lvt.sectors[si];
                    for (u32 wi = 0; wi < sec->wall_count; wi++) {
                        i32 adj = sec->walls[wi].adjoin;
                        if (adj >= 0) {
                            inf_trigger(&g_app.world.inf, (u32)adj);
                            inf_fire_triggers(&g_app.world.inf, (u32)adj, INF_EVENT_NUDGE);
                        }
                    }
                }

                if (best == INF_DOOR_LOCKED) {
                    /* Prefer the door's own LOCAL.MSG text ("You need the brass
                     * key.", "This door is stuck.", ...); fall back to a generic
                     * "YOU NEED THE X KEY" if the id has no table entry. */
                    const char *lt = (g_app.world.inf.pending_lock_msg >= 0)
                        ? msg_get(&g_app.world.messages, g_app.world.inf.pending_lock_msg)
                        : NULL;
                    if (lt)
                        snprintf(g_app.message_text, sizeof(g_app.message_text), "%s", lt);
                    else
                        snprintf(g_app.message_text, sizeof(g_app.message_text),
                                 "YOU NEED THE %s", inf_key_name(needed));
                    g_app.message_timer = 2.5f;
                    audio_play(&g_app.audio, g_app.sfx_locked);
                } else if (best == INF_DOOR_UNLOCKED) {
                    audio_play(&g_app.audio, g_app.sfx_unlock);
                }
                /* A door's own nudge USER_MSG (its lock hint) is superseded by
                 * our lock/unlock feedback — don't show the raw "MESSAGE N". */
                if (best != INF_DOOR_NONE)
                    g_app.world.inf.pending_user_msg = -1;
                g_app.world.inf.pending_lock_msg = -1;
                /* Play door sounds for newly-triggered elevators. */
                for (u32 ei = 0; ei < g_app.world.inf.count; ei++) {
                    const Elevator *el = &g_app.world.inf.elevs[ei];
                    if (el->just_triggered && el->sound_id)
                        audio_play(&g_app.audio, el->sound_id);
                    if (getenv("OL_DOORLOG") && el->just_triggered)
                        OL_LOG("DOORLOG nudged '%s' sec=%u result=%d\n",
                               el->sector_name, el->sector_idx, best);
                }
            }
        }

        /* Consume pending SYSTEM messages fired by triggers this frame — show
         * the real LOCAL.MSG text (objectives, hints). */
        if (g_app.world.inf.pending_user_msg >= 0) {
            const char *ut = msg_get(&g_app.world.messages,
                                     g_app.world.inf.pending_user_msg);
            if (ut)
                snprintf(g_app.message_text, sizeof(g_app.message_text), "%s", ut);
            else
                snprintf(g_app.message_text, sizeof(g_app.message_text),
                         "Message %d", g_app.world.inf.pending_user_msg);
            g_app.message_timer = 3.0f;
            g_app.world.inf.pending_user_msg = -1;
        }
        /* SPAWN_LEVEL: an OFFICE-hub porch (or similar) launched a mission. */
        if (g_app.world.inf.pending_spawn_level[0]) {
            char lvl[32];
            snprintf(lvl, sizeof(lvl), "%s", g_app.world.inf.pending_spawn_level);
            g_app.world.inf.pending_spawn_level[0] = '\0';
            g_app.world.inf.pending_spawn_start[0] = '\0';
            OL_LOG("SPAWN_LEVEL -> loading '%s'\n", lvl);
            g_app.campaign_active = false;   /* single mission from the hub */
            if (load_level_runtime(lvl)) {
                input_capture_mouse(&g_app.input, true);
                goto render_frame;   /* skip the rest of this stale frame */
            }
        }
        if (g_app.world.inf.pending_end_level) {
            g_app.world.inf.pending_end_level = false;
            if (g_app.campaign_active && !g_app.level_done) {
                /* Campaign: show a card, then auto-advance to the next mission. */
                g_app.level_done = true;
                g_app.level_done_timer = 3.5f;
                bool last = (g_app.campaign_idx + 1 >= g_app.campaign_count);
                snprintf(g_app.message_text, sizeof(g_app.message_text),
                         last ? "CAMPAIGN COMPLETE" : "MISSION COMPLETE");
                g_app.message_timer = 3.5f;
            } else {
                snprintf(g_app.message_text, sizeof(g_app.message_text), "LEVEL COMPLETE");
                g_app.message_timer = 4.0f;
            }
        }
        /* EXPLODE elevator detonated: play a blast and damage the player if near
         * the exploding sector's centroid. */
        if (g_app.world.inf.pending_explode_sector >= 0) {
            i32 es = g_app.world.inf.pending_explode_sector;
            g_app.world.inf.pending_explode_sector = -1;
            if (es < (i32)g_app.world.lvt.sector_count) {
                const LvtSector *s = &g_app.world.lvt.sectors[es];
                f32 cx = 0, cz = 0;
                for (u32 v = 0; v < s->vertex_count; v++) { cx += s->vertices[v].x; cz += s->vertices[v].y; }
                if (s->vertex_count) { cx /= s->vertex_count; cz /= s->vertex_count; }
                f32 dx = g_app.player.pos.x - cx, dz = g_app.player.pos.z - cz;
                f32 d2 = dx*dx + dz*dz;
                if (g_app.sfx_glass_break) audio_play(&g_app.audio, g_app.sfx_glass_break);
                if (d2 < 30.0f * 30.0f) {
                    i32 dmg = (i32)(60.0f * (1.0f - sqrtf(d2) / 30.0f));
                    if (dmg > 0) player_damage(&g_app.player, dmg);
                }
            }
        }
        if (g_app.message_timer > 0.0f) g_app.message_timer -= dt;

        /* ---- Entity AI ---- */
        /* Thrown projectiles (knife / dynamite): physics, fuses, pickup */
        projectile_update(&g_app.projectiles, &g_app.world.lvt, dt);
        {
            ProjKind pk = projectile_try_pickup(&g_app.projectiles,
                                                g_app.player.pos);
            if (pk == PROJ_KNIFE) {
                g_app.player.weapons.has_weapon[WEAPON_KNIFE] = true;
                weapon_add_ammo(&g_app.player.weapons, WEAPON_KNIFE, 1);
                audio_play(&g_app.audio, g_app.sfx_pickup);
            } else if (pk == PROJ_DYNAMITE) {
                g_app.player.weapons.has_weapon[WEAPON_DYNAMITE] = true;
                weapon_add_ammo(&g_app.player.weapons, WEAPON_DYNAMITE, 1);
                audio_play(&g_app.audio, g_app.sfx_pickup);
            }
        }

        entity_update_all(&g_app.world.entities, g_app.player.pos, dt,
                          &g_app.world.lvt, on_player_hurt);

        /* ---- Mission / boss objective (Sanchez in TOWN) ---- */
        {
            const char *mmsg = NULL; bool mdone = false;
            mission_update(&g_app.world.mission, &g_app.world.entities,
                           g_app.player.pos, &mmsg, &mdone);
            if (mmsg) {
                snprintf(g_app.message_text, sizeof(g_app.message_text), "%s", mmsg);
                g_app.message_timer = 4.0f;
                /* Boss appearing / dying — a fitting sting. */
                if (g_app.sfx_locked) audio_play(&g_app.audio, g_app.sfx_locked);
            }
            if (mdone) {
                g_app.world.inf.pending_end_level = true; /* triggers LEVEL COMPLETE */
            }
        }

        /* ---- Apply pending damage ---- */
        if (g_app.pending_player_damage > 0)
            player_damage(&g_app.player, g_app.pending_player_damage);

render_frame:
        /* ---- Camera ---- */
        {
            Vec3 eye = player_eye_pos(&g_app.player);
            /* LVT Z → GL -Z for the camera */
            Vec3 gl_eye = { eye.x, eye.y, -eye.z };

            /* Death camera effect: slowly fall */
            if (g_app.player.dead)
                gl_eye.y = eye.y - g_app.player.dead_timer * 2.0f;

            renderer_set_camera(&g_app.renderer, gl_eye,
                                g_app.player.yaw, g_app.player.pitch);
        }

        /* ---- Animated textures ---- */
        renderer_update_anim_textures(&g_app.renderer, dt);

        /* ---- Window shatter animation ----
         * While a window is playing its break frames, advance each broken
         * window's break_time and rebuild the level mesh so the frame updates
         * (the frame is baked into the batched mesh; window breaks are rare and
         * brief). Once past the animation length the windows hold their final
         * shattered frame and rebuilding stops. */
        if (g_app.window_anim_timer > 0.0f) {
            g_app.window_anim_timer -= dt;
            LvtLevel *lvt = &g_app.world.lvt;
            for (u32 si = 0; si < lvt->sector_count; si++) {
                LvtSector *sec = &lvt->sectors[si];
                for (u32 wi = 0; wi < sec->wall_count; wi++) {
                    LvtWall *w = &sec->walls[wi];
                    if (w->is_window && w->window_broken &&
                        w->break_time < WINDOW_BREAK_DURATION)
                        w->break_time += dt;
                }
            }
            renderer_build_level(&g_app.renderer, &g_app.world.lvt, &g_app.world.inf);
            if (g_app.window_anim_timer < 0.0f) g_app.window_anim_timer = 0.0f;
        }

        /* ---- Render ---- */
        renderer_begin_frame(&g_app.renderer);
        renderer_draw_sky(&g_app.renderer);
        renderer_draw_level(&g_app.renderer);
        renderer_draw_sprites(&g_app.renderer, &g_app.world.entities);

        /* Projectiles: 3DO models (knife/dynamite) + FX billboards */
        {
            BillboardDraw bbs[MAX_PROJECTILES];
            TdoDraw tds[MAX_PROJECTILES];
            u32 nbb = 0, ntd = 0;
            for (int i = 0; i < MAX_PROJECTILES; i++) {
                const Projectile *p = &g_app.projectiles.list[i];
                if (!p->active) continue;
                if (p->tdo >= 0) {
                    tds[ntd].pos    = (Vec3){ p->pos.x, p->pos.y + 0.4f, p->pos.z };
                    tds[ntd].yaw    = p->yaw;
                    tds[ntd].tumble = p->tumble;
                    tds[ntd].id     = p->tdo;
                    ntd++;
                } else if (p->tex) {
                    bbs[nbb].pos = p->pos;
                    bbs[nbb].tex = p->tex;
                    bbs[nbb].w   = p->w > 0 ? p->w : 1.0f;
                    bbs[nbb].h   = p->h > 0 ? p->h : 1.0f;
                    nbb++;
                }
            }
            if (ntd) renderer_draw_tdos(&g_app.renderer, tds, ntd);
            if (nbb) renderer_draw_billboards(&g_app.renderer, bbs, nbb);
            if (getenv("OL_PROJLOG") && (nbb || ntd)) {
                static int plog = 0;
                if ((plog++ % 20) == 0)
                    for (int i = 0; i < MAX_PROJECTILES; i++) {
                        const Projectile *p = &g_app.projectiles.list[i];
                        if (!p->active || p->kind == PROJ_FX) continue;
                        OL_LOG("PROJ k=%d pos=(%.1f,%.2f,%.1f) rest=%d sec=%d\n",
                               p->kind, p->pos.x, p->pos.y, p->pos.z,
                               p->resting, p->sector);
                    }
            }
        }

        /* HUD */
        {
            f32 fire_t = 0.0f;
            if (g_app.player.fire_anim && g_app.player.fire_anim_dur > 0.0f)
                fire_t = g_app.player.fire_anim_timer / g_app.player.fire_anim_dur;
            HudParams hud = {
                .health       = g_app.player.health,
                .max_health   = PLAYER_MAX_HEALTH,
                .ammo         = (g_weapon_defs[g_app.player.weapons.current].clip_size > 0)
                                ? g_app.player.weapons.clip[g_app.player.weapons.current]
                                : g_app.player.weapons.ammo[g_app.player.weapons.current],
                .reserve      = g_app.player.weapons.ammo[g_app.player.weapons.current],
                .clip_size    = g_weapon_defs[g_app.player.weapons.current].clip_size,
                .weapon_idx   = (int)g_app.player.weapons.current,
                .show_crosshair = !g_app.player.dead,
                .dead         = g_app.player.dead,
                .reloading    = g_app.player.weapons.reloading,
                .firing       = g_app.player.fire_anim,
                .fire_alt     = g_app.player.fire_alt,
                .cooking      = g_app.player.weapons.cooking,
                .cooking_alt  = (g_app.player.weapons.current == WEAPON_DYNAMITE),
                .holding_lit  = g_app.player.weapons.holding_lit &&
                                g_app.player.weapons.current == WEAPON_DYNAMITE,
                .fire_timer   = OL_CLAMP(fire_t, 0.0f, 0.99f),
                .message      = (g_app.message_timer > 0.0f) ? g_app.message_text : NULL,
                .inventory    = build_hud_left_text(
                                    g_app.world.mission.has_boss
                                        ? g_app.world.mission.objective : NULL,
                                    g_app.player.keys),
            };
            renderer_draw_hud(&g_app.renderer, &hud);
            if (g_app.show_map)
                renderer_draw_minimap(&g_app.renderer, &g_app.world.lvt,
                                      &g_app.world.inf, g_app.player.pos,
                                      g_app.player.yaw);
        }

        /* Debug UI overlay (INSERT to toggle) */
        {
            g_debug.player_x = g_app.player.pos.x;
            g_debug.player_y = g_app.player.pos.y;
            g_debug.player_z = g_app.player.pos.z;
            g_debug.player_yaw = g_app.player.yaw;
            g_debug.player_pitch = g_app.player.pitch;
            g_debug.player_sector = g_app.player.sector_idx;
            g_debug.player_health = g_app.player.health;
            g_debug.player_keys = g_app.player.keys;
            g_debug.sector_count = (int)g_app.world.lvt.sector_count;
            g_debug.entity_count = (int)g_app.world.entities.count;
            g_debug.draw_calls = (int)g_app.renderer.level_mesh_count;
            g_debug.dt = dt;
            g_debug.fps = (dt > 0.0f) ? 1.0f / dt : 0.0f;
            debug_ui_push_fps(g_debug.fps);
            /* Movement / sector detail */
            g_debug.player_vx = g_app.player.vel.x;
            g_debug.player_vy = g_app.player.vel_y;
            g_debug.player_vz = g_app.player.vel.z;
            g_debug.on_ground = g_app.player.on_ground ? 1 : 0;
            g_debug.crouching = g_app.player.crouching ? 1 : 0;
            g_debug.eye_height = g_app.player.eye_height;
            g_debug.difficulty = g_app.difficulty;
            g_debug.weapon_idx = (int)g_app.player.weapons.current;
            g_debug.weapon_clip = g_app.player.weapons.clip[g_app.player.weapons.current];
            g_debug.weapon_reserve = g_app.player.weapons.ammo[g_app.player.weapons.current];
            {
                f32 fy = 0, cy2 = 256;
                collision_heights(&g_app.world.lvt, g_app.player.sector_idx,
                                  g_app.player.pos.x, g_app.player.pos.z, &fy, &cy2);
                g_debug.sector_floor = fy; g_debug.sector_ceil = cy2;
            }
            /* Water / swim state of the player's current sector. */
            g_debug.in_water = 0;
            g_debug.sector_floor_tex[0] = '\0';
            if (g_app.player.sector_idx >= 0 &&
                g_app.player.sector_idx < (i32)g_app.world.lvt.sector_count) {
                const LvtSector *ps = &g_app.world.lvt.sectors[g_app.player.sector_idx];
                g_debug.in_water = ps->is_water ? 1 : 0;
                if (ps->floor_tex >= 0 && (u32)ps->floor_tex < g_app.world.lvt.texture_count)
                    snprintf(g_debug.sector_floor_tex, sizeof(g_debug.sector_floor_tex),
                             "%s", g_app.world.lvt.textures[ps->floor_tex]);
            }
            debug_compute_lookat();

            /* Level / mission status */
            snprintf(g_debug.map_name, sizeof(g_debug.map_name), "%s", g_app.level_name);
            g_debug.campaign_active   = g_app.campaign_active ? 1 : 0;
            g_debug.campaign_mission  = g_app.campaign_idx + 1;
            g_debug.campaign_total    = g_app.campaign_count;
            {
                const MissionState *ms = &g_app.world.mission;
                snprintf(g_debug.objective, sizeof(g_debug.objective), "%s", ms->objective);
                g_debug.has_boss         = ms->has_boss ? 1 : 0;
                g_debug.boss_spawned     = ms->boss_spawned ? 1 : 0;
                g_debug.boss_dead        = ms->boss_dead ? 1 : 0;
                g_debug.mission_complete = ms->complete ? 1 : 0;
            }
            {
                int en_alive = 0, it_alive = 0;
                const EntityList *el = &g_app.world.entities;
                for (u32 ei = 0; ei < el->count; ei++) {
                    const Entity *e = &el->entities[ei];
                    if (!e->active) continue;
                    if (e->kind == ENTITY_ENEMY) { if (e->health > 0) en_alive++; }
                    else if (e->kind == ENTITY_ITEM_HEALTH || e->kind == ENTITY_ITEM_AMMO ||
                             e->kind == ENTITY_ITEM_KEY   || e->kind == ENTITY_ITEM_WEAPON)
                        it_alive++;
                }
                g_debug.enemies_total = (int)g_app.world.mission.total_enemies;
                g_debug.enemies_alive = en_alive;
                g_debug.items_alive   = it_alive;
            }

            /* Apply cheats */
            if (g_debug.godmode && g_app.player.health < 100)
                g_app.player.health = 100;
            if (g_debug.give_all_weapons) {
                g_debug.give_all_weapons = false;
                for (int wi = 0; wi < WEAPON_COUNT; wi++) {
                    g_app.player.weapons.has_weapon[wi] = true;
                    g_app.player.weapons.ammo[wi] = 999;
                }
            }
            /* Debug: grant inventory items (keys / crowbar / shovel / badge). */
            if (g_debug.give_items) {
                g_app.player.keys |= g_debug.give_items;
                g_debug.give_items = 0;
            }
            /* Debug: set difficulty (reloads the level) / reload level. */
            if (g_debug.req_set_difficulty >= 1 && g_debug.req_set_difficulty <= 4) {
                g_app.difficulty = g_debug.req_set_difficulty;
                g_debug.req_set_difficulty = 0;
                g_debug.req_reload_level = 1;
            }
            if (g_debug.req_reload_level) {
                g_debug.req_reload_level = 0;
                load_level_runtime(g_app.level_name);
                goto render_frame;
            }

            /* Resolve the post-FX pass now so the debug overlay draws crisp on
             * top of the processed scene (not through the CRT/bloom shader). */
            renderer_post_resolve(&g_app.renderer);

            /* Wireframe toggle */
            if (g_debug.wireframe)
                glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

            debug_ui_render();

            if (g_debug.wireframe)
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        }

        /* ---- Pause overlay (drawn over the frozen scene) ---- */
        if (g_app.paused) {
            menu_frame(&g_app.menu, &g_app.input, &g_app.renderer, &g_app.audio);
            Menu *mm = &g_app.menu;
            if (mm->want_resume) {
                mm->want_resume = false;
                g_app.paused = false;
                g_app.menu.screen = MENU_INGAME;
                input_capture_mouse(&g_app.input, true);
            } else if (mm->req_save_slot >= 0) {
                do_save(mm->req_save_slot);
                mm->req_save_slot = -1;
                refresh_save_slots();
                mm->screen = MENU_PAUSE;   /* back to the pause root */
            } else if (mm->req_load_slot >= 0) {
                int slot = mm->req_load_slot;
                mm->req_load_slot = -1;
                if (do_load(slot)) {
                    g_app.paused = false;
                    g_app.menu.screen = MENU_INGAME;
                    input_capture_mouse(&g_app.input, true);
                }
            } else if (mm->want_quit_menu) {
                mm->want_quit_menu = false;
                g_app.paused = false;
                g_app.campaign_active = false;
                g_app.in_menu = true;
                g_app.menu.screen = MENU_MAIN;
                g_app.menu.sel = 0;
            }
        }

        renderer_end_frame(&g_app.renderer);

        /* ---- Headless screenshot capture ---- */
        screenshot_tick(&frame_no);

        /* ---- Frame cap ---- */
        u64 elapsed = SDL_GetTicks64() - now;
        if (elapsed < FRAME_TIME_MS)
            SDL_Delay((u32)(FRAME_TIME_MS - elapsed));
    }
}

/* -------------------------------------------------------------------------
 * Cleanup
 * ---------------------------------------------------------------------- */
static void app_shutdown(void) {
    world_free(&g_app.world);
    audio_shutdown(&g_app.audio);
    debug_ui_shutdown();
    renderer_shutdown(&g_app.renderer);
    archives_close(&g_app.archives);
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */
/* Print a machine-parseable health line for the loaded level, return exit code
 * (0 = PASS, 1 = FAIL). Used by --check for the automated level sweep. */
/* -------------------------------------------------------------------------
 * Automated door test (--check-doors)
 *
 * For every door in the level: try to open it WITHOUT the key (locked doors
 * must refuse, unlocked must open), then WITH the key (must unlock), and verify
 * the doorway collision flips from SOLID (closed) to PASSABLE (open). Catches
 * broken locks, doors that won't open, and doors that stay solid when open.
 * ---------------------------------------------------------------------- */
static const char *door_type_str(int t) {
    switch (t) {
        case ELEV_TYPE_MORPH_MOVE: return "SLIDE";
        case ELEV_TYPE_MORPH_SPIN: return "SWING";
        case ELEV_TYPE_DOOR:       return "DOOR";
        case ELEV_TYPE_INV_DOOR:   return "INVDOOR";
        default:                   return "?";
    }
}
static const char *door_res_str(InfDoorResult r) {
    switch (r) {
        case INF_DOOR_OPENED:   return "OPENED";
        case INF_DOOR_UNLOCKED: return "UNLOCKED";
        case INF_DOOR_LOCKED:   return "LOCKED";
        default:                return "-";
    }
}

/* First adjoin wall of a sector — the doorway passage to a neighbour. */
static int door_adjoin_wall(const LvtSector *s) {
    for (u32 wi = 0; wi < s->wall_count; wi++)
        if (s->walls[wi].adjoin >= 0) return (int)wi;
    return -1;
}

static int check_doors(void) {
    World *w = &g_app.world;
    InfSystem *inf = &w->inf;
    LvtLevel *lvt = &w->lvt;

    printf("DOOR CHECK level=%s\n", g_app.level_name);
    printf("  %-12s %-4s %-7s %-8s %-6s %-8s %-8s %-5s %-6s %s\n",
           "SECTOR", "IDX", "TYPE", "LOCK", "REACH", "NO-KEY", "WITH-KEY",
           "OPEN", "CLOSED", "RESULT");

    int total = 0, pass = 0;
    for (u32 i = 0; i < inf->count; i++) {
        Elevator *el = &inf->elevs[i];
        if (el->sector_idx >= lvt->sector_count) continue;
        bool is_door = (el->type == ELEV_TYPE_MORPH_MOVE ||
                        el->type == ELEV_TYPE_MORPH_SPIN ||
                        el->type == ELEV_TYPE_DOOR ||
                        el->type == ELEV_TYPE_INV_DOOR ||
                        el->type == ELEV_TYPE_FLAG_DOOR);
        if (!is_door) continue;
        total++;

        u32 D = el->sector_idx;
        LvtSector *sec = &lvt->sectors[D];
        bool morph = (el->type == ELEV_TYPE_MORPH_MOVE || el->type == ELEV_TYPE_MORPH_SPIN);
        bool perm  = el->perm_locked;
        bool locked = perm || (el->required_key != INF_KEY_NONE);

        /* Movement span across the door's stops. A morph door whose stops don't
         * differ (span ~0) has no open position — it's a static/decorative morph
         * sector (e.g. TRAIN "LOADED" cargo), not an openable door: report it as
         * STATIC and don't fail it. */
        f32 span = 0.0f;
        for (u32 s = 1; s < el->stop_count; s++) {
            f32 d = fabsf(el->stops[s].y - el->stops[0].y);
            if (d > span) span = d;
        }
        bool is_static = morph && (span < 0.5f);

        f32 testy = sec->floor_y + 2.0f;
        f32 cx = 0, cz = 0;
        for (u32 v = 0; v < sec->vertex_count; v++) { cx += sec->vertices[v].x; cz += sec->vertices[v].y; }
        if (sec->vertex_count) { cx /= sec->vertex_count; cz /= sec->vertex_count; }
        /* Pick the doorway wall: prefer an adjoin wall that is SOLID while the
         * door is closed (the real blocker), else the first adjoin. */
        int adjw = -1;
        for (u32 wi = 0; wi < sec->wall_count; wi++) {
            if (sec->walls[wi].adjoin < 0) continue;
            if (collision_wall_is_solid(lvt, (int)D, (int)wi, testy, PLAYER_BODY_HEIGHT)) {
                adjw = (int)wi; break;
            }
        }
        if (adjw < 0) adjw = door_adjoin_wall(sec);
        bool solid_closed = (adjw >= 0) &&
            collision_wall_is_solid(lvt, (int)D, adjw, testy, PLAYER_BODY_HEIGHT);

        /* Realistic standing spot: the player can't stand IN the closed door, so
         * approach head-on from the doorway on the NEIGHBOUR side. Stand a few
         * units off the doorway wall along its NORMAL (not toward the room
         * centroid — that skews for big/concave rooms and, with several door
         * leaves around one room, aims at a sibling). Facing straight down the
         * normal isolates the door the player is actually looking at. */
        f32 ax = cx, az = cz, aimx = 0, aimz = 1;
        if (adjw >= 0) {
            const LvtWall *dw = &sec->walls[adjw];
            if (dw->v1 >= 0 && dw->v2 >= 0 &&
                dw->v1 < (i32)sec->vertex_count && dw->v2 < (i32)sec->vertex_count) {
                f32 mx = (sec->vertices[dw->v1].x + sec->vertices[dw->v2].x) * 0.5f;
                f32 mz = (sec->vertices[dw->v1].y + sec->vertices[dw->v2].y) * 0.5f;
                f32 ex = sec->vertices[dw->v2].x - sec->vertices[dw->v1].x;
                f32 ez = sec->vertices[dw->v2].y - sec->vertices[dw->v1].y;
                f32 nl = sqrtf(ex*ex + ez*ez);
                f32 nx = nl > 1e-3f ? -ez/nl : 0, nz = nl > 1e-3f ? ex/nl : 1;
                /* Pick the normal side that lands in the neighbour room (the side
                 * the player stands on), then aim back through the door. */
                i32 pn = collision_find_sector(lvt, mx + nx*3.0f, mz + nz*3.0f, 0);
                f32 sgn = (pn == (i32)D || pn < 0) ? -1.0f : 1.0f;
                ax = mx + nx*3.0f*sgn; az = mz + nz*3.0f*sgn;
                aimx = -nx*sgn; aimz = -nz*sgn;
            }
        }
        int nk = 0;
        /* 1) nudge without any key (ray first, then proximity). The ray reach is
         * short (≈ the approach distance) so clustered door leaves sharing a room
         * are isolated — the player opens the one they face, not a neighbour. */
        InfDoorResult r0 = inf_nudge_door_ray(inf, lvt, ax, az, testy,
                                              aimx, aimz, 0.0f, 10.0f, 0, &nk);
        if (r0 == INF_DOOR_NONE)
            r0 = inf_nudge_door_near(inf, lvt, ax, az, 22.0f, 0, &nk);
        bool use_reaches = (r0 != INF_DOOR_NONE);
        /* 2) if it refused (locked), nudge again holding the required key */
        InfDoorResult r1 = r0;
        if (r0 == INF_DOOR_LOCKED && !perm) {
            u32 keym = (1u << el->required_key);
            r1 = inf_nudge_door_ray(inf, lvt, ax, az, testy, aimx, aimz, 0.0f, 24.0f, keym, &nk);
            if (r1 == INF_DOOR_NONE || r1 == INF_DOOR_LOCKED)
                r1 = inf_nudge_door_near(inf, lvt, ax, az, 22.0f, keym, &nk);
        }
        /* 3) advance the morph/elevator to fully open (10 s @ 60 Hz) */
        for (int f = 0; f < 600; f++) inf_update(inf, 1.0f/60.0f, lvt);

        bool opened = morph ? sec->door_open
                            : fabsf(el->current_y - el->stops[0].y) > 0.5f;
        bool solid_open = (adjw >= 0) &&
            collision_wall_is_solid(lvt, (int)D, adjw, testy, PLAYER_BODY_HEIGHT);

        /* Expectations */
        bool ok;
        const char *result;
        if (is_static) {
            /* No open position — lock logic must still be sane, but no movement. */
            ok = perm ? (r0 == INF_DOOR_LOCKED)
                      : locked ? (r0 == INF_DOOR_LOCKED) : true;
            result = ok ? "STATIC" : "FAIL";
        } else {
            /* The player must be able to REACH the door with USE (E). */
            if (!use_reaches) {
                ok = false;
            } else if (perm) {
                ok = (r0 == INF_DOOR_LOCKED) && !opened;          /* must stay shut */
            } else if (locked) {
                ok = (r0 == INF_DOOR_LOCKED) &&
                     (r1 == INF_DOOR_UNLOCKED || r1 == INF_DOOR_OPENED) && opened;
            } else {
                ok = (r0 == INF_DOOR_OPENED || r0 == INF_DOOR_UNLOCKED) && opened;
            }
            /* Collision must flip solid→passable for a morph door that opened. */
            if (morph && opened && adjw >= 0 && solid_closed && solid_open) ok = false;
            result = !use_reaches ? "NOREACH" : ok ? "PASS" : "FAIL";
        }

        if (ok) pass++;
        if (getenv("OL_DIRECT")) {
            /* Ground-truth: nudge the door's OWN sector directly (bypasses the
             * reach/approach heuristic) to confirm the elevator itself opens. */
            f32 y0 = el->current_y;
            int nk2 = 0; inf_nudge_door(inf, D, ~0u, &nk2);
            for (int f = 0; f < 120; f++) inf_update(inf, 1.0f/60.0f, lvt);
            printf("    DIRECT nudge sec %u: y %.1f -> %.1f (%s)\n", D, y0, el->current_y,
                   fabsf(el->current_y - el->stops[0].y) > 0.5f ? "OPENED" : "stuck");
        }
        if (getenv("OL_DOOR_AT"))
            printf("    approach (%.0f,%.0f) floor=%.0f\n", ax, az, sec->floor_y);
        printf("  %-12s %-4u %-7s %-8s %-6s %-8s %-8s %-5s %-6s %s\n",
               sec->name[0] ? sec->name : "(unnamed)", D, door_type_str(el->type),
               perm ? "PERM" : locked ? inf_key_name(el->required_key) : "none",
               use_reaches ? "yes" : "NO",
               door_res_str(r0), (r0 == INF_DOOR_LOCKED) ? door_res_str(r1) : "-",
               opened ? "yes" : "no",
               (adjw >= 0) ? (solid_closed ? "solid" : "open") : "n/a",
               result);
    }
    printf("DOOR CHECK: %d doors, PASS=%d FAIL=%d RESULT=%s\n",
           total, pass, total - pass, (total - pass == 0) ? "PASS" : "FAIL");
    fflush(stdout);
    return (total - pass == 0) ? 0 : 1;
}

static int print_level_check(void) {
    World *w = &g_app.world;
    u32 walls = 0;
    for (u32 s = 0; s < w->lvt.sector_count; s++) walls += w->lvt.sectors[s].wall_count;
    u32 enemies = 0, items = 0, boss_cand = 0;
    for (u32 i = 0; i < w->entities.count; i++) {
        const Entity *e = &w->entities.entities[i];
        if (e->is_boss) boss_cand++;
        else if (e->kind == ENTITY_ENEMY) enemies++;
        else if (e->kind >= ENTITY_ITEM_HEALTH && e->kind <= ENTITY_ITEM_WEAPON) items++;
    }
    bool start_ok = !(w->player_start.x == 0.0f && w->player_start.z == 0.0f);
    /* Hard failure = the level won't play: no geometry, unresolved scripting, no
     * player start, or a flood of missing textures. A handful of missing
     * textures (a few walls fall back to a placeholder) is reported but PASSes. */
    bool pass = w->lvt.sector_count > 0 && w->inf_unresolved == 0 &&
                start_ok && w->missing_textures <= 4;
    printf("CHECK level=%s sectors=%u walls=%u ents=%u enemies=%u items=%u "
           "boss_cand=%u inf_elevs=%u inf_triggers=%u inf_unresolved=%u "
           "miss_tex=%u start_ok=%d RESULT=%s\n",
           g_app.level_name, w->lvt.sector_count, walls, w->entities.count,
           enemies, items, boss_cand, w->inf.count, w->inf.trigger_count,
           w->inf_unresolved, w->missing_textures, start_ok ? 1 : 0,
           pass ? "PASS" : "FAIL");
    fflush(stdout);
    return pass ? 0 : 1;
}

int main(int argc, char **argv) {
    if (!app_init(argc, argv)) {
        /* In check mode a load failure is a reported FAIL, not a crash. */
        for (int i = 1; i < argc; i++) if (strcmp(argv[i], "--check") == 0) {
            printf("CHECK level=%s RESULT=FAIL (load failed)\n", g_app.level_name);
            fflush(stdout);
        }
        app_shutdown();
        return 1;
    }
    if (g_app.check_doors_mode) {
        int code = check_doors();
        app_shutdown();
        return code;
    }
    if (g_app.cutscene_cli[0]) {
        /* --cutscene <name>: play one cinematic and exit (manual test). With
         * --screenshot <path>, instead render one representative frame headless
         * and save it (automated verification of the in-engine video path). */
        if (g_app.screenshot_path[0]) cutscene_shot(g_app.cutscene_cli, g_app.screenshot_path);
        else cutscene_play(g_app.cutscene_cli);
        app_shutdown();
        return 0;
    }
    if (g_app.check_mode) {
        /* OL_LINETEST: verify LINE-trigger SPAWN_LEVEL — cross each resolved
         * line trigger's segment perpendicularly and report what fires. */
        if (getenv("OL_LINETEST")) {
            InfSystem *inf = &g_app.world.inf;
            for (u32 i = 0; i < inf->trigger_count; i++) {
                InfTrigger *tr = &inf->triggers[i];
                if (!tr->is_line) continue;
                OL_LOG("LINETEST trig '%s' line=%#x resolved=%d msg=%d spawn='%s' mask=%#x\n",
                       tr->sector_name, tr->line_id, tr->line_resolved,
                       tr->msg, tr->spawn_level, tr->event_mask);
                if (!tr->line_resolved) continue;
                /* midpoint + normal, cross from one side to the other */
                f32 mx = (tr->lx0 + tr->lx1) * 0.5f, mz = (tr->lz0 + tr->lz1) * 0.5f;
                f32 dx = tr->lx1 - tr->lx0, dz = tr->lz1 - tr->lz0;
                f32 L = sqrtf(dx*dx + dz*dz); if (L < 1e-4f) continue;
                f32 nx = -dz / L, nz = dx / L;
                inf->pending_spawn_level[0] = '\0';
                inf_check_line_cross(inf, mx - nx*2, mz - nz*2, mx + nx*2, mz + nz*2);
                OL_LOG("  -> crossed: pending_spawn='%s' user_msg=%d\n",
                       inf->pending_spawn_level, inf->pending_user_msg);
            }
        }
        /* OL_WEAPTEST: exercise the projectile/dynamite pipeline headlessly. */
        if (getenv("OL_WEAPTEST")) {
            Vec3 pp = g_app.world.player_start;
            g_app.player.pos = pp;
            /* Throw an UNLIT stick, let it fly and come to rest */
            g_app.player.weapons.has_weapon[WEAPON_DYNAMITE] = true;
            weapon_add_ammo(&g_app.player.weapons, WEAPON_DYNAMITE, 5);
            player_throw(PROJ_DYNAMITE, 1.0f, false, 0.0f);
            for (int f = 0; f < 300; f++)
                projectile_update(&g_app.projectiles, &g_app.world.lvt, 1.0f/60.0f);
            for (int i = 0; i < MAX_PROJECTILES; i++) {
                Projectile *pr = &g_app.projectiles.list[i];
                if (pr->active && pr->kind == PROJ_DYNAMITE)
                    OL_LOG("WEAPTEST tnt at (%.1f,%.2f,%.1f) resting=%d lit=%d\n",
                           pr->pos.x, pr->pos.y, pr->pos.z, pr->resting, pr->lit);
            }
            /* Shoot it: aim from player start at the stick */
            {
                Vec3 eye = player_eye_pos(&g_app.player);
                for (int i = 0; i < MAX_PROJECTILES; i++) {
                    Projectile *pr = &g_app.projectiles.list[i];
                    if (!pr->active || pr->kind != PROJ_DYNAMITE) continue;
                    Vec3 d = vec3_norm(vec3_sub((Vec3){pr->pos.x, pr->pos.y+0.5f, pr->pos.z}, eye));
                    f32 hd; int pi = projectile_raycast(&g_app.projectiles, eye, d, 200.0f, &hd);
                    OL_LOG("WEAPTEST shoot ray -> proj idx %d (dist %.1f)\n", pi, hd);
                    if (pi >= 0) projectile_damage(&g_app.projectiles, pi);
                    break;
                }
                /* Run fuse/explosion frames */
                for (int f = 0; f < 60; f++)
                    projectile_update(&g_app.projectiles, &g_app.world.lvt, 1.0f/60.0f);
                int still = 0;
                for (int i = 0; i < MAX_PROJECTILES; i++)
                    if (g_app.projectiles.list[i].active &&
                        g_app.projectiles.list[i].kind == PROJ_DYNAMITE) still++;
                OL_LOG("WEAPTEST after shot: dynamite left=%d (0 = exploded)\n", still);
            }
            /* Knife throw → rest → pickup */
            player_throw(PROJ_KNIFE, 0.8f, false, 0.0f);
            for (int f = 0; f < 300; f++)
                projectile_update(&g_app.projectiles, &g_app.world.lvt, 1.0f/60.0f);
            for (int i = 0; i < MAX_PROJECTILES; i++) {
                Projectile *pr = &g_app.projectiles.list[i];
                if (pr->active && pr->kind == PROJ_KNIFE) {
                    OL_LOG("WEAPTEST knife at (%.1f,%.2f,%.1f) resting=%d\n",
                           pr->pos.x, pr->pos.y, pr->pos.z, pr->resting);
                    g_app.player.pos = pr->pos;   /* walk to it */
                }
            }
            ProjKind got = projectile_try_pickup(&g_app.projectiles, g_app.player.pos);
            OL_LOG("WEAPTEST knife pickup: %s\n", got == PROJ_KNIFE ? "OK" : "FAIL");
        }
        /* OL_SCNTEST: exercise the scenery chor state machine headlessly —
         * find a bottle, log its chors, shoot it, run updates, log outcome. */
        if (getenv("OL_SCNTEST")) {
            EntityList *el = &g_app.world.entities;
            int found = 0;
            for (u32 i = 0; i < el->count; i++) {
                Entity *e = &el->entities[i];
                if (!e->active || !e->is_scenery) continue;
                if (found < 3)
                    OL_LOG("SCNTEST %s: type=%d chors=%u state=%u playing=%d "
                           "c0(n=%u end=%d) c1(n=%u end=%d snd=%d)\n",
                           e->type_name, e->scenery_type, e->scn_count,
                           e->scn_state, e->scn_playing,
                           e->scn[0].nframes, e->scn[0].end,
                           e->scn_count > 1 ? e->scn[1].nframes : 0,
                           e->scn_count > 1 ? e->scn[1].end : -1,
                           e->scn_count > 1 ? e->scn[1].sound_idx : -1);
                static bool shot_one = false;
                if (!shot_one && strncasecmp(e->type_name, "BOTTLE", 6) == 0) {
                    shot_one = true;
                    OL_LOG("SCNTEST bottle: chors=%u c1(n=%u end=%d snd=%d)\n",
                           e->scn_count,
                           e->scn_count > 1 ? e->scn[1].nframes : 0,
                           e->scn_count > 1 ? e->scn[1].end : -1,
                           e->scn_count > 1 ? e->scn[1].sound_idx : -1);
                    OL_LOG("SCNTEST shooting %s (idx %u)...\n", e->type_name, i);
                    entity_damage(el, (int)i, 2);
                    for (int f = 0; f < 180; f++)
                        entity_update_all(el, g_app.player.pos, 1.0f/60.0f,
                                          &g_app.world.lvt, NULL);
                    OL_LOG("SCNTEST after 3s: active=%d state=%u frame=%u playing=%d\n",
                           e->active, e->scn_state, e->scn_frame, e->scn_playing);
                }
                found++;
            }
            OL_LOG("SCNTEST scenery entities total: %d\n", found);
        }
        int code = print_level_check();
        app_shutdown();
        return code;
    }
    /* OL_LOADSHOT: draw one loading-screen frame (60%) and screenshot it. */
    if (getenv("OL_LOADSHOT") && g_app.screenshot_path[0]) {
        SDL_SetWindowSize(g_app.renderer.window, 800, 600);
        renderer_resize(&g_app.renderer, 800, 600);
        renderer_begin_frame(&g_app.renderer);
        renderer_draw_loading(&g_app.renderer, g_app.loading_bg_tex, 0.6f, "TOWN");
        int w = g_app.renderer.cfg.width, h = g_app.renderer.cfg.height;
        u8 *px = (u8*)malloc((size_t)w*h*4);
        glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,px);
        u8 *fl = (u8*)malloc((size_t)w*h*4);
        for (int y=0;y<h;y++) memcpy(fl+(size_t)(h-1-y)*w*4, px+(size_t)y*w*4,(size_t)w*4);
        SDL_Surface *sf=SDL_CreateRGBSurfaceFrom(fl,w,h,32,w*4,0xFF,0xFF00,0xFF0000,0xFF000000);
        if (sf){ SDL_SaveBMP(sf,g_app.screenshot_path); SDL_FreeSurface(sf); }
        free(fl); free(px);
        app_shutdown();
        return 0;
    }
    app_run();
    app_shutdown();
    return 0;
}
