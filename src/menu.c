/*
 * menu.c - Front-end menu (see menu.h)
 *
 * Structure follows the real Outlaws front end (LOCAL.MSG 6000-7607):
 *   Main: MAIN GAME / HISTORICAL MISSIONS / MULTIPLAYER / LOAD / OPTIONS / EXIT
 *   Difficulty: GOOD / BAD / UGLY / CANCEL  (shown before a story/mission launch)
 *   Options: VOLUME (5 channels) / CONTROLS (key binds + mouse) / VIDEO (resolution)
 * Settings live in g_settings and persist to outlaws.cfg (the app saves on change).
 */
#include "menu.h"
#include "world.h"     /* Archives, archives_get */
#include "pcx.h"
#include "settings.h"
#include "wax.h"       /* menu hand cursor (GCURSOR.NWX) */
#include <string.h>
#include <stdlib.h>

/* Main-menu items (LOCAL.MSG 6000-6005 order). The first three are the big
 * centred primary items; the last three are a small bottom row. */
static const char *MAIN_ITEMS[] = {
    "MAIN GAME", "MULTIPLAYER", "HISTORICAL MISSIONS", "LOAD", "OPTIONS", "EXIT"
};
#define MAIN_ITEM_COUNT 6

/* Difficulty items (LOCAL.MSG 6010-6013). GOOD/BAD/UGLY = skill 1/2/3. */
static const char *DIFF_ITEMS[] = { "GOOD", "BAD", "UGLY", "CANCEL" };
#define DIFF_ITEM_COUNT 4

/* Options hub. */
static const char *OPT_ITEMS[] = { "VOLUME", "CONTROLS", "VIDEO", "BACK" };
#define OPT_ITEM_COUNT 4

typedef struct { const char *label, *level; } LevelEntry;

/* Multiplayer maps (LEVELNAME path = data\LEVELS\multi\...). */
static const LevelEntry MP_MAPS[] = {
    { "SANCTUARY",      "TOWN_2P" },
    { "GALLERY",        "GALLERY" },
    { "SHOOTING RANGE", "SHOOT" },
    { "THE OFFICE",     "OFFICE" },
    { "DRY GULCH",      "DRYGULCH" },
    { "THE FORT",       "FORT" },
    { "WILDERNESS",     "WILDERNS" },
    { "GRANARY",        "GRANARY" },
    { "SIMMS RANCH",    "MLTSIMMS" },
};
#define MP_MAP_COUNT ((int)(sizeof(MP_MAPS)/sizeof(MP_MAPS[0])))

/* Historical Missions — the real "CHOOSE GAME" set (own .rca campaigns). NOTE:
 * these ship ONLY as binary .lvb levels (no text .LVT), so they need the binary
 * level loader before they play; the menu is faithful regardless. */
static const LevelEntry HIST_MISSIONS[] = {
    { "CIVIL WAR",        "civilwar" },   /* civilwar.rca → civlwar1, civlwar2 */
    { "ICE CAVES",        "icecaves" },   /* icecaves.rca */
    { "VILLA",            "villa"    },   /* villa.rca */
    { "MARSHAL TRAINING", "htrain"   },   /* single level (no .rca) */
    { "WHARF TOWN",       "wharf"    },   /* wharf.rca */
};
#define HIST_COUNT ((int)(sizeof(HIST_MISSIONS)/sizeof(HIST_MISSIONS[0])))

/* Resolution presets for the VIDEO screen. */
static const struct { int w, h; } RESOS[] = {
    {640,480}, {800,600}, {1024,768}, {1280,720}, {1280,960}, {1600,900}, {1920,1080}
};
#define RESO_COUNT ((int)(sizeof(RESOS)/sizeof(RESOS[0])))

/* Mission-select posters (Pickscrn.pcx) — kept for the debug poster picker. */
typedef struct { const char *name, *level; f32 x0,y0,x1,y1; } Poster;
static const Poster POSTERS[] = {
    { "SPITTIN' JACK SANCHEZ", "TOWN",     0.07f,0.04f, 0.34f,0.46f },
    { "JAMES ANDERSON",        "TRAIN",    0.37f,0.04f, 0.63f,0.46f },
    { "CHIEF TWO FEATHERS",    "WILDERNS", 0.66f,0.04f, 0.93f,0.46f },
    { "DR. DEATH",             "HIDEOUT",  0.07f,0.54f, 0.34f,0.95f },
    { "BOB GRAHAM",            "MILL",     0.37f,0.54f, 0.63f,0.95f },
    { "\"BLOODY MARY\" NASA",  "MINER",    0.66f,0.54f, 0.93f,0.95f },
};
#define POSTER_COUNT 6

void menu_init(Menu *m) {
    memset(m, 0, sizeof(*m));
    m->screen = MENU_MAIN;
    m->music_vol = 7;
    m->sfx_vol = 8;
    m->req_save_slot = -1;
    m->req_load_slot = -1;
    m->rebind_action = -1;
    m->chosen_difficulty = g_settings.difficulty;
    const char *s = getenv("OL_MENU_SCREEN");   /* debug: 1=mission 2=options */
    if (s) m->screen = (MenuScreen)atoi(s);
}

