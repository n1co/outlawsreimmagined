/*
 * menu.c - Front-end menu (see menu.h)
 */
#include "menu.h"
#include "world.h"     /* Archives, archives_get */
#include "pcx.h"
#include <string.h>
#include <stdlib.h>

/* Main-menu items on the parchment (Outlaws front end). */
static const char *MAIN_ITEMS[] = { "STORY", "MULTIPLAYER", "OPTIONS", "QUIT" };
#define MAIN_ITEM_COUNT 4

/* Multiplayer maps (simple picker). */
typedef struct { const char *label, *level; } MpMap;
static const MpMap MP_MAPS[] = {
    { "SANCTUARY (2P)", "TOWN_2P" },
    { "GALLERY",        "GALLERY" },
    { "SHOOTING RANGE", "SHOOT" },
    { "OFFICE",         "OFFICE" },
};
#define MP_MAP_COUNT 4

/* Mission-select posters (Pickscrn.pcx): the six gang bosses, each launching a
 * story mission. Regions are normalised (0..1) over the 640x480 artwork. The
 * boss→level mapping follows the campaign (Sanchez = Sanctuary/TOWN, confirmed;
 * the rest are the boss levels — tweakable). */
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
    /* Authentic front-end fonts. */
    laf_load(&m->font_big,   arc, r, "mf3s.laf", "font_mf3s");
    laf_load(&m->font_small, arc, r, "sf3.laf",  "font_sf3");
    m->sfx_click = sfx_click;
    m->assets_loaded = true;
}

/*
 * Draw menu text with an authentic LAF font when available, falling back to the
 * embedded 5x7 font. `cap` is the desired glyph height in pixels (the LAF is
 * scaled to it). Returns the text advance width. (x,y) = top-left.
 */
static f32 mtext(Menu *m, Renderer *r, bool small, const char *s,
                 f32 x, f32 y, f32 cap, f32 cr, f32 cg, f32 cb) {
    LafFont *f = small ? &m->font_small : &m->font_big;
    if (f->loaded && f->height > 0) {
        f32 scale = cap / (f32)f->height;
        return laf_draw(f, r, s, x, y, scale, cr, cg, cb, 1.0f);
    }
    f32 px = OL_MAX(1.5f, cap / 8.0f);           /* 5x7 fallback */
    renderer_draw_text(r, s, x, y, px, cr, cg, cb);
    return (f32)strlen(s) * 6.0f * px;
}

/* Advance width of menu text at the given cap height. */
static f32 mtext_w(Menu *m, bool small, const char *s, f32 cap) {
    LafFont *f = small ? &m->font_small : &m->font_big;
    if (f->loaded && f->height > 0)
        return laf_text_width(f, s, cap / (f32)f->height);
    return (f32)strlen(s) * 6.0f * OL_MAX(1.5f, cap / 8.0f);
}

static void click(Menu *m, AudioSystem *au) {
    if (m->sfx_click) audio_play(au, m->sfx_click);
}

/* Draw the shared background stretched to the window. */
static void draw_bg(Renderer *r, u32 tex, f32 W, f32 H) {
    if (tex) renderer_draw_image(r, tex, 0, 0, W, H, 1.0f, 1.0f);
    else     renderer_draw_rect(r, 0, 0, W, H, 0.15f, 0.10f, 0.05f, 1.0f);
}

