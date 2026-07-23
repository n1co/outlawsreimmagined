/*
 * renderer.h - OpenGL renderer for the Outlaws engine
 *
 * Handles:
 *   - OpenGL context setup (via SDL2)
 *   - Shader compilation
 *   - Texture management (upload PCX images)
 *   - Level geometry building (sectors → GL meshes)
 *   - Billboard sprite rendering
 *   - HUD (2D orthographic overlay)
 *   - Frame rendering
 */
#pragma once

#include "engine.h"
#include "lvt.h"
#include "entity.h"
#include "weapon.h"
#include "inf.h"
#include "postfx.h"
#include <GL/glew.h>
#include <SDL2/SDL.h>

/* Maximum textures cached in GPU — large levels (TOWN) need >512 */
/* Large levels (e.g. TOWN) plus 8-directional enemy animation frames and morph
 * doors need many textures; enemy costumes alone can add >1000. */
#define R_MAX_TEXTURES 16384

/* Animated texture support (ATX files with multiple PCX frames) */
#define R_MAX_ANIM_TEXTURES 64
#define R_MAX_ANIM_FRAMES   32

typedef struct {
    u32    base_tex;                    /* Texture slot used by level meshes */
    GLuint frame_handles[R_MAX_ANIM_FRAMES]; /* GL texture handles per frame */
    u32    frame_count;
    f32    fps;
    f32    timer;
    u32    current_frame;
    /* ATX control flow: an animated texture whose passive program hits a STOP
     * (before any GOTO) is STATIC and holds frame 0 — e.g. an intact glass
     * window that only animates the break sequence when shot. A program that
     * loops via GOTO cycles frames [loop_start..frame_count-1]. */
    bool   loop;
    u32    loop_start;
} AnimTexture;

/* Renderer configuration */
typedef struct {
    int   width, height;   /* Window/framebuffer size */
    float fov;             /* Field of view (degrees) */
    float near_plane;
    float far_plane;
} RenderConfig;

/* A texture handle on the GPU */
typedef struct {
    GLuint handle;
    u32    width, height;
    char   name[64];
} GpuTexture;

/* A renderable mesh (VAO + VBO) */
typedef struct {
    GLuint vao, vbo, ibo;
    u32    index_count;
    u32    tex_id;          /* Index into renderer's texture table */
    u32    sector_idx;      /* Source sector (0xFFFFFFFF = batched/no sector) */
    bool   is_scroll_floor; /* True: UV offset from INF scroll system applies */
    bool   is_sign;         /* Wall sign overlay: drawn with polygon offset */
} RenderMesh;

/* Per-vertex data for world geometry */
typedef struct {
    f32 x, y, z;   /* World position */
    f32 u, v;       /* Texture coordinates */
    f32 light;      /* Ambient light factor */
} WorldVertex;

/* HUD draw parameters */
typedef struct {
    i32  health, max_health;
    i32  ammo;              /* Loaded rounds (clip) */
    i32  reserve;           /* Reserve ammo */
    i32  clip_size;         /* Max clip capacity (0 = no clip/melee) */
    i32  weapon_idx;        /* For weapon name display */
    bool show_crosshair;
    bool dead;
    bool firing;            /* True during weapon fire animation */
    bool fire_alt;          /* True if this was an alternate fire */
    f32  fire_timer;        /* Normalized fire animation progress [0..1] */
    bool cooking;           /* Thrown-weapon wind-up held (arm back pose):
                             * shows the throw chor's first frame while the
                             * button is held (chor 0xFFF9 hold semantics) */
    bool cooking_alt;       /* Wind-up pose comes from FIRE_CHOR_2 (dynamite)
                             * instead of FIRE_CHOR_1 (knife) */
    bool holding_lit;       /* Lit dynamite in hand: hold the light chor's
                             * last frame (burning stick) */
    bool reloading;         /* Currently reloading */
    const char *message;    /* On-screen message (NULL/empty = none) */
    const char *inventory;  /* Held inventory items, one per line (NULL = none) */
} HudParams;