static u32 load_menu_pcx(struct Archives *arc, Renderer *r, const char *file, const char *tname) {
    u32 sz = 0;
    const u8 *d = archives_get(arc, file, &sz);
    if (!d || sz == 0) return 0;
    u32 w = 0, h = 0;
    u8 *rgba = pcx_decode_rgba(d, sz, &w, &h);
    if (!rgba) return 0;
    u32 tex = renderer_upload_texture(r, tname, rgba, w, h);
    free(rgba);
    return tex;
}

void menu_load_assets(Menu *m, struct Archives *arc, Renderer *r, u32 sfx_click) {
    m->tex_main = load_menu_pcx(arc, r, "nmm.pcx",      "menu_main");
    if (!m->tex_main) m->tex_main = load_menu_pcx(arc, r, "MM220.PCX", "menu_main");
    m->tex_pick = load_menu_pcx(arc, r, "Pickscrn.pcx", "menu_pick");
    m->tex_opts = load_menu_pcx(arc, r, "volumes.PCX",  "menu_opts");
    /* Difficulty + Choose-Game screens: parchment-on-the-right backgrounds with
     * the decorative flourishes baked in (real Outlaws md220c / MC220). */
    m->tex_diff   = load_menu_pcx(arc, r, "md220c.pcx", "menu_diff");
    m->tex_choose = load_menu_pcx(arc, r, "MC220.pcx",  "menu_choose");
    /* Pointing-hand cursor drawn beside the highlighted item (GCURSOR.NWX cell
     * 0). It's a UI sprite → authored against the master UI palette (OLPAL). */
    {
        u32 sz = 0;
        const u8 *d = archives_get(arc, "CURSOR.NWX", &sz);   /* the orange pointing hand (50x24) */
        if (d && sz) {
            WaxSprite s;
            if (wax_decode(&s, d, sz, arc->hud_palette) && s.cell_count > 0 &&
                s.cells[0].pixels && s.cells[0].width > 0 && s.cells[0].height > 0) {
                m->tex_hand = renderer_upload_texture(r, "menu_hand",
                    s.cells[0].pixels, s.cells[0].width, s.cells[0].height);
                m->hand_w = (f32)s.cells[0].width;
                m->hand_h = (f32)s.cells[0].height;
            }
            wax_free(&s);
        }
    }
    laf_load(&m->font_big,   arc, r, "mf3s.laf", "font_mf3s");
    laf_load(&m->font_small, arc, r, "sf3.laf",  "font_sf3");
    m->sfx_click = sfx_click;   /* app also sets sfx_nav/select/back directly */
    m->assets_loaded = true;
}

static f32 mtext(Menu *m, Renderer *r, bool small, const char *s,
                 f32 x, f32 y, f32 cap, f32 cr, f32 cg, f32 cb) {
    LafFont *f = small ? &m->font_small : &m->font_big;
    if (f->loaded && f->height > 0) {
        f32 scale = cap / (f32)f->height;
        return laf_draw(f, r, s, x, y, scale, cr, cg, cb, 1.0f);
    }
    f32 px = OL_MAX(1.5f, cap / 8.0f);
    renderer_draw_text(r, s, x, y, px, cr, cg, cb);
    return (f32)strlen(s) * 6.0f * px;
}

static f32 mtext_w(Menu *m, bool small, const char *s, f32 cap) {
    LafFont *f = small ? &m->font_small : &m->font_big;
    if (f->loaded && f->height > 0)
        return laf_text_width(f, s, cap / (f32)f->height);
    return (f32)strlen(s) * 6.0f * OL_MAX(1.5f, cap / 8.0f);
}

/* Sounds: navigate blip / activate / back (fall back to the click sound). */
static void snd(AudioSystem *au, u32 s) { if (s) audio_play(au, s); }
static void nav(Menu *m, AudioSystem *au) { snd(au, m->sfx_nav ? m->sfx_nav : m->sfx_click); }
static void sel(Menu *m, AudioSystem *au) { snd(au, m->sfx_select ? m->sfx_select : m->sfx_click); }
static void back_snd(Menu *m, AudioSystem *au) { snd(au, m->sfx_back ? m->sfx_back : m->sfx_click); }

static void draw_cursor(Menu *m, Renderer *r, f32 mx, f32 my, f32 H);

static void draw_bg(Renderer *r, u32 tex, f32 W, f32 H) {
    if (tex) renderer_draw_image(r, tex, 0, 0, W, H, 1.0f, 1.0f);
    else     renderer_draw_rect(r, 0, 0, W, H, 0.15f, 0.10f, 0.05f, 1.0f);
}

/* Draw a vertical list of items; returns activated index (-1 none). Handles
 * hover-select, up/down nav (with sound), and enter/click activation. */
static int list_menu(Menu *m, InputState *in, Renderer *r, AudioSystem *au,
                     const char *const *items, int n, bool clicked,
                     f32 ix, f32 iy0, f32 dy, f32 cap) {
    for (int i = 0; i < n; i++) {
        f32 iy = iy0 + i*dy;
        f32 tw = mtext_w(m, false, items[i], cap);
        if (in->mouse_x >= ix && in->mouse_x <= ix+tw &&
            in->mouse_y >= iy && in->mouse_y <= iy+cap) m->sel = i;
        bool hot = (m->sel == i);
        mtext(m, r, false, items[i], ix, iy, cap,
              hot?1.0f:0.75f, hot?0.9f:0.55f, hot?0.3f:0.2f);
    }
    if (input_key_pressed(in, SDL_SCANCODE_DOWN)) { m->sel=(m->sel+1)%n; nav(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_UP))   { m->sel=(m->sel+n-1)%n; nav(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_RETURN) ||
        input_key_pressed(in, SDL_SCANCODE_SPACE) || clicked) return m->sel;
    return -1;
}

