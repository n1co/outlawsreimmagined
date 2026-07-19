/*
 * pcx.c - ZSoft PCX image decoder
 *
 * PCX format (version 5, 8-bit):
 *   Header (128 bytes): manufacturer, version, encoding, bpp, dimensions...
 *   Pixel data: RLE encoded, byte >= 0xC0 means (byte & 0x3F) repetitions
 *   Palette marker: 0x0C at byte (file_size - 769)
 *   Palette: 768 bytes (256 * RGB)
 */
#include "pcx.h"

#pragma pack(push, 1)
typedef struct {
    u8  manufacturer;   /* Always 0x0A */
    u8  version;        /* PCX version */
    u8  encoding;       /* 1 = RLE */
    u8  bpp;            /* Bits per pixel per plane */
    u16 xmin, ymin;
    u16 xmax, ymax;
    u16 hdpi, vdpi;
    u8  palette[48];    /* EGA palette (unused for 256-color) */
    u8  reserved;
    u8  nplanes;        /* Color planes */
    u16 bytes_per_line;
    u16 palette_info;
    u16 h_screen_size;
    u16 v_screen_size;
    u8  filler[54];
} PcxHeader;
#pragma pack(pop)

bool pcx_decode(PcxImage *img, const u8 *data, u32 size) {
    if (size < 128 + 769) {
        OL_ERR("PCX too small: %u bytes\n", size);
        return false;
    }

    const PcxHeader *hdr = (const PcxHeader *)data;
    if (hdr->manufacturer != 0x0A) {
        OL_ERR("Bad PCX magic: 0x%02X\n", hdr->manufacturer);
        return false;
    }
    if (hdr->bpp != 8 || hdr->nplanes != 1) {
        OL_ERR("Unsupported PCX format: %d bpp, %d planes\n", hdr->bpp, hdr->nplanes);
        return false;
    }

    u32 width  = (u32)(hdr->xmax - hdr->xmin + 1);
    u32 height = (u32)(hdr->ymax - hdr->ymin + 1);
    u32 stride = hdr->bytes_per_line;

    /* Read 256-color palette from end of file */
    if (data[size - 769] != 0x0C) {
        OL_WARN("PCX: no 256-color palette marker, image may be wrong colors\n");
    }
    const u8 *pal = data + size - 768;
    memcpy(img->palette, pal, 768);

    /* Decompress RLE pixel data */
    u8 *indexed = malloc(stride * height);
    if (!indexed) return false;

    const u8 *src = data + sizeof(PcxHeader);
    const u8 *src_end = data + size - 769;
    u8 *dst = indexed;
    u8 *dst_end = indexed + stride * height;

    while (dst < dst_end && src < src_end) {
        u8 byte = *src++;
        if ((byte & 0xC0) == 0xC0) {
            u32 count = byte & 0x3F;
            if (src >= src_end) break;
            u8 value = *src++;
            u32 n = (u32)(dst_end - dst);
            if (count > n) count = n;
            memset(dst, value, count);
            dst += count;
        } else {
            *dst++ = byte;
        }
    }

    /* Convert indexed to RGBA */
    img->width  = width;
    img->height = height;
    img->pixels = malloc(width * height * 4);
    if (!img->pixels) { free(indexed); return false; }

    for (u32 y = 0; y < height; y++) {
        for (u32 x = 0; x < width; x++) {
            u8 idx = indexed[y * stride + x];
            u8 *out = &img->pixels[(y * width + x) * 4];
            out[0] = pal[idx * 3 + 0];
            out[1] = pal[idx * 3 + 1];
            out[2] = pal[idx * 3 + 2];
            out[3] = (idx == 0) ? 0 : 255;
        }
    }

    free(indexed);
    return true;
}

void pcx_free(PcxImage *img) {
    if (img->pixels) { free(img->pixels); img->pixels = NULL; }
    img->width = img->height = 0;
}

u8 *pcx_decode_rgba(const u8 *data, u32 size, u32 *out_w, u32 *out_h) {
    PcxImage img = {0};
    if (!pcx_decode(&img, data, size)) return NULL;
    if (out_w) *out_w = img.width;
    if (out_h) *out_h = img.height;
    return img.pixels;  /* Caller must free */
}

