/*
 * pcx.h - ZSoft PCX image decoder
 *
 * Outlaws uses 8-bit (256 color) palettized PCX images for all textures,
 * sprites, and UI elements. The palette is stored at the end of the file
 * preceded by a 0x0C marker.
 */
#pragma once

#include "engine.h"

typedef struct {
    u8  *pixels;    /* RGBA8 pixel data (width * height * 4 bytes) */
    u32  width;
    u32  height;
    u8   palette[256][3];  /* RGB palette entries */
} PcxImage;

/*
 * Decode a PCX file from memory.
 * Returns true on success; pixels must be freed with pcx_free().
 */
bool pcx_decode(PcxImage *img, const u8 *data, u32 size);

/* Free pixel data. */
void pcx_free(PcxImage *img);

/*
 * Decode and return a flat RGBA pixel buffer (convenience wrapper).
 * Caller must free() the returned pointer.
 */
u8 *pcx_decode_rgba(const u8 *data, u32 size, u32 *out_w, u32 *out_h);

/*
 * Decode PCX using an external 256-color palette (overrides the embedded one).
 * ext_pal: pointer to 256 × 3 bytes of RGB data (e.g. arc->palette).
 * Caller must free() the returned pointer.
 */
/* Decode a glass-window PCX: low palette indices (1-15) become translucent
 * glass, index 0 transparent, indices 16+ opaque (frame). See pcx.c. */
u8 *pcx_decode_rgba_glass(const u8 *data, u32 size, u32 *out_w, u32 *out_h,
                          const u8 ext_pal[256][3]);

u8 *pcx_decode_rgba_pal(const u8 *data, u32 size, u32 *out_w, u32 *out_h,
                         const u8 ext_pal[256][3]);
