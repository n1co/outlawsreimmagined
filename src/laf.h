/*
 * laf.h - Outlaws ".LAF" bitmap font (LucasArts "TNFN" resource)
 *
 * The Outlaws front end and HUD draw text with LAF fonts (mf3s.laf, SF3.LAF,
 * text.laf ...). Format reverse-engineered from olwin.exe (text.c: the glyph
 * blitter FUN_004961b0). Raw file layout, little-endian, base = file start:
 *   0x00: u32[8] header. [2]@0x08 = line height, [3]@0x0c = ascent,
 *         [6]@0x18 = first_char, [7]@0x1c = last_char.
 *   0x20: char table, (last-first+1) x u16.  glyph_index = ct[char - first].
 *   recbase = 0x20 + (last-first+1)*2 : records, 16 bytes each:
 *         {off u32 (into pixel blob), metrics u32 (adv=byte0, ybear=byte2),
 *          width u32 @0x08, height u32 @0x0c}.
 *   pixblob = recbase + (max(ct)+1)*16 : per glyph width*height bytes,
 *         row-major, 0 = transparent, non-zero = opaque (colour index).
 *
 * We decode every glyph into one RGBA atlas (white where opaque, transparent
 * elsewhere) and draw it tinted, matching the original's colour-remapped blit.
 */
#pragma once

#include "engine.h"
#include "renderer.h"

struct Archives;

typedef struct {
    short ax, ay;              /* position in the atlas */
    short w, h;                /* glyph size (0 = no pixels, e.g. space) */
    i8    advance, xbear, ybear;
} LafGlyph;

typedef struct {
    int      height, ascent;
    int      first, last;
    LafGlyph glyph[256];       /* indexed by char code */
    u32      atlas_tex;        /* renderer texture id (0 = failed) */
    int      atlas_w, atlas_h;
    bool     loaded;
} LafFont;

/* Load + decode a LAF from the archives and upload its atlas. */
bool laf_load(LafFont *f, struct Archives *arc, Renderer *r,
              const char *file, const char *tex_name);

/* Advance width of a string at the given scale (pixels). */
float laf_text_width(const LafFont *f, const char *s, float scale);

/*
 * Draw a string. (x,y) is the top-left of the text box. Renders a dark drop
 * shadow then the glyphs in (cr,cg,cb,ca). Returns the advance width drawn.
 */
float laf_draw(LafFont *f, Renderer *r, const char *s, float x, float y,
               float scale, float cr, float cg, float cb, float ca);