/* Begin a launch: go to the difficulty screen first (story / historical / MP). */
static void begin_launch(Menu *m, int kind, const char *level) {
    m->pending_kind = kind;
    if (level) snprintf(m->pending_level, sizeof(m->pending_level), "%s", level);
    m->diff_return = m->screen;
    m->sel = g_settings.difficulty - 1;   /* preselect current skill */
    if (m->sel < 0 || m->sel > 2) m->sel = 1;
    m->screen = MENU_DIFFICULTY;
}

/* The pointing hand IS the mouse cursor in the original — draw it at the pointer
 * with the fingertip (right-centre of the sprite) as the hotspot. Scaled to the
 * 640x480 authoring space so it stays the right size at any window resolution. */
static void draw_cursor(Menu *m, Renderer *r, f32 mx, f32 my, f32 H) {
    if (!m->tex_hand || m->hand_h <= 0) return;
    f32 sc = H / 480.0f;
    f32 w = m->hand_w * sc, h = m->hand_h * sc;
    f32 x = mx - w;              /* fingertip at the right edge → sits at the pointer */
    f32 y = my - h * 0.5f;
    renderer_draw_image(r, m->tex_hand, x, y, w, h, 1.0f, 1.0f);
}

/* Draw one horizontally-centred item at center-x `cx`, top `y`; hover-selects. */
static void item_centered(Menu *m, InputState *in, Renderer *r, bool small,
                          const char *s, f32 cx, f32 y, f32 cap, int idx) {
    f32 tw = mtext_w(m, small, s, cap);
    f32 x = cx - tw*0.5f;
    if (in->mouse_x >= x && in->mouse_x <= x+tw &&
        in->mouse_y >= y && in->mouse_y <= y+cap) m->sel = idx;
    bool hot = (m->sel == idx);
    mtext(m, r, small, s, x, y, cap, hot?1.0f:0.72f, hot?0.9f:0.5f, hot?0.28f:0.16f);
}

/* ---- Main menu: OUTLAWS title, 3 big centred items, bottom row LOAD/OPT/EXIT --- */
static void menu_main(Menu *m, InputState *in, Renderer *r, AudioSystem *au,
                      f32 W, f32 H, bool clicked) {
    draw_bg(r, m->tex_main, W, H);
    f32 cx = W*0.285f;   /* centre of the left parchment */
    { const char *t = "OUTLAWS";
      mtext(m, r, false, t, cx - mtext_w(m,false,t,H*0.13f)*0.5f, H*0.085f, H*0.13f,
            0.82f, 0.66f, 0.26f); }
    /* Three primary items, big and centred. */
    f32 py[3] = { H*0.40f, H*0.545f, H*0.69f };
    for (int i = 0; i < 3; i++) item_centered(m, in, r, false, MAIN_ITEMS[i], cx, py[i], H*0.072f, i);
    /* Bottom row: LOAD  OPTIONS  EXIT. */
    f32 bx[3] = { W*0.11f, W*0.285f, W*0.46f };
    for (int i = 0; i < 3; i++) item_centered(m, in, r, false, MAIN_ITEMS[3+i], bx[i], H*0.88f, H*0.052f, 3+i);
    /* Version string (bottom-left), like the original. */
    mtext(m, r, true, "Outlaws - open recreation  v2.0", W*0.015f, H*0.955f, H*0.028f, 0.55f,0.5f,0.42f);

    if (input_key_pressed(in, SDL_SCANCODE_DOWN))  { m->sel=(m->sel+1)%MAIN_ITEM_COUNT; nav(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_UP))    { m->sel=(m->sel+MAIN_ITEM_COUNT-1)%MAIN_ITEM_COUNT; nav(m,au); }
    if (m->sel >= 3) {   /* bottom row also moves with left/right */
        if (input_key_pressed(in, SDL_SCANCODE_RIGHT)) { m->sel = 3 + (m->sel-3+1)%3; nav(m,au); }
        if (input_key_pressed(in, SDL_SCANCODE_LEFT))  { m->sel = 3 + (m->sel-3+2)%3; nav(m,au); }
    }
    if (!(input_key_pressed(in, SDL_SCANCODE_RETURN) ||
          input_key_pressed(in, SDL_SCANCODE_SPACE) || clicked)) return;
    sel(m, au);
    switch (m->sel) {
    case 0: begin_launch(m, 1, NULL); break;                              /* MAIN GAME */
    case 1: m->screen = MENU_MP;         m->sel = 0; break;               /* MULTIPLAYER */
    case 2: m->screen = MENU_HISTORICAL; m->sel = 0; break;               /* HISTORICAL MISSIONS */
    case 3: m->load_return = MENU_MAIN; m->screen = MENU_LOAD; m->sel = 0; break; /* LOAD */
    case 4: m->return_screen = MENU_MAIN; m->screen = MENU_OPTIONS; m->sel = 0; break; /* OPTIONS */
    default: m->want_quit = true; break;                                  /* EXIT */
    }
}

