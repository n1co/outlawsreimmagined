/*
 * wax.c - Outlaws NWX/WAX sprite decoder
 *
 * Reverse-engineered from olwin.exe:
 *   wax_LoadCostume  @0x0044d240  (WAXF/CELT container + cell loader)
 *   wax_LoadFrames   @0x0044dac0  (FRMT frame table)
 *   wax_LoadChors    @0x0044de10  (CHOT choreography table)
 *   RLE line decoder @0x004d3cf0
 *
 * See wax.h for the on-disk layout. Unlike the previous heuristic decoder,
 * this reads the explicit per-cell size headers, so cell boundaries and
 * row/column orientation are exact (no scanning, no sentinels).
 */
#include "wax.h"

/* Write one pixel (palette index -> RGBA) into the cell buffer.
 * i    = offset-table line index (row for row-major, column for column-major)
 * pos  = position along the line (column for row-major, row for column-major)
 * glow = cell has the translucent flag (CELT flag 0x20). In such cells the
 *        original engine's blit (FUN_004fef60) draws palette indices 1-15 as a
 *        TRANSLUCENT glow (blended into the scene through a translucency table)
 *        while indices >= 16 stay opaque. We approximate that here: indices
 *        1-15 become a warm muzzle-flash ramp (orange halo -> white core) with
 *        alpha rising with the index, so the flash reads as a soft glow over the
 *        opaque gun instead of solid EGA blocks. */
static inline void write_pixel(u8 *pixels, u32 w, u32 h,
                               u32 i, u32 pos, bool row_major,
                               u8 idx, const u8 palette[256][3], bool glow) {
    if (idx == 0) return; /* transparent */
    u8 r, g, b, a = 255;
    if (glow && idx < 16) {
        /* Translucent glow ramp: idx encodes intensity (1 = faint outer halo,
         * 15 = bright core). Warm orange -> white, alpha proportional to idx. */
        f32 t = (f32)idx / 15.0f;
        r = 255;
        g = (u8)(140.0f + 115.0f * t);
        b = (u8)( 40.0f + 190.0f * t);
        a = (u8)(40.0f + 205.0f * t);
    } else {
        r = palette[idx][0]; g = palette[idx][1]; b = palette[idx][2];
        if (r == 255 && g == 0 && b == 255) return; /* magenta color key */
    }
    u32 px = row_major ? pos : i;
    u32 py = row_major ? i   : pos;
    if (px >= w || py >= h) return;
    u8 *out = &pixels[(py * w + px) * 4];
    out[0] = r; out[1] = g; out[2] = b; out[3] = a;
}

/*
 * Decode one CELT cell into RGBA.
 *   base   points at the 20-byte cell header [synch,size,w,h,flags].
 *   returns bytes consumed (20 + size), or 0 on failure.
 */