static void menu_main(Menu *m, InputState *in, Renderer *r, AudioSystem *au,
                      f32 W, f32 H, bool clicked) {
    draw_bg(r, m->tex_main, W, H);
    mtext(m, r, false, "OUTLAWS", W*0.06f, H*0.10f, H*0.12f, 0.85f, 0.70f, 0.30f);

    f32 cap = H * 0.062f;
    f32 ix = W * 0.08f, iy0 = H * 0.40f, dy = H * 0.11f;
    for (int i = 0; i < MAIN_ITEM_COUNT; i++) {
        f32 iy = iy0 + i*dy;
        f32 tw = mtext_w(m, false, MAIN_ITEMS[i], cap);
        /* mouse hover selects */
        if (in->mouse_x >= ix && in->mouse_x <= ix+tw &&
            in->mouse_y >= iy && in->mouse_y <= iy+cap) m->sel = i;
        bool hot = (m->sel == i);
        mtext(m, r, false, MAIN_ITEMS[i], ix, iy, cap,
              hot ? 1.0f : 0.75f, hot ? 0.9f : 0.55f, hot ? 0.3f : 0.2f);
    }

    if (input_key_pressed(in, SDL_SCANCODE_DOWN)) { m->sel = (m->sel+1)%MAIN_ITEM_COUNT; click(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_UP))   { m->sel = (m->sel+MAIN_ITEM_COUNT-1)%MAIN_ITEM_COUNT; click(m,au); }
    bool activate = input_key_pressed(in, SDL_SCANCODE_RETURN) ||
                    input_key_pressed(in, SDL_SCANCODE_SPACE) || clicked;
    if (activate) {
        click(m, au);
        switch (m->sel) {
        case 0: m->start_story = true; break;                /* STORY → campaign */
        case 1: m->screen = MENU_MP;      m->sel = 0; break; /* MULTIPLAYER */
        case 2: m->return_screen = MENU_MAIN;
                m->screen = MENU_OPTIONS; m->sel = 0; break; /* OPTIONS */
        default: m->want_quit = true; break;                 /* QUIT */
        }
    }
}

/* Multiplayer map list (over the parchment). */
static void menu_mp(Menu *m, InputState *in, Renderer *r, AudioSystem *au,
                    f32 W, f32 H, bool clicked) {
    draw_bg(r, m->tex_main, W, H);
    mtext(m, r, false, "MULTIPLAYER", W*0.06f, H*0.10f, H*0.09f, 0.85f, 0.70f, 0.30f);
    f32 cap = H * 0.055f;
    f32 ix = W*0.08f, iy0 = H*0.38f, dy = H*0.10f;
    for (int i = 0; i < MP_MAP_COUNT; i++) {
        f32 iy = iy0 + i*dy;
        f32 tw = mtext_w(m, false, MP_MAPS[i].label, cap);
        if (in->mouse_x >= ix && in->mouse_x <= ix+tw &&
            in->mouse_y >= iy && in->mouse_y <= iy+cap) m->sel = i;
        bool hot = (m->sel == i);
        mtext(m, r, false, MP_MAPS[i].label, ix, iy, cap,
              hot?1.0f:0.75f, hot?0.9f:0.55f, hot?0.3f:0.2f);
    }
    if (input_key_pressed(in, SDL_SCANCODE_DOWN)) { m->sel=(m->sel+1)%MP_MAP_COUNT; click(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_UP))   { m->sel=(m->sel+MP_MAP_COUNT-1)%MP_MAP_COUNT; click(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_ESCAPE)) { m->screen=MENU_MAIN; m->sel=1; click(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_RETURN) || clicked) {
        click(m, au);
        snprintf(m->start_level, sizeof(m->start_level), "%s", MP_MAPS[m->sel].level);
    }
}