/* ---- Difficulty (GOOD / BAD / UGLY / CANCEL) — parchment on the RIGHT ---- */
static void menu_difficulty(Menu *m, InputState *in, Renderer *r, AudioSystem *au,
                            f32 W, f32 H, bool clicked) {
    draw_bg(r, m->tex_diff ? m->tex_diff : m->tex_main, W, H);
    f32 cx = W*0.70f;   /* centre of the right parchment */
    { const char *t = "DIFFICULTY";
      mtext(m, r, false, t, cx - mtext_w(m,false,t,H*0.10f)*0.5f, H*0.13f, H*0.10f, 0.82f,0.66f,0.26f); }
    f32 py[3] = { H*0.37f, H*0.51f, H*0.65f };
    for (int i = 0; i < 3; i++) item_centered(m, in, r, false, DIFF_ITEMS[i], cx, py[i], H*0.08f, i);
    item_centered(m, in, r, false, DIFF_ITEMS[3], W*0.82f, H*0.87f, H*0.055f, 3);   /* CANCEL */

    if (input_key_pressed(in, SDL_SCANCODE_DOWN)) { m->sel=(m->sel+1)%DIFF_ITEM_COUNT; nav(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_UP))   { m->sel=(m->sel+DIFF_ITEM_COUNT-1)%DIFF_ITEM_COUNT; nav(m,au); }
    bool esc = input_key_pressed(in, SDL_SCANCODE_ESCAPE);
    bool act = input_key_pressed(in, SDL_SCANCODE_RETURN) ||
               input_key_pressed(in, SDL_SCANCODE_SPACE) || clicked;
    if (esc || (act && m->sel == 3)) {
        back_snd(m,au); m->screen = m->diff_return; m->sel = 0; m->pending_kind = 0; return;
    }
    if (!act) return;
    sel(m, au);
    m->chosen_difficulty = m->sel + 1;   /* GOOD=1, BAD=2, UGLY=3 */
    if (m->pending_kind == 1)       m->start_story = true;
    else if (m->pending_kind == 2)  snprintf(m->start_level, sizeof(m->start_level), "%s", m->pending_level);
    else if (m->pending_kind == 3) { m->start_historical = true;
        snprintf(m->start_level, sizeof(m->start_level), "%s", m->pending_level); }
    m->pending_kind = 0;
}

/* ---- Choose Game (historical / MP): parchment on the RIGHT, centred list ---- */
static void menu_levellist(Menu *m, InputState *in, Renderer *r, AudioSystem *au,
                           f32 W, f32 H, bool clicked, const char *title,
                           const LevelEntry *list, int n) {
    draw_bg(r, m->tex_choose ? m->tex_choose : m->tex_main, W, H);
    f32 cx = W*0.70f;
    mtext(m, r, false, title, cx - mtext_w(m,false,title,H*0.085f)*0.5f, H*0.07f, H*0.085f, 0.82f,0.66f,0.26f);

    f32 cap = H*0.05f, iy0 = H*0.30f, dy = H*0.082f;
    for (int i = 0; i < n; i++)
        item_centered(m, in, r, true, list[i].label, cx, iy0 + i*dy, cap, i);
    item_centered(m, in, r, false, "CANCEL", W*0.82f, H*0.90f, H*0.05f, n);   /* CANCEL row = n */

    if (input_key_pressed(in, SDL_SCANCODE_DOWN)) { m->sel=(m->sel+1)%(n+1); nav(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_UP))   { m->sel=(m->sel+n)%(n+1); nav(m,au); }
    bool esc = input_key_pressed(in, SDL_SCANCODE_ESCAPE);
    bool act = input_key_pressed(in, SDL_SCANCODE_RETURN) || clicked;
    if (esc || (act && m->sel == n)) { back_snd(m,au); m->screen=MENU_MAIN; m->sel=0; return; }
    if (act && m->sel < n) {
        int kind = (m->screen == MENU_HISTORICAL) ? 3 : 2;  /* 3 = .rca campaign */
        begin_launch(m, kind, list[m->sel].level);
    }
}

/* ---- Options hub ---- */
static void menu_options(Menu *m, InputState *in, Renderer *r, AudioSystem *au,
                         f32 W, f32 H, bool clicked) {
    draw_bg(r, m->tex_opts, W, H);
    mtext(m, r, false, "OPTIONS", W*0.5f - mtext_w(m,false,"OPTIONS",H*0.08f)*0.5f,
          H*0.08f, H*0.08f, 0.9f,0.8f,0.3f);
    int act = list_menu(m, in, r, au, OPT_ITEMS, OPT_ITEM_COUNT, clicked,
                        W*0.36f, H*0.34f, H*0.10f, H*0.055f);
    if (input_key_pressed(in, SDL_SCANCODE_ESCAPE)) {
        back_snd(m,au); m->screen = (m->return_screen==MENU_PAUSE)?MENU_PAUSE:MENU_MAIN; m->sel=0; return;
    }
    if (act < 0) return;
    sel(m, au);
    switch (act) {
    case 0: m->screen = MENU_OPT_VOLUME;   m->sel = 0; break;
    case 1: m->screen = MENU_OPT_CONTROLS; m->sel = 0; m->ctrl_scroll = 0; m->rebind_action = -1; break;
    case 2: m->screen = MENU_OPT_VIDEO;    m->sel = 0; break;
    default: m->screen = (m->return_screen==MENU_PAUSE)?MENU_PAUSE:MENU_MAIN; m->sel=0; break;
    }
}