u8 *pcx_decode_rgba_pal(const u8 *data, u32 size, u32 *out_w, u32 *out_h,
                         const u8 ext_pal[256][3]) {
    /* Decode indexed image first, then re-map with the external palette */
    PcxImage img = {0};
    if (!pcx_decode(&img, data, size)) return NULL;

    u32 w = img.width, h = img.height;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;

    /* Re-decode pixels using ext_pal instead of the embedded palette.
     * We need the raw index data, so decode again from scratch. */
    const PcxHeader *hdr = (const PcxHeader *)data;
    u32 stride = hdr->bytes_per_line;
    u8 *indexed = malloc(stride * h);
    if (!indexed) { pcx_free(&img); return img.pixels; }

    const u8 *src = data + sizeof(PcxHeader);
    const u8 *src_end = data + size - 769;
    u8 *dst = indexed, *dst_end = indexed + stride * h;
    while (dst < dst_end && src < src_end) {
        u8 byte = *src++;
        if ((byte & 0xC0) == 0xC0) {
            u32 count = byte & 0x3F;
            if (src >= src_end) break;
            u8 value = *src++;
            u32 n = (u32)(dst_end - dst);
            if (count > n) count = n;
            memset(dst, value, count);
            dst += count;
        } else {
            *dst++ = byte;
        }
    }

    /* Apply external palette.
     * Palette index 0 = transparent in the Jedi Engine (color key).
     * Used for window textures (99.5% index-0 = see-through glass). */
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) {
            u8 idx = indexed[y * stride + x];
            u8 *out = &img.pixels[(y * w + x) * 4];
            out[0] = ext_pal[idx][0];
            out[1] = ext_pal[idx][1];
            out[2] = ext_pal[idx][2];
            out[3] = (idx == 0) ? 0 : 255;
        }
    }
    free(indexed);
    return img.pixels;
}

u8 *pcx_decode_rgba_glass(const u8 *data, u32 size, u32 *out_w, u32 *out_h,
                          const u8 ext_pal[256][3]) {
    /* Like pcx_decode_rgba_pal, but the low palette indices (1-15) — used by
     * Outlaws glass window textures for the translucent pane/reflection — are
     * drawn as translucent light-blue glass instead of opaque EGA colors (the
     * original engine renders these via a translucency table, not the palette).
     *   index 0      -> fully transparent (see through)
     *   index 1-15   -> translucent glass tint (alpha scales with index)
     *   index 16+    -> opaque frame/mullions from the palette */
    u8 *rgba = pcx_decode_rgba_pal(data, size, out_w, out_h, ext_pal);
    if (!rgba) return NULL;
    u32 w = out_w ? *out_w : 0, h = out_h ? *out_h : 0;

    /* We need the raw indices again to classify glass vs frame. Re-decode. */
    const PcxHeader *hdr = (const PcxHeader *)data;
    u32 stride = hdr->bytes_per_line;
    u8 *indexed = malloc((size_t)stride * h);
    if (!indexed) return rgba;
    const u8 *src = data + sizeof(PcxHeader);
    const u8 *src_end = data + size - 769;
    u8 *dst = indexed, *dst_end = indexed + (size_t)stride * h;
    while (dst < dst_end && src < src_end) {
        u8 byte = *src++;
        if ((byte & 0xC0) == 0xC0) {
            u32 count = byte & 0x3F;
            if (src >= src_end) break;
            u8 value = *src++;
            u32 n = (u32)(dst_end - dst);
            if (count > n) count = n;
            memset(dst, value, count);
            dst += count;
        } else {
            *dst++ = byte;
        }
    }
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) {
            u8 idx = indexed[y * stride + x];
            if (idx == 0 || idx >= 16) continue; /* transparent / opaque frame */
            u8 *o = &rgba[(y * w + x) * 4];
            /* Light blue-white glass; brighter/more opaque toward higher index. */
            u8 a = (u8)(50 + idx * 6);   /* ~56..140 */
            o[0] = 200; o[1] = 220; o[2] = 245; o[3] = a;
        }
    }
    free(indexed);
    return rgba;
}
