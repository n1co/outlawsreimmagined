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
#include "collision.h"
#include "entity.h"
#include "weapon.h"
#include "audio.h"
#include "inf.h"
#include "wax.h"
#include "debug_ui.h"
#include "menu.h"
#include "savegame.h"

#include <SDL2/SDL.h>
#include <math.h>

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

    /* Story campaign progression + in-game pause. */
    char  campaign[16][64];      /* level order parsed from OUTLAWS.RCS */
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

    /* Previous weapon for switch-sound detection */
    WeaponType prev_weapon;

    /* Sound effect IDs (0 = not loaded) */
    u32   sfx_weapon[WEAPON_COUNT];  /* Weapon fire sounds */
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

static void load_sfx(void) {
    /* Weapon fire sounds (exact names from olsfx.lab) */
    g_app.sfx_weapon[WEAPON_FIST]        = sfx_load("jab.wav");
    g_app.sfx_weapon[WEAPON_PISTOL]      = sfx_load("PISTOL.WAV");
    g_app.sfx_weapon[WEAPON_RIFLE]       = sfx_load("RIFLE.WAV");
    g_app.sfx_weapon[WEAPON_SHOTGUN]     = sfx_load("SINGLE.WAV");
    g_app.sfx_weapon[WEAPON_DBL_SHOTGUN] = sfx_load("DOUBLE.WAV");
    g_app.sfx_weapon[WEAPON_SAW_GUN]     = sfx_load("sawblade.WAV");
    g_app.sfx_weapon[WEAPON_DYNAMITE]    = sfx_load("DYNAMITE.WAV");
    g_app.sfx_weapon[WEAPON_KNIFE]       = sfx_load("THROW.WAV");
    g_app.sfx_weapon[WEAPON_GATLING]     = sfx_load("GatlShot.WAV");

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

static bool load_level_runtime(const char *name) {
    if (name != g_app.level_name)   /* avoid snprintf self-copy (UB) */
        snprintf(g_app.level_name, sizeof(g_app.level_name), "%s", name);
    audio_stop_all(&g_app.audio);
    if (g_app.world.loaded) world_free(&g_app.world);
    if (!world_load(&g_app.world, &g_app.archives, &g_app.renderer, g_app.level_name)) {
        OL_ERR("Failed to load level '%s'\n", g_app.level_name);
        return false;
    }
    setup_entity_sounds();
    setup_inf_sounds();
    memset(g_app.renderer.sector_scroll_u, 0, sizeof(g_app.renderer.sector_scroll_u));
    memset(g_app.renderer.sector_scroll_v, 0, sizeof(g_app.renderer.sector_scroll_v));

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
                    OL_LOG("   wall %u -> adjoin sec %d '%s' floor=%.1f ceil=%.1f\n",
                           wi, w->adjoin, a->name, a->floor_y, a->ceil_y); } }
        }
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

/*
 * Build the campaign level order by parsing the original story script
 * OUTLAWS.RCS (BPCR resource): every "LEVEL: <name> <disc>" line, in order.
 * This is exactly how the original sequences the story. Falls back to the
 * verified retail order if the script is missing.
 */