/* Apply the five volume channels to the audio system (master-scaled). */
static void apply_volumes(AudioSystem *au) {
    audio_set_music_volume(au, settings_gain(&g_settings, VOL_MUSIC));
    audio_set_sfx_volume(au,   settings_gain(&g_settings, VOL_EFFECTS));
}

/* ---- Options: VOLUME (5 channels) ---- */
static void menu_opt_volume(Menu *m, InputState *in, Renderer *r, AudioSystem *au,
                            f32 W, f32 H, bool clicked) {
    draw_bg(r, m->tex_opts, W, H);
    mtext(m, r, false, "VOLUME CONTROL", W*0.5f - mtext_w(m,false,"VOLUME CONTROL",H*0.07f)*0.5f,
          H*0.06f, H*0.07f, 0.9f,0.8f,0.3f);
    int rows = VOL_COUNT + 1;   /* channels + BACK */
    f32 cap = H*0.042f, ix = W*0.18f, iy0 = H*0.24f, dy = H*0.095f;
    char buf[96];
    for (int i = 0; i < rows; i++) {
        f32 iy = iy0 + i*dy;
        if (i < VOL_COUNT) {
            int v = g_settings.volume[i];
            char bar[24]; int k=0; int fill=(v*20)/100; for (; k<20; k++) bar[k]=(k<fill)?'#':'-'; bar[k]='\0';
            snprintf(buf, sizeof(buf), "%-15s [%s] %3d", vol_label(i), bar, v);
        } else snprintf(buf, sizeof(buf), "BACK");
        f32 tw = mtext_w(m, false, buf, cap);
        if (in->mouse_x >= ix && in->mouse_x <= ix+tw &&
            in->mouse_y >= iy && in->mouse_y <= iy+cap) m->sel = i;
        bool hot = (m->sel == i);
        mtext(m, r, false, buf, ix, iy, cap, hot?1.0f:0.72f, hot?0.9f:0.52f, hot?0.3f:0.2f);
    }
    if (input_key_pressed(in, SDL_SCANCODE_DOWN)) { m->sel=(m->sel+1)%rows; nav(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_UP))   { m->sel=(m->sel+rows-1)%rows; nav(m,au); }
    if (m->sel < VOL_COUNT) {
        int *v = &g_settings.volume[m->sel];
        bool ch = false;
        if (input_key_pressed(in, SDL_SCANCODE_RIGHT) && *v<100) { *v = OL_MIN(100,*v+5); ch=true; }
        if (input_key_pressed(in, SDL_SCANCODE_LEFT)  && *v>0)   { *v = OL_MAX(0,*v-5);   ch=true; }
        if (ch) { nav(m,au); apply_volumes(au); m->settings_dirty = true; }
    }
    bool esc = input_key_pressed(in, SDL_SCANCODE_ESCAPE);
    bool activate = input_key_pressed(in, SDL_SCANCODE_RETURN) || clicked;
    if (esc || (activate && m->sel == VOL_COUNT)) {
        back_snd(m,au); m->screen = MENU_OPTIONS; m->sel = 0; m->settings_dirty = true;
    }
}

/* Scan for the first key newly pressed this frame (for rebinding). */
static SDL_Scancode first_pressed_key(InputState *in) {
    for (int k = 4; k < KEY_MAX; k++)   /* skip 0-3 (none/unused) */
        if (in->keys[k] && !in->keys_prev[k]) return (SDL_Scancode)k;
    return SDL_SCANCODE_UNKNOWN;
}