static void menu_mission(Menu *m, InputState *in, Renderer *r, AudioSystem *au,
                         f32 W, f32 H, bool clicked) {
    draw_bg(r, m->tex_pick, W, H);
    {
        const char *t = "SELECT A POSTER TO CHOOSE YOUR MISSION";
        f32 cap = H * 0.035f;
        mtext(m, r, true, t, W*0.5f - mtext_w(m, true, t, cap)*0.5f, H*0.005f,
              cap, 1.0f, 0.9f, 0.5f);
    }

    int hover = -1;
    for (int i = 0; i < POSTER_COUNT; i++) {
        f32 x0 = POSTERS[i].x0*W, y0 = POSTERS[i].y0*H;
        f32 x1 = POSTERS[i].x1*W, y1 = POSTERS[i].y1*H;
        if (in->mouse_x >= x0 && in->mouse_x <= x1 &&
            in->mouse_y >= y0 && in->mouse_y <= y1) hover = i;
    }
    if (hover >= 0) m->sel = hover;

    /* Highlight the selected poster with a bright frame. */
    {
        f32 x0 = POSTERS[m->sel].x0*W, y0 = POSTERS[m->sel].y0*H;
        f32 x1 = POSTERS[m->sel].x1*W, y1 = POSTERS[m->sel].y1*H;
        f32 t = OL_MAX(2.0f, H/240.0f);
        renderer_draw_rect(r, x0, y0, x1-x0, t, 1,0.9f,0.3f,0.9f);
        renderer_draw_rect(r, x0, y1-t, x1-x0, t, 1,0.9f,0.3f,0.9f);
        renderer_draw_rect(r, x0, y0, t, y1-y0, 1,0.9f,0.3f,0.9f);
        renderer_draw_rect(r, x1-t, y0, t, y1-y0, 1,0.9f,0.3f,0.9f);
    }

    if (input_key_pressed(in, SDL_SCANCODE_RIGHT)) { m->sel = (m->sel+1)%POSTER_COUNT; click(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_LEFT))  { m->sel = (m->sel+POSTER_COUNT-1)%POSTER_COUNT; click(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_DOWN))  { m->sel = (m->sel+3)%POSTER_COUNT; click(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_UP))    { m->sel = (m->sel+3)%POSTER_COUNT; click(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_ESCAPE)) { m->screen = MENU_MAIN; m->sel = 0; click(m,au); }

    if (input_key_pressed(in, SDL_SCANCODE_RETURN) || clicked) {
        click(m, au);
        snprintf(m->start_level, sizeof(m->start_level), "%s", POSTERS[m->sel].level);
    }
}

static void menu_options(Menu *m, InputState *in, Renderer *r, AudioSystem *au,
                         f32 W, f32 H, bool clicked) {
    draw_bg(r, m->tex_opts, W, H);
    {
        f32 cap = H*0.08f;
        mtext(m, r, false, "OPTIONS", W*0.5f - mtext_w(m,false,"OPTIONS",cap)*0.5f,
              H*0.08f, cap, 0.9f,0.8f,0.3f);
    }

    char buf[64];
    f32 cap = H*0.05f;
    const char *labels[3] = { "MUSIC VOLUME", "SFX VOLUME", "BACK" };
    for (int i = 0; i < 3; i++) {
        f32 iy = H*0.35f + i*H*0.12f, ix = W*0.30f;
        bool hot = (m->sel == i);
        if (i < 2) {
            int v = (i==0) ? m->music_vol : m->sfx_vol;
            char bar[16]; int k=0; for (; k<10; k++) bar[k] = (k<v)?'#':'-'; bar[k]='\0';
            snprintf(buf, sizeof(buf), "%s  [%s]", labels[i], bar);
        } else snprintf(buf, sizeof(buf), "%s", labels[i]);
        mtext(m, r, false, buf, ix, iy, cap, hot?1.0f:0.7f, hot?0.9f:0.55f, hot?0.3f:0.2f);
    }

    if (input_key_pressed(in, SDL_SCANCODE_DOWN)) { m->sel=(m->sel+1)%3; click(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_UP))   { m->sel=(m->sel+2)%3; click(m,au); }
    int *vol = (m->sel==0)?&m->music_vol : (m->sel==1)?&m->sfx_vol : NULL;
    if (vol) {
        if (input_key_pressed(in, SDL_SCANCODE_RIGHT) && *vol<10) { (*vol)++; click(m,au); }
        if (input_key_pressed(in, SDL_SCANCODE_LEFT)  && *vol>0)  { (*vol)--; click(m,au); }
    }
    audio_set_music_volume(au, m->music_vol / 10.0f);
    audio_set_sfx_volume(au, m->sfx_vol / 10.0f);
    if (input_key_pressed(in, SDL_SCANCODE_ESCAPE) ||
        ((input_key_pressed(in, SDL_SCANCODE_RETURN) || clicked) && m->sel==2)) {
        m->screen = (m->return_screen == MENU_PAUSE) ? MENU_PAUSE : MENU_MAIN;
        m->sel = 0; click(m,au);
    }
}