static u32 decode_cell(WaxCell *cell, const u8 *base, const u8 *file_end,
                       const u8 palette[256][3]) {
    memset(cell, 0, sizeof(*cell));
    if (base + 20 > file_end) return 0;
    u32 size  = *(const u32 *)(base + 4);
    u32 w     = *(const u32 *)(base + 8);
    u32 h     = *(const u32 *)(base + 12);
    u32 flags = *(const u32 *)(base + 16);
    const u8 *data = base + 20;
    if (data + size > file_end) return 0;
    /* Degenerate/empty cell (zero-sized placeholder). These occur in real
     * sprites (e.g. BGY enemies); the original loader still consumes them.
     * Record an empty cell and advance past its data so later cells decode. */
    if (w == 0 || h == 0 || w > 4096 || h > 4096) {
        cell->flags = flags;
        return 20 + size;
    }

    bool row_major = (flags & 1) == 0;
    bool glow      = (flags & WAX_CELL_TRANSLUCENT) != 0;
    u32  n_lines   = row_major ? h : w; /* offset-table entries */
    u32  span      = row_major ? w : h; /* pixels per line */

    if ((u64)n_lines * 4u > size) return 0;
    const u32 *offs = (const u32 *)data;

    cell->pixels = calloc((size_t)w * h * 4, 1);
    if (!cell->pixels) return 0;
    cell->width    = w;
    cell->height   = h;
    cell->offset_x = 0;
    cell->offset_y = 0;
    cell->flags    = flags;

    for (u32 i = 0; i < n_lines; i++) {
        u32 start = offs[i];
        u32 end   = (i + 1 < n_lines) ? offs[i + 1] : size;
        if (start >= size || end > size || end < start) continue;
        const u8 *src     = data + start;
        const u8 *src_end = data + end;

        u32 pos = 0;
        while (pos < span && src < src_end) {
            u8  ctrl  = *src++;
            u32 count = (ctrl >> 1) + 1;
            if (pos + count > span) count = span - pos;
            if (ctrl & 1) {
                if (src >= src_end) break;
                u8 fill = *src++;
                for (u32 k = 0; k < count; k++, pos++)
                    write_pixel(cell->pixels, w, h, i, pos, row_major, fill, palette, glow);
            } else {
                for (u32 k = 0; k < count; k++, pos++) {
                    if (src >= src_end) break;
                    write_pixel(cell->pixels, w, h, i, pos, row_major, *src++, palette, glow);
                }
            }
        }
    }
    return 20 + size;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

bool wax_decode(WaxSprite *sprite, const u8 *data, u32 size,
                const u8 palette[256][3]) {
    memset(sprite, 0, sizeof(*sprite));
    if (size < 32 || memcmp(data, "WAXF", 4) != 0) {
        OL_ERR("WAX: bad magic\n");
        return false;
    }
    const u8 *file_end = data + size;

    sprite->scale_x = *(const f32 *)(data + 12);
    sprite->scale_y = *(const f32 *)(data + 16);
    u32 celt_off = *(const u32 *)(data + 20);
    u32 frmt_off = *(const u32 *)(data + 24);
    u32 chot_off = *(const u32 *)(data + 28);

    /* ---- CELT: decode all cells ---- */
    if (celt_off == 0 || celt_off + 8 > size ||
        memcmp(data + celt_off, "CELT", 4) != 0) {
        OL_ERR("WAX: missing CELT\n");
        return false;
    }
    /* CELT header: "CELT"(4) + cell_count(4) + chunk_size(4), then cells. */
    u32 ncells = *(const u32 *)(data + celt_off + 4);
    if (ncells > WAX_MAX_CELLS) ncells = WAX_MAX_CELLS;

    const u8 *p = data + celt_off + 12;
    for (u32 i = 0; i < ncells; i++) {
        WaxCell *c = &sprite->cells[sprite->cell_count];
        u32 consumed = decode_cell(c, p, file_end, palette);
        if (consumed == 0) {
            /* Record an empty cell so indices stay aligned, then stop. */
            sprite->cells[sprite->cell_count] = (WaxCell){0};
            sprite->cell_count++;
            break;
        }
        sprite->cell_count++;
        p += consumed;
    }

    /* ---- FRMT: frame table ---- */
    if (frmt_off && frmt_off + 8 <= size &&
        memcmp(data + frmt_off, "FRMT", 4) == 0) {
        /* FRMT header: "FRMT"(4) + frame_count(4) + chunk_size(4), then frames. */
        u32 nframes = *(const u32 *)(data + frmt_off + 4);
        if (nframes > WAX_MAX_FRAMES) nframes = WAX_MAX_FRAMES;
        const u8 *fp = data + frmt_off + 12;
        for (u32 i = 0; i < nframes && fp + 24 <= file_end; i++) {
            i32 field_a = *(const i32 *)(fp + 4);
            u32 nviews  = *(const u32 *)(fp + 20);
            fp += 24;
            if (nviews > 4096) break; /* corrupt */

            WaxFrame *fr = &sprite->frames[sprite->frame_count];
            memset(fr, 0, sizeof(*fr));
            fr->seq = (u32)field_a;
            fr->view_count = (nviews < WAX_DIRS) ? nviews : WAX_DIRS;

            for (u32 v = 0; v < nviews && fp + 16 <= file_end; v++, fp += 16) {
                u32 cell = *(const u32 *)(fp + 0);
                i32 ox   = *(const i32 *)(fp + 4);
                i32 oy   = *(const i32 *)(fp + 8);
                if (v < WAX_DIRS) fr->view_cell[v] = cell;
                if (v == 0) {
                    fr->cell_idx = cell;
                    fr->off_x = ox;
                    fr->off_y = oy;
                    if (cell < sprite->cell_count) {
                        fr->frame_w = (f32)sprite->cells[cell].width;
                        fr->frame_h = (f32)sprite->cells[cell].height;
                        /* Anchor the cell's stored offset from its first ref */
                        if (sprite->cells[cell].offset_x == 0 &&
                            sprite->cells[cell].offset_y == 0) {
                            sprite->cells[cell].offset_x = ox;
                            sprite->cells[cell].offset_y = oy;
                        }
                    }
                }
            }
            sprite->frame_count++;
        }
    }

    /* ---- CHOT: choreography table ---- */
    u32 chor0_dir_frame[64]; /* per-direction first display frame of chor 0 */
    u32 chor0_ndirs = 0;
    for (u32 i = 0; i < 64; i++) chor0_dir_frame[i] = 0;

    if (chot_off && chot_off + 8 <= size &&
        memcmp(data + chot_off, "CHOT", 4) == 0) {
        /* CHOT header: "CHOT"(4) + chor_count(4) + chunk_size(4), then chors. */
        u32 nchors = *(const u32 *)(data + chot_off + 4);
        if (nchors > WAX_MAX_CHORS) nchors = WAX_MAX_CHORS;
        const u8 *cp = data + chot_off + 12;
        for (u32 i = 0; i < nchors && cp + 32 <= file_end; i++) {
            i32 rate  = *(const i32 *)(cp + 16);
            u32 ndirs = *(const u32 *)(cp + 24);
            u32 nfrm  = *(const u32 *)(cp + 28);
            cp += 32;
            u64 tok_bytes = (u64)ndirs * nfrm * 2u;
            if (ndirs == 0 || nfrm == 0 || ndirs > 256 || nfrm > 4096 ||
                cp + tok_bytes > file_end) {
                /* Corrupt chor: stop parsing further chors. */
                break;
            }
            const u16 *tok = (const u16 *)cp;

            WaxChor *ch = &sprite->chors[sprite->chor_count];
            ch->rate  = rate;
            ch->ndirs = ndirs;
            ch->nfrm  = nfrm;
            ch->seq_len = (nfrm < WAX_MAX_SEQ) ? nfrm : WAX_MAX_SEQ;
            for (u32 t = 0; t < ch->seq_len; t++)
                ch->seq[t] = tok[t]; /* direction 0 stream */
            /* Full per-direction grid (capped) for directional enemy anims. */
            ch->grid_dirs = (ndirs < WAX_CHOR_MAXDIRS) ? ndirs : WAX_CHOR_MAXDIRS;
            ch->grid_nfrm = (nfrm  < WAX_CHOR_MAXNFRM) ? nfrm  : WAX_CHOR_MAXNFRM;
            for (u32 dd = 0; dd < ch->grid_dirs; dd++)
                for (u32 tt = 0; tt < ch->grid_nfrm; tt++)
                    ch->grid[dd][tt] = tok[(u64)dd * nfrm + tt];
            sprite->chor_count++;

            /* Capture chor 0's per-direction first display frame for dir_cell */
            if (i == 0) {
                chor0_ndirs = ndirs;
                for (u32 d = 0; d < ndirs && d < 64; d++) {
                    const u16 *ds = tok + (u64)d * nfrm;
                    u32 first = 0;
                    for (u32 t = 0; t < nfrm; t++) {
                        if (ds[t] < 0x8000) { first = ds[t]; break; }
                    }
                    chor0_dir_frame[d] = first;
                }
            }
            cp += tok_bytes;
        }
    }

    /* ---- Build 8-direction idle cell table from chor 0 ---- */
    if (chor0_ndirs > 0) {
        for (u32 d = 0; d < WAX_DIRS; d++) {
            u32 src = (u32)((u64)d * chor0_ndirs / WAX_DIRS);
            if (src >= chor0_ndirs) src = 0;
            u32 fi = chor0_dir_frame[src];
            sprite->dir_cell[d] = (fi < sprite->frame_count)
                                  ? sprite->frames[fi].cell_idx : 0;
        }
    } else {
        /* No CHOT: fall back to first frames / cell 0. */
        for (u32 d = 0; d < WAX_DIRS; d++)
            sprite->dir_cell[d] = (d < sprite->frame_count)
                                  ? sprite->frames[d].cell_idx : 0;
    }

    /* Idle world-space dimensions from direction-0 cell. */
    {
        u32 idle = sprite->dir_cell[0];
        if (idle < sprite->cell_count) {
            sprite->first_w = (f32)sprite->cells[idle].width;
            sprite->first_h = (f32)sprite->cells[idle].height;
        }
    }

    OL_LOG("WAX: %u cells, %u frames, %u chors, scale=(%.1f,%.1f)\n",
           sprite->cell_count, sprite->frame_count, sprite->chor_count,
           sprite->scale_x, sprite->scale_y);
    return sprite->cell_count > 0;
}

/* Walk a token stream (opcodes + frame indices), emitting display frames.
 * Shared by wax_chor_frames (dir 0) and wax_chor_frames_dir. */
static u32 expand_token_stream(const WaxSprite *sprite, const u16 *seq,
                              u32 seq_len, u32 dt_ms,
                              u32 *out_frames, u32 *out_dt, u32 max) {
    u32 n = 0;
    for (u32 t = 0; t < seq_len && n < max; t++) {
        u16 tok = seq[t];
        if (tok < 0x8000) {
            if (tok < sprite->frame_count) {
                out_frames[n] = tok;
                if (out_dt) out_dt[n] = dt_ms;
                n++;
            }
            continue;
        }
        u16 param = (t + 1 < seq_len) ? seq[t + 1] : 0;
        t++; /* consume inline param word */
        switch (tok) {
        case 0xFFFF: /* stop */
        case 0xFFFE: /* end-of-cycle / loop */
            return n;
        case 0xFFFB: /* event with `param` extra inline words */
            t += param;
            break;
        case 0xFFFC: /* chain chor */
        case 0xFFFA: /* jump to frame */
        case 0xFFF7: /* sound/callback */
            t += 1;
            break;
        default:
            break;
        }
    }
    return n;
}

u32 wax_chor_frames(const WaxSprite *sprite, u32 chor,
                    u32 *out_frames, u32 *out_dt, u32 max) {
    if (chor >= sprite->chor_count || max == 0) return 0;
    const WaxChor *ch = &sprite->chors[chor];
    u32 dt_ms = (ch->rate > 0) ? (1000u / (u32)ch->rate) : 100u;
    if (dt_ms == 0) dt_ms = 1;
    return expand_token_stream(sprite, ch->seq, ch->seq_len, dt_ms,
                               out_frames, out_dt, max);
}

u32 wax_chor_frames_dir(const WaxSprite *sprite, u32 chor, u32 dir,
                        u32 *out_frames, u32 *out_dt, u32 max) {
    if (chor >= sprite->chor_count || max == 0) return 0;
    const WaxChor *ch = &sprite->chors[chor];
    u32 dt_ms = (ch->rate > 0) ? (1000u / (u32)ch->rate) : 100u;
    if (dt_ms == 0) dt_ms = 1;
    if (ch->grid_dirs == 0)
        return expand_token_stream(sprite, ch->seq, ch->seq_len, dt_ms,
                                   out_frames, out_dt, max);
    if (dir >= ch->grid_dirs) dir %= ch->grid_dirs;
    return expand_token_stream(sprite, ch->grid[dir], ch->grid_nfrm, dt_ms,
                               out_frames, out_dt, max);
}

void wax_free(WaxSprite *sprite) {
    for (u32 i = 0; i < sprite->cell_count; i++) {
        if (sprite->cells[i].pixels) {
            free(sprite->cells[i].pixels);
            sprite->cells[i].pixels = NULL;
        }
    }
    sprite->cell_count  = 0;
    sprite->frame_count = 0;
    sprite->chor_count  = 0;
}