/* ---- Options: CONTROLS (key bindings + mouse) ---- */
static void menu_opt_controls(Menu *m, InputState *in, Renderer *r, AudioSystem *au,
                              f32 W, f32 H, bool clicked) {
    draw_bg(r, m->tex_opts, W, H);
    mtext(m, r, false, "CONTROLS", W*0.5f - mtext_w(m,false,"CONTROLS",H*0.06f)*0.5f,
          H*0.03f, H*0.06f, 0.9f,0.8f,0.3f);

    /* Rows: BIND_COUNT actions + mouse sensitivity + invert + BACK. */
    int rows = BIND_COUNT + 3;
    int MOUSE_SENS = BIND_COUNT, MOUSE_INV = BIND_COUNT+1, BACK_ROW = BIND_COUNT+2;

    /* If waiting for a key, capture it. */
    if (m->rebind_action >= 0) {
        if (input_key_pressed(in, SDL_SCANCODE_ESCAPE)) { m->rebind_action = -1; back_snd(m,au); }
        else {
            SDL_Scancode k = first_pressed_key(in);
            if (k != SDL_SCANCODE_UNKNOWN) {
                g_settings.bind[m->rebind_action] = k;
                m->rebind_action = -1; sel(m,au); m->settings_dirty = true;
            }
        }
    }

    /* Visible window (scrolling list). */
    f32 cap = H*0.032f, ix = W*0.12f, vx = W*0.62f, iy0 = H*0.13f, dy = H*0.040f;
    int visible = 18;
    if (m->sel < m->ctrl_scroll) m->ctrl_scroll = m->sel;
    if (m->sel >= m->ctrl_scroll + visible) m->ctrl_scroll = m->sel - visible + 1;

    char lbl[64], val[64];
    for (int vi = 0; vi < visible; vi++) {
        int i = m->ctrl_scroll + vi;
        if (i >= rows) break;
        f32 iy = iy0 + vi*dy;
        if (i < BIND_COUNT) {
            snprintf(lbl, sizeof(lbl), "%s", bind_label(i));
            const char *kn = SDL_GetScancodeName((SDL_Scancode)g_settings.bind[i]);
            if (m->rebind_action == i) snprintf(val, sizeof(val), "< press a key >");
            else snprintf(val, sizeof(val), "%s", (kn && kn[0]) ? kn : "---");
        } else if (i == MOUSE_SENS) {
            snprintf(lbl, sizeof(lbl), "Mouse Sensitivity");
            snprintf(val, sizeof(val), "%.2f", g_settings.mouse_sensitivity);
        } else if (i == MOUSE_INV) {
            snprintf(lbl, sizeof(lbl), "Invert Mouse");
            snprintf(val, sizeof(val), "%s", g_settings.mouse_invert ? "ON" : "OFF");
        } else { snprintf(lbl, sizeof(lbl), "BACK"); val[0] = '\0'; }

        bool hot = (m->sel == i);
        if (in->mouse_x >= ix && in->mouse_x <= W*0.9f &&
            in->mouse_y >= iy && in->mouse_y <= iy+cap) m->sel = i;
        mtext(m, r, true, lbl, ix, iy, cap, hot?1.0f:0.72f, hot?0.9f:0.52f, hot?0.3f:0.2f);
        if (val[0]) mtext(m, r, true, val, vx, iy, cap, hot?1.0f:0.7f, hot?0.85f:0.5f, hot?0.4f:0.3f);
    }

    if (m->rebind_action >= 0) return;   /* swallow nav while capturing */

    if (input_key_pressed(in, SDL_SCANCODE_DOWN)) { m->sel=(m->sel+1)%rows; nav(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_UP))   { m->sel=(m->sel+rows-1)%rows; nav(m,au); }
    bool activate = input_key_pressed(in, SDL_SCANCODE_RETURN) || clicked;
    if (m->sel == MOUSE_SENS) {
        bool ch=false;
        if (input_key_pressed(in, SDL_SCANCODE_RIGHT) && g_settings.mouse_sensitivity<4.0f) { g_settings.mouse_sensitivity+=0.25f; ch=true; }
        if (input_key_pressed(in, SDL_SCANCODE_LEFT)  && g_settings.mouse_sensitivity>0.25f) { g_settings.mouse_sensitivity-=0.25f; ch=true; }
        if (ch) { nav(m,au); m->settings_dirty=true; }
    }
    if (input_key_pressed(in, SDL_SCANCODE_ESCAPE)) { back_snd(m,au); m->screen=MENU_OPTIONS; m->sel=1; m->settings_dirty=true; return; }
    if (activate) {
        if (m->sel < BIND_COUNT)        { m->rebind_action = m->sel; sel(m,au); }
        else if (m->sel == MOUSE_INV)   { g_settings.mouse_invert = !g_settings.mouse_invert; sel(m,au); m->settings_dirty=true; }
        else if (m->sel == BACK_ROW)    { back_snd(m,au); m->screen=MENU_OPTIONS; m->sel=1; m->settings_dirty=true; }
    }
}

/* ---- Options: VIDEO (resolution + fullscreen) ---- */
static void menu_opt_video(Menu *m, InputState *in, Renderer *r, AudioSystem *au,
                           f32 W, f32 H, bool clicked) {
    draw_bg(r, m->tex_opts, W, H);
    mtext(m, r, false, "VIDEO", W*0.5f - mtext_w(m,false,"VIDEO",H*0.07f)*0.5f,
          H*0.05f, H*0.07f, 0.9f,0.8f,0.3f);
    int rows = RESO_COUNT + 2;   /* resolutions + fullscreen + BACK */
    int FS_ROW = RESO_COUNT, BACK_ROW = RESO_COUNT+1;
    f32 cap = H*0.038f, ix = W*0.30f, iy0 = H*0.20f, dy = H*0.058f;
    char buf[64];
    for (int i = 0; i < rows; i++) {
        f32 iy = iy0 + i*dy;
        if (i < RESO_COUNT) {
            bool cur = (g_settings.win_w==RESOS[i].w && g_settings.win_h==RESOS[i].h);
            snprintf(buf, sizeof(buf), "%s%d x %d", cur?"> ":"  ", RESOS[i].w, RESOS[i].h);
        } else if (i == FS_ROW) snprintf(buf, sizeof(buf), "FULLSCREEN: %s", g_settings.fullscreen?"ON":"OFF");
        else snprintf(buf, sizeof(buf), "BACK");
        f32 tw = mtext_w(m, false, buf, cap);
        if (in->mouse_x >= ix && in->mouse_x <= ix+tw &&
            in->mouse_y >= iy && in->mouse_y <= iy+cap) m->sel = i;
        bool hot = (m->sel == i);
        mtext(m, r, false, buf, ix, iy, cap, hot?1.0f:0.72f, hot?0.9f:0.52f, hot?0.3f:0.2f);
    }
    if (input_key_pressed(in, SDL_SCANCODE_DOWN)) { m->sel=(m->sel+1)%rows; nav(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_UP))   { m->sel=(m->sel+rows-1)%rows; nav(m,au); }
    bool activate = input_key_pressed(in, SDL_SCANCODE_RETURN) || clicked;
    if (input_key_pressed(in, SDL_SCANCODE_ESCAPE)) { back_snd(m,au); m->screen=MENU_OPTIONS; m->sel=2; return; }
    if (activate) {
        if (m->sel < RESO_COUNT) {
            g_settings.win_w = RESOS[m->sel].w; g_settings.win_h = RESOS[m->sel].h;
            m->apply_video = true; m->settings_dirty = true; sel(m,au);
        } else if (m->sel == FS_ROW) {
            g_settings.fullscreen = !g_settings.fullscreen;
            m->apply_video = true; m->settings_dirty = true; sel(m,au);
        } else if (m->sel == BACK_ROW) { back_snd(m,au); m->screen=MENU_OPTIONS; m->sel=2; }
    }
}