/* ---- In-game pause menu + save/load slot lists ------------------------- */

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

/* Dim the frozen game frame behind the pause UI. */
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
        mtext(m, r, false, PAUSE_ITEMS[i], ix, iy, cap,
              hot?1.0f:0.75f, hot?0.9f:0.55f, hot?0.3f:0.2f);
    }
    if (input_key_pressed(in, SDL_SCANCODE_DOWN)) { m->sel=(m->sel+1)%PAUSE_ITEM_COUNT; click(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_UP))   { m->sel=(m->sel+PAUSE_ITEM_COUNT-1)%PAUSE_ITEM_COUNT; click(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_ESCAPE)) { m->want_resume = true; click(m,au); return; }
    if (input_key_pressed(in, SDL_SCANCODE_RETURN) || input_key_pressed(in, SDL_SCANCODE_SPACE) || clicked) {
        click(m, au);
        switch (m->sel) {
        case 0: m->want_resume = true; break;
        case 1: m->screen = MENU_SAVE; m->sel = 0; break;
        case 2: m->screen = MENU_LOAD; m->sel = 0; break;
        case 3: m->return_screen = MENU_PAUSE; m->screen = MENU_OPTIONS; m->sel = 0; break;
        default: m->want_quit_menu = true; break;
        }
    }
}

/* Shared save/load slot list. save=true → writing, false → loading. */
static void menu_slots(Menu *m, InputState *in, Renderer *r, AudioSystem *au,
                       f32 W, f32 H, bool clicked, bool save) {
    pause_dim(r, W, H);
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
    if (input_key_pressed(in, SDL_SCANCODE_DOWN)) { m->sel=(m->sel+1)%SAVE_SLOTS; click(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_UP))   { m->sel=(m->sel+SAVE_SLOTS-1)%SAVE_SLOTS; click(m,au); }
    if (input_key_pressed(in, SDL_SCANCODE_ESCAPE)) { m->screen = MENU_PAUSE; m->sel = 0; click(m,au); return; }
    if (input_key_pressed(in, SDL_SCANCODE_RETURN) || clicked) {
        if (save) { m->req_save_slot = m->sel; click(m,au); }
        else if (m->slot_used[m->sel]) { m->req_load_slot = m->sel; click(m,au); }
    }
}

void menu_frame(Menu *m, InputState *in, Renderer *r, AudioSystem *au) {
    f32 W = (f32)r->cfg.width, H = (f32)r->cfg.height;
    bool down = in->mouse_buttons[0];
    bool clicked = down && !m->prev_click;   /* left-click edge */
    m->prev_click = down;

    switch (m->screen) {
    case MENU_MAIN:    menu_main(m, in, r, au, W, H, clicked);    break;
    case MENU_MISSION: menu_mission(m, in, r, au, W, H, clicked); break;
    case MENU_MP:      menu_mp(m, in, r, au, W, H, clicked);      break;
    case MENU_OPTIONS: menu_options(m, in, r, au, W, H, clicked); break;
    case MENU_PAUSE:   menu_pause(m, in, r, au, W, H, clicked);   break;
    case MENU_SAVE:    menu_slots(m, in, r, au, W, H, clicked, true);  break;
    case MENU_LOAD:    menu_slots(m, in, r, au, W, H, clicked, false); break;
    default: break;
    }
}