/* Renderer state */
typedef struct {
    SDL_Window   *window;
    SDL_GLContext gl_ctx;

    RenderConfig  cfg;

    /* Shaders */
    GLuint prog_world;   /* World geometry shader */
    GLuint prog_sprite;  /* Billboard sprite shader */
    GLuint prog_hud;     /* 2D HUD shader */
    GLuint prog_sky;     /* Parallax sky shader (Jedi cylindrical projection) */
    GLuint sky_vao, sky_vbo; /* Fullscreen quad for the sky pass */

    /* Camera matrices (updated each frame) */
    Mat4 view;
    Mat4 proj;

    /* Camera right/up vectors (extracted from view) for billboards */
    Vec3 cam_right;
    Vec3 cam_up;

    /* Texture cache */
    GpuTexture textures[R_MAX_TEXTURES];
    u32        texture_count;

    /* Level texture-index → GL texture id map, computed once per level-mesh build.
     * Avoids an O(walls × GL-textures) name lookup during rebuilds (was ~36ms on
     * civlwar1, tanking FPS whenever an elevator moved). */
    u32        texmap[LVT_MAX_TEXTURES];

    /* Level geometry */
    RenderMesh *level_meshes;
    u32         level_mesh_count;

    /* Sprite dynamic VBO */
    GLuint sprite_vao, sprite_vbo;

    /* HUD dynamic VBO */
    GLuint hud_vao, hud_vbo;

    /* "Missing texture" fallback (checkerboard) */
    GLuint missing_tex;

    /* First-person weapon sprites — multi-frame animation per choreography state.
     * weapon_hud_tex[w] = idle/rest frame
     * weapon_fire_frames[w][f] = primary fire frame textures (FIRE_CHOR_1)
     * weapon_fire2_frames[w][f] = secondary fire frame textures (FIRE_CHOR_2)
     * weapon_reload_frames[w][f] = reload animation frame textures */
    #define WEAPON_MAX_ANIM_FRAMES 16
    u32 weapon_hud_tex[WEAPON_COUNT];
    u32 weapon_fire_frames[WEAPON_COUNT][WEAPON_MAX_ANIM_FRAMES];
    u32 weapon_fire_frame_count[WEAPON_COUNT];
    u32 weapon_fire_dt[WEAPON_COUNT][WEAPON_MAX_ANIM_FRAMES]; /* ms per frame */
    u32 weapon_fire2_frames[WEAPON_COUNT][WEAPON_MAX_ANIM_FRAMES];
    u32 weapon_fire2_frame_count[WEAPON_COUNT];
    u32 weapon_fire2_dt[WEAPON_COUNT][WEAPON_MAX_ANIM_FRAMES];
    u32 weapon_reload_frames[WEAPON_COUNT][WEAPON_MAX_ANIM_FRAMES];
    u32 weapon_reload_frame_count[WEAPON_COUNT];
    u32 weapon_reload_dt[WEAPON_COUNT][WEAPON_MAX_ANIM_FRAMES];

    /* Per-frame anchor geometry (FRMT off_x/off_y and cell pixel size) so the
     * first-person weapon draws all animation frames in one common coordinate
     * space (native 640x480) — the hand stays fixed while the gun recoils.
     * Indexed [weapon][0]=idle, then fire/fire2/reload share the same layout as
     * the *_frames[] arrays above. */
    i32 weapon_idle_ox[WEAPON_COUNT], weapon_idle_oy[WEAPON_COUNT];
    u32 weapon_idle_w[WEAPON_COUNT],  weapon_idle_h[WEAPON_COUNT];
    i32 weapon_fire_ox[WEAPON_COUNT][WEAPON_MAX_ANIM_FRAMES];
    i32 weapon_fire_oy[WEAPON_COUNT][WEAPON_MAX_ANIM_FRAMES];
    u32 weapon_fire_w[WEAPON_COUNT][WEAPON_MAX_ANIM_FRAMES];
    u32 weapon_fire_h[WEAPON_COUNT][WEAPON_MAX_ANIM_FRAMES];
    i32 weapon_fire2_ox[WEAPON_COUNT][WEAPON_MAX_ANIM_FRAMES];
    i32 weapon_fire2_oy[WEAPON_COUNT][WEAPON_MAX_ANIM_FRAMES];
    u32 weapon_fire2_w[WEAPON_COUNT][WEAPON_MAX_ANIM_FRAMES];
    u32 weapon_fire2_h[WEAPON_COUNT][WEAPON_MAX_ANIM_FRAMES];
    i32 weapon_reload_ox[WEAPON_COUNT][WEAPON_MAX_ANIM_FRAMES];
    i32 weapon_reload_oy[WEAPON_COUNT][WEAPON_MAX_ANIM_FRAMES];
    u32 weapon_reload_w[WEAPON_COUNT][WEAPON_MAX_ANIM_FRAMES];
    u32 weapon_reload_h[WEAPON_COUNT][WEAPON_MAX_ANIM_FRAMES];
    /* Per-frame translucency: muzzle-flash frames (CELT flag 0x20) are drawn
     * with additive blending in the original engine. */
    u8  weapon_fire_trans[WEAPON_COUNT][WEAPON_MAX_ANIM_FRAMES];
    u8  weapon_fire2_trans[WEAPON_COUNT][WEAPON_MAX_ANIM_FRAMES];

    /* Player face portrait (from ATIM.NWX, changes with health) */
    u32 face_hud_tex;

    /* HUD number digit textures from NUMBERS.NWX (cells 0-9 = digits '0'-'9') */
    u32 digit_tex[10];

    /* HUD panel background (INTERFAC.NWX cell 0, 640x43) */
    u32 hud_panel_tex;

    /* Heart icon (INTHEART.NWX cell 0, 34x64) */
    u32 hud_heart_tex;

    /* Energy indicator cells (INTNERGY.NWX, 22 cells = health states empty→full) */
    u32 hud_energy_cells[32];
    u32 hud_energy_cell_count;

    /* Per-weapon ammo cartridge sprite (seq 11 of weapon NWX, Ghidra RE) */
    u32 weapon_ammo_tex[WEAPON_COUNT];

    /* Animated textures (ATX) */
    AnimTexture anim_textures[R_MAX_ANIM_TEXTURES];
    u32         anim_texture_count;

    /* Window break sequences: a shot window plays its ATX break frames
     * (GWWNx42..x47) then holds the last (shattered) frame. Keyed by the ATX
     * base texture id (frame 0 = intact). */
    struct {
        u32 base_tex;
        u32 frame_tex[8];      /* [0]=intact, [1..count-1]=break frames */
        u32 frame_count;
        f32 fps;
    } window_breaks[16];
    u32 window_break_count;

    /* Per-sector UV scroll offsets (updated by renderer_sync_scroll from InfSystem) */
    f32  sector_scroll_u[4096];
    f32  sector_scroll_v[4096];

    /* Sky panorama texture (0 = none, use clear color) */
    u32 sky_tex;              /* Texture ID of sky panorama PCX */
    f32 sky_parallax_x;      /* Sky texels per full turn, horizontal (LVT PARALLAX) */
    f32 sky_parallax_y;      /* Sky texels per full turn, vertical */

    /* Camera yaw/pitch cached from renderer_set_camera for sky rendering */
    f32 cam_yaw;
    f32 cam_pitch;
    f32 cam_fov_rad;
    f32 view_zoom;    /* Scope zoom factor (0/1 = none) */

    /* Uploaded 3DO models (CPU-side colored tri lists, sprite-shader layout) */
    #define R_MAX_TDO 8
    f32 *tdo_data[R_MAX_TDO];     /* 9 floats/vert: pos3 uv2 rgba4 */
    u32  tdo_vcount[R_MAX_TDO];
    u32  tdo_count;

    /* -------- Post-processing (optional pretty shaders) -------- *
     * When post.enabled, begin_frame binds post_fbo; the scene + HUD render into
     * it; renderer_post_resolve() draws a fullscreen quad through prog_post to the
     * backbuffer. Debug UI / pause menu draw AFTER the resolve so they stay crisp. */
    PostFX post;
    GLuint post_fbo;             /* offscreen framebuffer */
    GLuint post_color;           /* color attachment (sampled by the post shader) */
    GLuint post_depth;           /* depth renderbuffer */
    GLuint prog_post;            /* fullscreen post-process program */
    GLuint post_vao, post_vbo;   /* fullscreen quad */
    int    post_w, post_h;       /* current size of the attachments */
    bool   post_resolved;        /* resolve already done this frame? */
} Renderer;