/* ---- Debug poster picker (unchanged) ---- */
static void menu_mission(Menu *m, InputState *in, Renderer *r, AudioSystem *au,
                         f32 W, f32 H, bool clicked) {
    draw_bg(r, m->tex_pick, W, H);
    const char *t = "SELECT A POSTER TO CHOOSE YOUR MISSION";
    f32 cap = H * 0.035f;
    mtext(m, r, true, t, W*0.5f - mtext_w(m, true, t, cap)*0.5f, H*0.005f, cap, 1.0f, 0.9f, 0.5f);
    int hover = -1;
    for (int i = 0; i < POSTER_COUNT; i++) {
        f32 x0 = POSTERS[i].x0*W, y0 = POSTERS[i].y0*H, x1 = POSTERS[i].x1*W, y1 = POSTERS[i].y1*H;
        if (in->mouse_x>=x0 && in->mouse_x<=x1 && in->mouse_y>=y0 && in->mouse_y<=y1) hover = i;
    }
    if (hover >= 0) m->sel = hover;
    { f32 x0=POSTERS[m->sel].x0*W,y0=POSTERS[m->sel].y0*H,x1=POSTERS[m->sel].x1*W,y1=POSTERS[m->sel].y1*H;
      f32 t2=OL_MAX(2.0f,H/240.0f);
      renderer_draw_rect(r,x0,y0,x1-x0,t2,1,0.9f,0.3f,0.9f); renderer_draw_rect(r,x0,y1-t2,x1-x0,t2,1,0.9f,0.3f,0.9f);
      renderer_draw_rect(r,x0,y0,t2,y1-y0,1,0.9f,0.3f,0.9f); renderer_draw_rect(r,x1-t2,y0,t2,y1-y0,1,0.9f,0.3f,0.9f); }
    if (input_key_pressed(in, SDL_SCANCODE_RIGHT)) { m->sel=(m->sel+1)%POSTER_COUNT; nav(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_LEFT))  { m->sel=(m->sel+POSTER_COUNT-1)%POSTER_COUNT; nav(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_ESCAPE)) { m->screen=MENU_MAIN; m->sel=0; back_snd(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_RETURN) || clicked) { sel(m,au); begin_launch(m, 2, POSTERS[m->sel].level); }
}

/* ---- In-game pause menu + save/load slot lists ---- */
static const char *PAUSE_ITEMS[] = {
    "RESUME", "SAVE GAME", "LOAD GAME", "OPTIONS", "QUIT TO MENU"
};
#define PAUSE_ITEM_COUNT 5

void menu_pause_open(Menu *m) {
    m->screen = MENU_PAUSE;
    m->sel = 0;
    m->want_resume = m->want_quit_menu = false;
    m->req_save_slot = m->req_load_slot = -1;
}

static void pause_dim(Renderer *r, f32 W, f32 H) {
    renderer_draw_rect(r, 0, 0, W, H, 0.0f, 0.0f, 0.0f, 0.55f);
}

static void menu_pause(Menu *m, InputState *in, Renderer *r, AudioSystem *au,
                       f32 W, f32 H, bool clicked) {
    pause_dim(r, W, H);
    mtext(m, r, false, "PAUSED", W*0.5f - mtext_w(m,false,"PAUSED",H*0.09f)*0.5f,
          H*0.12f, H*0.09f, 0.85f, 0.70f, 0.30f);
    f32 cap = H*0.055f, ix = W*0.34f, iy0 = H*0.34f, dy = H*0.10f;
    for (int i = 0; i < PAUSE_ITEM_COUNT; i++) {
        f32 iy = iy0 + i*dy;
        f32 tw = mtext_w(m, false, PAUSE_ITEMS[i], cap);
        if (in->mouse_x >= ix && in->mouse_x <= ix+tw &&
            in->mouse_y >= iy && in->mouse_y <= iy+cap) m->sel = i;
        bool hot = (m->sel == i);
        mtext(m, r, false, PAUSE_ITEMS[i], ix, iy, cap, hot?1.0f:0.75f, hot?0.9f:0.55f, hot?0.3f:0.2f);
    }
    if (input_key_pressed(in, SDL_SCANCODE_DOWN)) { m->sel=(m->sel+1)%PAUSE_ITEM_COUNT; nav(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_UP))   { m->sel=(m->sel+PAUSE_ITEM_COUNT-1)%PAUSE_ITEM_COUNT; nav(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_ESCAPE)) { m->want_resume = true; back_snd(m,au); return; }
    if (input_key_pressed(in, SDL_SCANCODE_RETURN) || input_key_pressed(in, SDL_SCANCODE_SPACE) || clicked) {
        sel(m, au);
        switch (m->sel) {
        case 0: m->want_resume = true; break;
        case 1: m->screen = MENU_SAVE; m->sel = 0; break;
        case 2: m->load_return = MENU_PAUSE; m->screen = MENU_LOAD; m->sel = 0; break;
        case 3: m->return_screen = MENU_PAUSE; m->screen = MENU_OPTIONS; m->sel = 0; break;
        default: m->want_quit_menu = true; break;
        }
    }
}

