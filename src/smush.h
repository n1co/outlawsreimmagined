/*
 * smush.h - LucasArts SMUSH (.SAN) cutscene decoder
 *
 * Decodes the Outlaws cinematics (ANIM/AHDR container, codec47 video,
 * NPAL/XPAL palettes, IACT audio). This is a faithful reimplementation of
 * the original SMUSH player: the codec47 pixel decoder, the palette-fade
 * (XPAL) math and the IACT audio decompression all match the retail engine
 * (cross-referenced against LECSMUSH.DLL / the documented codec47 algorithm).
 *
 * Outlaws uses codec47 "compression 1" for intra/keyframes, which is NOT the
 * same as the delta path (compression 2): it runs the glyph decoder without
 * the sequence-continuity gate. This was verified by decoding real frames.
 *
 * Usage:
 *   SmushVideo *v = smush_open(data, size);
 *   while (smush_next(v, &rgba, &pcm, &pcm_bytes)) {
 *       // upload rgba (w*h*4) to a texture, queue pcm (22050Hz S16 stereo)
 *   }
 *   smush_close(v);
 */
#pragma once
#include "engine.h"

typedef struct SmushVideo SmushVideo;

/* Open a SMUSH animation from an in-memory .SAN buffer. The buffer must
 * remain valid for the lifetime of the returned SmushVideo (it is not
 * copied). Returns NULL on parse failure. */
SmushVideo *smush_open(const u8 *data, u32 size);

/* Free all decoder resources (does not free the input buffer). */
void smush_close(SmushVideo *v);

int   smush_width(const SmushVideo *v);
int   smush_height(const SmushVideo *v);
int   smush_frame_count(const SmushVideo *v);
float smush_fps(const SmushVideo *v);

/*
 * Decode the next frame.
 *  - *out_rgba  → internal RGBA8 buffer (width*height*4), valid until the
 *                 next smush_next()/smush_close(). NULL if this frame had no
 *                 video (rare).
 *  - *out_pcm   → interleaved signed-16 stereo PCM decoded during this frame
 *                 (22050 Hz), or NULL if none. Valid until next call.
 *  - *out_pcm_bytes → byte length of *out_pcm.
 * Returns 1 if a frame was produced, 0 when the animation is finished.
 */
int smush_next(SmushVideo *v, const u8 **out_rgba,
               const i16 **out_pcm, int *out_pcm_bytes);
