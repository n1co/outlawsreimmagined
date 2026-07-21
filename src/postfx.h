/*
 * postfx.h - Post-processing effect parameters (shared C / C++ header)
 *
 * The renderer renders the 3D scene + HUD into an offscreen framebuffer, then
 * resolves it to the backbuffer through a single fullscreen post shader that
 * applies any enabled effects (CRT, bloom, chromatic aberration, vignette,
 * film grain, colour grading). These are OPTIONAL eye-candy — the game is
 * pixel-faithful with post-FX off (the default). The debug menu (INSERT) edits
 * this struct live; main.c copies it into the renderer each frame.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POST_PRESET_OFF = 0,
    POST_PRESET_CRT,        /* arcade CRT: curvature, scanlines, aperture, vignette */
    POST_PRESET_CINEMATIC,  /* soft bloom + vignette + grain + warm grade */
    POST_PRESET_VIBRANT,    /* punchy saturation/contrast + light bloom */
    POST_PRESET_CUSTOM,     /* user tweaked the sliders */
    POST_PRESET_COUNT,
} PostPreset;

typedef struct {
    bool  enabled;          /* master switch (post pass is skipped when false) */
    int   preset;           /* PostPreset (for the debug menu combo) */

    /* Effect toggles */
    bool  crt;
    bool  bloom;
    bool  chromatic;
    bool  vignette;
    bool  grain;
    bool  grade;            /* saturation / contrast / gamma colour grade */

    /* Effect parameters */
    float curvature;        /* CRT barrel curvature       0..0.3   */
    float scanline;         /* scanline darkening         0..1     */
    float mask;             /* aperture-grille strength   0..1     */
    float bloom_amt;        /* bloom add strength         0..2     */
    float bloom_thresh;     /* bloom luminance threshold  0..1     */
    float chroma_amt;       /* chromatic aberration (px)  0..6     */
    float vignette_amt;     /* corner darkening           0..1.5   */
    float grain_amt;        /* film grain intensity       0..0.25  */
    float saturation;       /* 1 = neutral                0..2     */
    float contrast;         /* 1 = neutral                0.5..2   */
    float gamma;            /* 1 = neutral                0.5..2.5 */
} PostFX;

/* Fill `p` (keeping master `enabled`) with the named preset's parameters. */
static inline void postfx_apply_preset(PostFX *p, int preset) {
    bool was_enabled = p->enabled;
    /* neutral baseline */
    p->crt = p->bloom = p->chromatic = p->vignette = p->grain = p->grade = false;
    p->curvature = 0.12f; p->scanline = 0.35f; p->mask = 0.25f;
    p->bloom_amt = 0.6f;  p->bloom_thresh = 0.65f;
    p->chroma_amt = 1.5f; p->vignette_amt = 0.5f; p->grain_amt = 0.05f;
    p->saturation = 1.0f; p->contrast = 1.0f; p->gamma = 1.0f;
    p->preset = preset;

    switch (preset) {
    case POST_PRESET_CRT:
        p->crt = true; p->vignette = true; p->grain = true;
        p->scanline = 0.4f; p->mask = 0.3f; p->curvature = 0.14f;
        p->vignette_amt = 0.55f; p->grain_amt = 0.04f;
        break;
    case POST_PRESET_CINEMATIC:
        p->bloom = true; p->vignette = true; p->grain = true; p->grade = true;
        p->bloom_amt = 0.75f; p->bloom_thresh = 0.6f;
        p->vignette_amt = 0.6f; p->grain_amt = 0.06f;
        p->saturation = 0.92f; p->contrast = 1.08f; p->gamma = 1.05f;
        break;
    case POST_PRESET_VIBRANT:
        p->bloom = true; p->grade = true;
        p->bloom_amt = 0.4f; p->bloom_thresh = 0.7f;
        p->saturation = 1.35f; p->contrast = 1.12f; p->gamma = 0.98f;
        break;
    case POST_PRESET_OFF:
    default:
        p->enabled = false;
        return;
    }
    (void)was_enabled;
    p->enabled = true;   /* selecting a real preset turns post-processing on */
}

#ifdef __cplusplus
}
#endif