static void menu_slots(Menu *m, InputState *in, Renderer *r, AudioSystem *au,
                       f32 W, f32 H, bool clicked, bool save) {
    /* From the pause menu we overlay the frozen frame; from the main menu we sit
     * on the parchment. */
    if (save || m->load_return == MENU_PAUSE) pause_dim(r, W, H);
    else draw_bg(r, m->tex_main, W, H);
    const char *title = save ? "SAVE GAME" : "LOAD GAME";
    mtext(m, r, false, title, W*0.5f - mtext_w(m,false,title,H*0.08f)*0.5f,
          H*0.08f, H*0.08f, 0.85f, 0.70f, 0.30f);
    f32 cap = H*0.045f, ix = W*0.18f, iy0 = H*0.28f, dy = H*0.085f;
    char buf[64];
    for (int i = 0; i < SAVE_SLOTS; i++) {
        f32 iy = iy0 + i*dy;
        if (m->slot_used[i]) snprintf(buf, sizeof(buf), "%d. %s", i+1, m->slot_label[i]);
        else                 snprintf(buf, sizeof(buf), "%d. [ EMPTY ]", i+1);
        f32 tw = mtext_w(m, false, buf, cap);
        if (in->mouse_x >= ix && in->mouse_x <= ix+tw &&
            in->mouse_y >= iy && in->mouse_y <= iy+cap) m->sel = i;
        bool hot = (m->sel == i);
        bool selectable = save || m->slot_used[i];
        f32 dim = selectable ? 1.0f : 0.45f;
        mtext(m, r, false, buf, ix, iy, cap,
              (hot?1.0f:0.7f)*dim, (hot?0.9f:0.55f)*dim, (hot?0.3f:0.2f)*dim);
    }
    if (input_key_pressed(in, SDL_SCANCODE_DOWN)) { m->sel=(m->sel+1)%SAVE_SLOTS; nav(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_UP))   { m->sel=(m->sel+SAVE_SLOTS-1)%SAVE_SLOTS; nav(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_ESCAPE)) {
        back_snd(m,au);
        m->screen = save ? MENU_PAUSE : m->load_return;
        m->sel = 0; return;
    }
    if (input_key_pressed(in, SDL_SCANCODE_RETURN) || clicked) {
        if (save) { m->req_save_slot = m->sel; sel(m,au); }
        else if (m->slot_used[m->sel]) { m->req_load_slot = m->sel; sel(m,au); }
    }
}

void menu_frame(Menu *m, InputState *in, Renderer *r, AudioSystem *au) {
    f32 W = (f32)r->cfg.width, H = (f32)r->cfg.height;
    bool down = in->mouse_buttons[0];
    bool clicked = down && !m->prev_click;
    m->prev_click = down;

    switch (m->screen) {
    case MENU_MAIN:        menu_main(m, in, r, au, W, H, clicked); break;
    case MENU_DIFFICULTY:  menu_difficulty(m, in, r, au, W, H, clicked); break;
    case MENU_HISTORICAL:  menu_levellist(m, in, r, au, W, H, clicked, "CHOOSE GAME", HIST_MISSIONS, HIST_COUNT); break;
    case MENU_MP:          menu_levellist(m, in, r, au, W, H, clicked, "MULTIPLAYER", MP_MAPS, MP_MAP_COUNT); break;
    case MENU_MISSION:     menu_mission(m, in, r, au, W, H, clicked); break;
    case MENU_OPTIONS:     menu_options(m, in, r, au, W, H, clicked); break;
    case MENU_OPT_VOLUME:  menu_opt_volume(m, in, r, au, W, H, clicked); break;
    case MENU_OPT_CONTROLS:menu_opt_controls(m, in, r, au, W, H, clicked); break;
    case MENU_OPT_VIDEO:   menu_opt_video(m, in, r, au, W, H, clicked); break;
    case MENU_PAUSE:       menu_pause(m, in, r, au, W, H, clicked); break;
    case MENU_SAVE:        menu_slots(m, in, r, au, W, H, clicked, true);  break;
    case MENU_LOAD:        menu_slots(m, in, r, au, W, H, clicked, false); break;
    default: break;
    }

    /* The pointing hand IS the mouse cursor — draw it last (on top) at the
     * pointer and hide the OS cursor while a menu is up. */
    if (m->tex_hand) {
        SDL_ShowCursor(SDL_DISABLE);
        draw_cursor(m, r, (f32)in->mouse_x, (f32)in->mouse_y, H);
    }
}
