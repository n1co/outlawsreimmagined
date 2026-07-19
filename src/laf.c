/*
 * laf.c - Outlaws LAF bitmap font decoder + atlas builder (see laf.h)
 */
#include "laf.h"
#include "world.h"     /* Archives, archives_get */
#include <string.h>
#include <stdlib.h>

static inline u32 rd_u32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1]<<8) | ((u32)p[2]<<16) | ((u32)p[3]<<24);
}
static inline u16 rd_u16(const u8 *p) { return (u16)(p[0] | (p[1]<<8)); }

#define LAF_ATLAS_W 1024

bool laf_load(LafFont *f, struct Archives *arc, Renderer *r,
              const char *file, const char *tex_name) {
    memset(f, 0, sizeof(*f));

    u32 sz = 0;
    const u8 *d = archives_get(arc, file, &sz);
    if (!d || sz < 0x20) return false;

    f->height = (int)rd_u32(d + 0x08);
    f->ascent = (int)rd_u32(d + 0x0c);
    f->first  = (int)rd_u32(d + 0x18);
    f->last   = (int)rd_u32(d + 0x1c);
    if (f->first < 0 || f->last > 255 || f->last < f->first) return false;

    int nchars = f->last - f->first + 1;
    u32 ct_base  = 0x20;
    u32 rec_base = ct_base + (u32)nchars * 2;
    if (rec_base > sz) return false;

    /* Glyph count = highest index in the char table + 1. */
    int nglyphs = 0;
    for (int i = 0; i < nchars; i++) {
        int gi = rd_u16(d + ct_base + i*2);
        if (gi + 1 > nglyphs) nglyphs = gi + 1;
    }
    u32 pix_base = rec_base + (u32)nglyphs * 16;
    if (pix_base > sz) return false;

    /* --- First pass: read records, shelf-pack the atlas. --- */
    int ax = 0, ay = 0, row_h = 0, atlas_w = 0;
    for (int c = f->first; c <= f->last; c++) {
        int gi = rd_u16(d + ct_base + (c - f->first)*2);
        const u8 *rec = d + rec_base + (u32)gi*16;
        u32 off = rd_u32(rec);
        int adv = (i8)rec[4], ybear = (i8)rec[6], xbear = (i8)rec[5];
        int w = (int)rd_u32(rec + 8), h = (int)rd_u32(rec + 12);
        if (w < 0 || h < 0 || w > 256 || h > 256) { w = 0; h = 0; }
        if (pix_base + off + (u32)w*h > sz) { w = 0; h = 0; }

        LafGlyph *g = &f->glyph[c];
        g->advance = (i8)adv;
        g->xbear   = (i8)xbear;
        g->ybear   = (i8)ybear;
        g->w = (short)w; g->h = (short)h;
        if (w > 0 && h > 0) {
            if (ax + w + 1 > LAF_ATLAS_W) { ax = 0; ay += row_h + 1; row_h = 0; }
            g->ax = (short)ax; g->ay = (short)ay;
            ax += w + 1;
            if (w + 1 > atlas_w) atlas_w = ax;   /* track used width */
            if (atlas_w < ax) atlas_w = ax;
            if (h > row_h) row_h = h;
        }
    }
    atlas_w = LAF_ATLAS_W;
    int atlas_h = ay + row_h + 1;
    if (atlas_h < 1) return false;

    /* --- Second pass: blit glyph pixels into the RGBA atlas. --- */
    u8 *rgba = (u8 *)calloc((size_t)atlas_w * atlas_h, 4);
    if (!rgba) return false;
    for (int c = f->first; c <= f->last; c++) {
        LafGlyph *g = &f->glyph[c];
        if (g->w <= 0 || g->h <= 0) continue;
        int gi = rd_u16(d + ct_base + (c - f->first)*2);
        u32 off = rd_u32(d + rec_base + (u32)gi*16);
        const u8 *px = d + pix_base + off;
        for (int y = 0; y < g->h; y++) {
            for (int x = 0; x < g->w; x++) {
                if (!px[y*g->w + x]) continue;
                u8 *o = rgba + (((size_t)(g->ay + y) * atlas_w) + (g->ax + x)) * 4;
                o[0] = 255; o[1] = 255; o[2] = 255; o[3] = 255;
            }
        }
    }

    f->atlas_w = atlas_w; f->atlas_h = atlas_h;
    f->atlas_tex = renderer_upload_texture(r, tex_name, rgba, (u32)atlas_w, (u32)atlas_h);
    free(rgba);
    f->loaded = (f->atlas_tex != 0);
    return f->loaded;
}

float laf_text_width(const LafFont *f, const char *s, float scale) {
    if (!f || !f->loaded || !s) return 0.0f;
    float w = 0.0f;
    for (int i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        int adv = f->glyph[c].advance;
        if (adv == 0 && f->glyph[c].w == 0) adv = f->height / 3;   /* fallback space */
        w += adv * scale;
    }
    return w;
}

float laf_draw(LafFont *f, Renderer *r, const char *s, float x, float y,
               float scale, float cr, float cg, float cb, float ca) {
    if (!f || !f->loaded || !s) return 0.0f;
    float baseline = y + f->ascent * scale;
    float aw = (float)f->atlas_w, ah = (float)f->atlas_h;

    for (int pass = 0; pass < 2; pass++) {
        float ox = (pass == 0) ? scale * 1.5f : 0.0f;   /* shadow offset */
        float oy = (pass == 0) ? scale * 1.5f : 0.0f;
        float R = (pass == 0) ? 0.0f : cr;
        float G = (pass == 0) ? 0.0f : cg;
        float B = (pass == 0) ? 0.0f : cb;
        float penx = x;
        for (int i = 0; s[i]; i++) {
            unsigned char c = (unsigned char)s[i];
            const LafGlyph *g = &f->glyph[c];
            if (g->w > 0 && g->h > 0) {
                float dx = penx + g->xbear * scale + ox;
                float dy = baseline + g->ybear * scale + oy;
                float u0 = g->ax / aw,          v0 = g->ay / ah;
                float u1 = (g->ax + g->w) / aw, v1 = (g->ay + g->h) / ah;
                renderer_draw_image_uv(r, f->atlas_tex, dx, dy,
                                       g->w * scale, g->h * scale,
                                       u0, v0, u1, v1, R, G, B, ca);
            }
            int adv = g->advance;
            if (adv == 0 && g->w == 0) adv = f->height / 3;
            penx += adv * scale;
        }
    }
    return laf_text_width(f, s, scale);
}