/* Initialize renderer, create window and GL context. Returns false on error. */
bool renderer_init(Renderer *r, const RenderConfig *cfg, const char *title);

/* Shutdown renderer, destroy GL context and window. */
void renderer_shutdown(Renderer *r);

/* Begin a new frame (clear buffers). */
void renderer_begin_frame(Renderer *r);

/* End frame (swap buffers). Resolves the post-process pass first if it hasn't
 * been resolved explicitly this frame. */
void renderer_end_frame(Renderer *r);

/* Resolve the offscreen scene to the backbuffer through the post-process shader
 * (no-op if post-FX is disabled or already resolved this frame). Call this right
 * before drawing UI that must stay crisp (debug overlay, pause menu). */
void renderer_post_resolve(Renderer *r);

/* Upload a texture to GPU. Returns texture index+1 (or 0 on error). */
u32 renderer_upload_texture(Renderer *r, const char *name,
                            const u8 *rgba, u32 w, u32 h);

/* Create or update a dynamic RGBA video texture (cutscene frames). *slot is
 * the texture id: pass 0 on the first call (a new LINEAR, non-mipmapped slot
 * is created and stored back into *slot); reuse it to stream later frames via
 * glTexSubImage2D. */
void renderer_upload_video(Renderer *r, u32 *slot, const u8 *rgba, int w, int h);

