/*
 * sixel_out.h - libsixel wrapper that keeps a stable palette across frames.
 *
 * The interesting part of driving libsixel for a live stream is palette
 * handling:
 *
 *   - sixel_dither_initialize() builds a palette from a pixel buffer and
 *     marks the dither as "optimized". From then on the palette is *not*
 *     recomputed by sixel_encode(), so every frame is quantised against the
 *     same colour registers.
 *
 *   - Because the palette no longer changes, we can set "body only" mode
 *     after the first frame. That suppresses the "#Pc;2;R;G;B" colour
 *     register definitions, which for a 256 colour palette is roughly 3.3 KB
 *     of pure overhead per frame. The terminal keeps the registers it was
 *     given the first time.
 *
 * This requires the terminal to use a *shared* palette rather than a private
 * one per graphic; see terminal.c, which sends DECRST 1070 for that.
 */
#ifndef VDCSX_SIXEL_OUT_H
#define VDCSX_SIXEL_OUT_H

#include <stdio.h>

#include <sixel.h>

typedef struct {
    sixel_output_t *output;
    sixel_dither_t *dither;
    int ncolors;             /* requested palette size */
    int frame_count;         /* frames encoded so far */
    int palette_refresh;     /* re-derive palette every N frames (0 = never) */
    int body_only_enabled;   /* whether body-only mode is allowed at all */
    int body_only;           /* current body-only state of the dither */
} sixel_sink_t;

/* Returns 0 on success, -1 on failure. */
int sixel_sink_init(sixel_sink_t *sink, FILE *out, int ncolors,
                    int encode_policy, int tmux_passthrough,
                    int palette_refresh, int body_only);

void sixel_sink_destroy(sixel_sink_t *sink);

/* Encode one RGB24 frame. Returns 0 on success, -1 on failure. */
int sixel_sink_frame(sixel_sink_t *sink, const unsigned char *rgb,
                     int width, int height);

#endif /* VDCSX_SIXEL_OUT_H */