static void campaign_parse(void) {
    g_app.campaign_count = 0;
    u32 sz = 0;
    const u8 *d = archives_get(&g_app.archives, "OUTLAWS.RCS", &sz);
    if (d && sz > 0) {
        const char *p = (const char *)d, *end = p + sz;
        while (p < end && g_app.campaign_count < 16) {
            /* find "LEVEL:" */
            if ((size_t)(end - p) >= 6 && strncasecmp(p, "LEVEL:", 6) == 0) {
                p += 6;
                while (p < end && (*p == ' ' || *p == '\t')) p++;
                char *dst = g_app.campaign[g_app.campaign_count];
                int n = 0;
                while (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && n < 63)
                    dst[n++] = *p++;
                dst[n] = '\0';
                if (n > 0) g_app.campaign_count++;
            } else {
                p++;
            }
        }
    }
    if (g_app.campaign_count == 0) {
        int n = (int)(sizeof(CAMPAIGN_FALLBACK)/sizeof(CAMPAIGN_FALLBACK[0]));
        for (int i = 0; i < n; i++)
            snprintf(g_app.campaign[i], sizeof(g_app.campaign[i]), "%s", CAMPAIGN_FALLBACK[i]);
        g_app.campaign_count = n;
    }
    OL_LOG("Campaign: %d missions (%s ...)\n", g_app.campaign_count,
           g_app.campaign_count ? g_app.campaign[0] : "?");
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

    /* Initialize renderer */
    RenderConfig rcfg = {
        .width      = DEFAULT_WIDTH,
        .height     = DEFAULT_HEIGHT,
        .fov        = DEFAULT_FOV,
        .near_plane = 1.0f,
        .far_plane  = 2048.0f,
    };
    if (!renderer_init(&g_app.renderer, &rcfg, "Outlaws - Open Source Recreation")) {
        OL_ERR("Renderer init failed\n");
        return false;
    }

    /* Debug UI (ImGui) */
    debug_ui_init(g_app.renderer.window, g_app.renderer.gl_ctx);

    /* Audio */
    if (!audio_init(&g_app.audio))
        OL_WARN("Audio init failed - continuing without audio\n");

    /* Input */
    input_init(&g_app.input);

    /* Load HUD assets (weapon sprites, face portrait) BEFORE the level so the
     * HUD/weapon textures are guaranteed a slot even on large levels. */
    load_hud_assets(&g_app.renderer, &g_app.archives);

    /* Global (level-independent) sound effects. */
    load_sfx();

    /* Front-end menu. Start at the menu unless a level was given on the command
     * line (dev / --check), in which case go straight into that level. */
    menu_init(&g_app.menu);
    menu_load_assets(&g_app.menu, &g_app.archives, &g_app.renderer, g_app.sfx_weapon_switch);
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
    OL_LOG("WASD: move  Mouse: look  LMB: fire  1-9: weapon  F5: reload\n");
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
    best->window_broken = true;
    if (best_mirror) best_mirror->window_broken = true;
    audio_play(&g_app.audio, g_app.sfx_glass_break);
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

static void do_player_shoot(void) {
    const WeaponDef *def = &g_weapon_defs[g_app.player.weapons.current];
    Vec3 eye = player_eye_pos(&g_app.player);

    /* Play weapon fire sound */
    audio_play(&g_app.audio, g_app.sfx_weapon[g_app.player.weapons.current]);

    /* Build GL-space shoot direction (same as camera forward) */
    f32 cp = cosf(g_app.player.pitch), sp = sinf(g_app.player.pitch);
    f32 cy = cosf(g_app.player.yaw),   sy = sinf(g_app.player.yaw);
    Vec3 dir = { cy*cp, sp, sy*cp };  /* LVT-space direction */

    /* Melee weapons: short range, no raycast needed */
    if (def->melee) {
        f32 dist;
        int idx = entity_raycast(&g_app.world.entities, eye, dir,
                                 def->range_2, &dist);
        if (idx >= 0) {
            const Entity *e = &g_app.world.entities.entities[idx];
            bool died = entity_damage(&g_app.world.entities, idx, def->damage_1);
            audio_play(&g_app.audio, died ? e->sfx_die : e->sfx_hit);
        }
        return;
    }

    /* Ranged weapons: shatter any glass window along the aim, then hit-scan. */
    break_glass_windows(eye, dir);
    fire_shoot_triggers(eye, dir);
    for (int p = 0; p < def->pellets; p++) {
        Vec3 shot_dir = dir;
        if (def->spread_1 > 0.0f && p > 0) {
            /* Randomize direction within spread cone */
            f32 sx = ((f32)(rand() % 200) - 100) / 100.0f * def->spread_1;
            f32 sy2 = ((f32)(rand() % 200) - 100) / 100.0f * def->spread_1;
            shot_dir.x += sx; shot_dir.y += sy2;
            shot_dir = vec3_norm(shot_dir);
        }
        f32 dist;
        int idx = entity_raycast(&g_app.world.entities, eye, shot_dir,
                                 8192.0f, &dist);
        if (idx >= 0) {
            const Entity *e = &g_app.world.entities.entities[idx];
            bool died = entity_damage(&g_app.world.entities, idx, def->damage_1);
            /* Play hit/die sound (only once per shot, for first pellet hit) */
            if (p == 0)
                audio_play(&g_app.audio, died ? e->sfx_die : e->sfx_hit);
        }
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
        {
            int w, h;
            SDL_GetWindowSize(g_app.renderer.window, &w, &h);
            if (w != g_app.renderer.cfg.width || h != g_app.renderer.cfg.height)
                renderer_resize(&g_app.renderer, w, h);
        }

        /* ---- Front-end menu (main / mission select / options) ---- */
        if (g_app.in_menu) {
            g_app.input.suppress_capture = true;        /* don't grab on click */
            input_capture_mouse(&g_app.input, false);   /* free cursor for the menu */
            renderer_begin_frame(&g_app.renderer);
            menu_frame(&g_app.menu, &g_app.input, &g_app.renderer, &g_app.audio);
            if (g_app.menu.want_quit) { g_app.running = false; break; }
            /* STORY → begin the campaign at mission 1. */
            if (g_app.menu.start_story) {
                g_app.menu.start_story = false;
                if (campaign_load(0, false)) {
                    g_app.in_menu = false;
                    g_app.menu.screen = MENU_INGAME;
                    input_capture_mouse(&g_app.input, true);
                }
            }
            /* MULTIPLAYER / debug poster select → load a single map (no campaign). */
            if (g_app.menu.start_level[0]) {
                char lvl[64]; snprintf(lvl, sizeof(lvl), "%s", g_app.menu.start_level);
                g_app.menu.start_level[0] = '\0';
                if (load_level_runtime(lvl)) {
                    g_app.campaign_active = false;
                    g_app.in_menu = false;
                    g_app.menu.screen = MENU_INGAME;
                    input_capture_mouse(&g_app.input, true);
                }
            }
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
                if (next < g_app.campaign_count) {
                    campaign_load(next, true);   /* carry the loadout forward */
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
        bool fired = player_update(&g_app.player, game_input, dt);
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
            /* Resolve new position (post-movement) against level walls.
             * from = pre-movement pos, to = post-movement pos. */
            Vec3 resolved = collision_resolve(
                &g_app.world.lvt, pre_move_pos, g_app.player.pos,
                PLAYER_RADIUS, PLAYER_BODY_HEIGHT, &g_app.player.sector_idx);
            g_app.player.pos.x = resolved.x;
            g_app.player.pos.z = resolved.z;

            /* Floor / ceiling */
            f32 floor_y = 0, ceil_y = 256;
            collision_heights(&g_app.world.lvt, g_app.player.sector_idx,
                              g_app.player.pos.x, g_app.player.pos.z,
                              &floor_y, &ceil_y);

            /* Gravity: snap to floor */
            if (g_app.player.pos.y <= floor_y) {
                g_app.player.pos.y = floor_y;
                /* Landing sound: was airborne, now on ground */
                if (g_app.was_airborne)
                    audio_play(&g_app.audio, g_app.sfx_player_land);
                g_app.player.vel_y = 0.0f;
                g_app.player.on_ground = true;
                g_app.was_airborne = false;
            } else {
                g_app.player.on_ground = false;
                g_app.was_airborne = true;
            }
            /* Ceiling clamp — crouching lowers the body height so the player
             * fits under low openings (HEIGHT/4). */
            f32 body_h = g_app.player.crouching ? PLAYER_CROUCH_HEIGHT
                                                : PLAYER_BODY_HEIGHT;
            f32 head_y = g_app.player.pos.y + body_h;
            if (head_y > ceil_y) {
                g_app.player.pos.y = ceil_y - body_h;
                if (g_app.player.vel_y > 0) g_app.player.vel_y = 0;
            }

            /* Jump (Space on ground) */
            if (input_key_pressed(&g_app.input, SDL_SCANCODE_SPACE) &&
                g_app.player.on_ground)
                g_app.player.vel_y = 20.0f;  /* ITM AIR_VEL_JUMP=20 */

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
        } /* end noclip check */
        } /* end collision block */

        /* ---- Item pickup (dispatch by kind: heal / ammo / weapon / key) ---- */
        {
            PickupResult pk = entity_try_pickup_ex(&g_app.world.entities,
                                                   g_app.player.pos);
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
                    WeaponType w = WEAPON_SHOTGUN;
                    const char *t = pk.type_name;
                    if      (strncasecmp(t, "GSAWGUN", 7) == 0) w = WEAPON_SAW_GUN;
                    else if (strncasecmp(t, "GSCOPE",  6) == 0) w = WEAPON_RIFLE;
                    else if (strncasecmp(t, "GSGUN",   5) == 0) w = WEAPON_SHOTGUN;
                    else                                        w = WEAPON_SHOTGUN;
                    weapon_pickup(&g_app.player.weapons, w, 20);
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

        /* ---- Weapon firing (primary LMB, alternate RMB) ---- */
        if (fired) {
            do_player_shoot();
            g_app.player.fire_anim = true;
            g_app.player.fire_alt = false;
            g_app.player.fire_anim_timer = 0.0f;
            /* Compute duration from sum of frame dts (ms→sec), fallback 0.25s */
            {
                int wi = (int)g_app.player.weapons.current;
                u32 total_ms = 0;
                for (u32 f = 0; f < g_app.renderer.weapon_fire_frame_count[wi]; f++)
                    total_ms += g_app.renderer.weapon_fire_dt[wi][f];
                g_app.player.fire_anim_dur = total_ms > 0 ? (f32)total_ms / 1000.0f : 0.25f;
            }
        }
        /* Alternate fire (RMB = SDL_BUTTON_RIGHT=3, index 2) */
        if (g_app.input.mouse_buttons[2] && !g_app.player.dead) {
            if (weapon_fire_alt(&g_app.player.weapons)) {
                do_player_shoot();
                g_app.player.fire_anim = true;
                g_app.player.fire_alt = true;
                g_app.player.fire_anim_timer = 0.0f;
                {
                    int wi = (int)g_app.player.weapons.current;
                    u32 total_ms = 0;
                    for (u32 f = 0; f < g_app.renderer.weapon_fire2_frame_count[wi]; f++)
                        total_ms += g_app.renderer.weapon_fire2_dt[wi][f];
                    g_app.player.fire_anim_dur = total_ms > 0 ? (f32)total_ms / 1000.0f : 0.25f;
                }
            }
        }
        /* Manual reload (R key) */
        if (input_key_pressed(&g_app.input, SDL_SCANCODE_R))
            weapon_reload(&g_app.player.weapons);
        /* Automap overlay toggle (TAB) */
        if (input_key_pressed(&g_app.input, SDL_SCANCODE_TAB))
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

        inf_update(&g_app.world.inf, dt, &g_app.world.lvt);
        /* Sync scroll UV offsets every frame (no rebuild needed for scroll) */
        renderer_sync_scroll(&g_app.renderer, &g_app.world.inf);
        if (g_app.world.inf.dirty)
            renderer_build_level(&g_app.renderer, &g_app.world.lvt, &g_app.world.inf);

        /* USE key (E): open the door in front (with lock/key check), fire NUDGE
         * triggers, in the current and adjacent sectors. */
        if (input_key_pressed(&g_app.input, SDL_SCANCODE_E) || g_app.force_use) {
            int si = g_app.player.sector_idx;
            {
                int needed = INF_KEY_NONE;
                u32 keys = (u32)g_app.player.keys;

                /* Nudge the nearest door by proximity (robust vs sector graph). */
                InfDoorResult best = inf_nudge_door_near(
                    &g_app.world.inf, &g_app.world.lvt,
                    g_app.player.pos.x, g_app.player.pos.z, 22.0f, keys, &needed);

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

        /* ---- Render ---- */
        renderer_begin_frame(&g_app.renderer);
        renderer_draw_sky(&g_app.renderer);
        renderer_draw_level(&g_app.renderer);
        renderer_draw_sprites(&g_app.renderer, &g_app.world.entities);

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
                        el->type == ELEV_TYPE_INV_DOOR);
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
         * approach from the doorway on the NEIGHBOUR side. Use the doorway wall
         * midpoint nudged a few units into the adjoining sector. */
        f32 ax = cx, az = cz;
        if (adjw >= 0) {
            const LvtWall *dw = &sec->walls[adjw];
            if (dw->v1 >= 0 && dw->v2 >= 0 &&
                dw->v1 < (i32)sec->vertex_count && dw->v2 < (i32)sec->vertex_count) {
                f32 mx = (sec->vertices[dw->v1].x + sec->vertices[dw->v2].x) * 0.5f;
                f32 mz = (sec->vertices[dw->v1].y + sec->vertices[dw->v2].y) * 0.5f;
                const LvtSector *nb = &lvt->sectors[dw->adjoin];
                f32 ncx = 0, ncz = 0;
                for (u32 v = 0; v < nb->vertex_count; v++) { ncx += nb->vertices[v].x; ncz += nb->vertices[v].y; }
                if (nb->vertex_count) { ncx /= nb->vertex_count; ncz /= nb->vertex_count; }
                f32 dx = ncx - mx, dz = ncz - mz, dl = sqrtf(dx*dx + dz*dz);
                if (dl > 1e-3f) { ax = mx + dx/dl * 4.0f; az = mz + dz/dl * 4.0f; }
                else { ax = mx; az = mz; }
            }
        }

        /* Use the REAL in-game USE path: inf_nudge_door_near (22u radius) from the
         * doorway — this is what pressing E does. A direct by-sector nudge would
         * falsely pass doors the player can't actually reach. */
        int nk = 0;
        /* 1) nudge without any key */
        InfDoorResult r0 = inf_nudge_door_near(inf, lvt, ax, az, 22.0f, 0, &nk);
        bool use_reaches = (r0 != INF_DOOR_NONE);
        /* 2) if it refused (locked), nudge again holding the required key */
        InfDoorResult r1 = r0;
        if (r0 == INF_DOOR_LOCKED && !perm) {
            r1 = inf_nudge_door_near(inf, lvt, ax, az, 22.0f, (1u << el->required_key), &nk);
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
    if (g_app.check_mode) {
        int code = print_level_check();
        app_shutdown();
        return code;
    }
    app_run();
    app_shutdown();
    return 0;
}