/* Find a texture by name (lowercase). Returns 0 if not found. */
u32 renderer_find_texture(const Renderer *r, const char *name);

/* Build GPU meshes for a level. Textures must be uploaded first.
 * inf: optional INF system for detecting scroll-floor sectors (may be NULL). */
bool renderer_build_level(Renderer *r, const LvtLevel *level, const InfSystem *inf);

/*
 * Sync per-sector UV scroll offsets from INF system into the renderer.
 * Call each frame after inf_update() to animate scroll-floor meshes.
 */
void renderer_sync_scroll(Renderer *r, const InfSystem *inf);

/* Simple world-space billboard (projectiles, FX). pos = base (bottom
 * center) in LVT coordinates. */
typedef struct {
    Vec3 pos;
    u32  tex;     /* renderer texture id (1-based) */
    f32  w, h;    /* world-unit size */
} BillboardDraw;

/* Draw a batch of camera-facing billboards with the sprite shader. */
void renderer_draw_billboards(Renderer *r, const BillboardDraw *list, u32 count);

/* View zoom (rifle scope): narrows the horizontal FOV by `zoom` (1 = none). */
void renderer_set_zoom(Renderer *r, f32 zoom);

/* Flat-shaded 3DO model instances (thrown knife/dynamite, ground objects —
 * the original renders these as untextured palette-colored models). */
typedef struct {
    Vec3 pos;      /* LVT world position (model origin) */
    f32  yaw;      /* Heading (radians) */
    f32  tumble;   /* End-over-end tumble angle (radians) */
    int  id;       /* From renderer_upload_tdo */
} TdoDraw;

struct TdoModel;
/* Build a CPU-side colored triangle list from a parsed 3DO. Returns id ≥ 0
 * or -1. pal = 256-color palette for the per-triangle color indices. */
int renderer_upload_tdo(Renderer *r, const struct TdoModel *m,
                        const u8 pal[256][3]);
void renderer_draw_tdos(Renderer *r, const TdoDraw *list, u32 count);

/* Render the level. */
void renderer_draw_level(Renderer *r);

/* Set camera view matrix from position + yaw/pitch. */
void renderer_set_camera(Renderer *r, Vec3 pos, f32 yaw, f32 pitch);

/*
 * Draw all active entities as billboard sprites.
 * Entities with sprite_tex == 0 draw as a colored placeholder quad.
 */
void renderer_draw_sprites(Renderer *r, const EntityList *entities);

/* Draw the HUD overlay (2D orthographic, disables depth test). */
void renderer_draw_hud(Renderer *r, const HudParams *hud);

/* Draw the level loading screen (MM220 background + green progress bar +
 * level name). progress in [0,1]. Caller swaps buffers. */
void renderer_draw_loading(Renderer *r, u32 bg_tex, float progress,
                           const char *label);

/* Automap overlay (TAB): player-oriented top-down wall map over the 3D view. */
void renderer_draw_minimap(Renderer *r, const LvtLevel *level,
                           const InfSystem *inf, Vec3 ppos, f32 pyaw);

/* --- 2D menu drawing helpers (screen-space, pixel coords) --- */
void renderer_draw_image(Renderer *r, u32 tex, f32 x, f32 y, f32 w, f32 h,
                         f32 tint, f32 alpha);
/* Textured quad with explicit UV sub-rect + colour tint (for font atlases). */
void renderer_draw_image_uv(Renderer *r, u32 tex, f32 x, f32 y, f32 w, f32 h,
                            f32 u0, f32 v0, f32 u1, f32 v1,
                            f32 cr, f32 cg, f32 cb, f32 ca);
void renderer_draw_rect(Renderer *r, f32 x, f32 y, f32 w, f32 h,
                        f32 cr, f32 cg, f32 cb, f32 ca);
void renderer_draw_text(Renderer *r, const char *s, f32 x, f32 y, f32 px,
                        f32 cr, f32 cg, f32 cb);

/* Handle window resize. */
void renderer_resize(Renderer *r, int width, int height);

/*
 * Register an animated texture.
 * base_tex: texture slot ID that level meshes reference (the ATX name texture).
 * frames: array of GL texture handles for each frame (frame 0 = initial).
 * count: number of frames.
 * fps: animation rate in frames per second.
 */
void renderer_add_anim_texture(Renderer *r, u32 base_tex,
                               const GLuint *frame_handles, u32 count, f32 fps,
                               bool loop, u32 loop_start);

/* Register a window's break sequence: frame_tex[0] = intact, [1..count-1] = the
 * shatter frames. base_tex is the ATX base texture id the wall references. */
void renderer_register_window_break(Renderer *r, u32 base_tex,
                                     const u32 *frame_tex, u32 count, f32 fps);

/* Texture id to render for a window wall given its break state: the intact frame
 * (break_time < 0 or not broken), the current shatter frame, or the held final
 * frame. Returns base_tex unchanged if it isn't a registered window. */
u32 renderer_window_frame_tex(const Renderer *r, u32 base_tex,
                              bool broken, f32 break_time);

/* Advance animated texture timers and swap current frame handles (call each frame). */
void renderer_update_anim_textures(Renderer *r, f32 dt);

/*
 * Set sky panorama texture and parallax parameters.
 * sky_tex: texture ID from renderer_upload_texture (0 = disable sky).
 * parallax_x: horizontal panorama extent in world units (from LVT PARALLAX).
 */
void renderer_set_sky(Renderer *r, u32 sky_tex, f32 parallax_x, f32 parallax_y);

/* Draw sky panorama background (call before renderer_draw_level). */
void renderer_draw_sky(Renderer *r);
