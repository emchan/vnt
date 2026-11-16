#include <stdlib.h>
#include <string.h>

#include "sixel_out.h"

/*
 * Write the sixel data straight out.
 */
static int sixel_write_cb(char *data, int size, void *priv)
{
    FILE *fp = (FILE *)priv;
    size_t written;

    if (size <= 0) {
        return 0;
    }
    written = fwrite(data, 1, (size_t)size, fp);
    if (written != (size_t)size) {
        return -1;
    }
    return (int)written;
}

/*
 * Write the sixel data wrapped in a tmux passthrough sequence.
 *
 * tmux forwards a DCS payload to the outer terminal when it looks like
 *
 *     ESC P tmux; <payload with every ESC doubled> ESC \
 *
 * The payload here is the complete sixel stream including its own DCS
 * envelope, so doubling the ESC bytes is enough to get it through tmux
 * untouched.
 *
 * (libsixel's own "penetrate multiplexer" mode splits the stream into
 * plain DCS envelopes without the tmux; prefix, which tmux would not
 * understand, so the wrapping is done here instead.)
 */
static int sixel_write_tmux_cb(char *data, int size, void *priv)
{
    FILE *fp = (FILE *)priv;
    int i;

    if (size <= 0) {
        return 0;
    }
    if (fwrite("\x1bPtmux;", 7, 1, fp) != 1) {
        return -1;
    }
    for (i = 0; i < size; i++) {
        if (data[i] == 0x1b) {
            if (fputc(0x1b, fp) == EOF) {
                return -1;
            }
        }
        if (fputc(data[i], fp) == EOF) {
            return -1;
        }
    }
    if (fwrite("\x1b\\", 2, 1, fp) != 1) {
        return -1;
    }
    return size;
}

int sixel_sink_init(sixel_sink_t *sink, FILE *out, int ncolors,
                    int encode_policy, int tmux_passthrough,
                    int palette_refresh, int body_only)
{
    SIXELSTATUS status;

    memset(sink, 0, sizeof(*sink));

    if (ncolors < 2) {
        ncolors = 256;
    }
    if (ncolors > 256) {
        ncolors = 256;
    }
    sink->ncolors = ncolors;
    sink->palette_refresh = palette_refresh;
    sink->body_only_enabled = body_only ? 1 : 0;
    sink->body_only = 0;

    status = sixel_output_new(&sink->output,
                              tmux_passthrough ? sixel_write_tmux_cb
                                               : sixel_write_cb,
                              out, NULL);
    if (SIXEL_FAILED(status) || sink->output == NULL) {
        sink->output = NULL;
        return -1;
    }

    /*
     * SIZE policy packs each 6 pixel band more tightly (a few percent
     * smaller output, pixel identical). AUTO and FAST are equivalent.
     */
    sixel_output_set_encode_policy(sink->output, encode_policy);

    status = sixel_dither_new(&sink->dither, ncolors, NULL);
    if (SIXEL_FAILED(status) || sink->dither == NULL) {
        sink->dither = NULL;
        sixel_output_destroy(sink->output);
        sink->output = NULL;
        return -1;
    }

    sixel_dither_set_pixelformat(sink->dither, SIXEL_PIXELFORMAT_RGB888);

    return 0;
}

void sixel_sink_destroy(sixel_sink_t *sink)
{
    if (sink->dither != NULL) {
        sixel_dither_destroy(sink->dither);
        sink->dither = NULL;
    }
    if (sink->output != NULL) {
        sixel_output_destroy(sink->output);
        sink->output = NULL;
    }
}

/*
 * Build the colour palette from the given frame.
 */
static int build_palette(sixel_sink_t *sink, const unsigned char *rgb,
                         int width, int height)
{
    SIXELSTATUS status;

    status = sixel_dither_initialize(sink->dither,
                                     (unsigned char *)rgb,
                                     width,
                                     height,
                                     SIXEL_PIXELFORMAT_RGB888,
                                     SIXEL_LARGE_NORM,
                                     SIXEL_REP_CENTER_BOX,
                                     SIXEL_QUALITY_LOW);
    if (SIXEL_FAILED(status)) {
        return -1;
    }
    return 0;
}

int sixel_sink_frame(sixel_sink_t *sink, const unsigned char *rgb,
                     int width, int height)
{
    SIXELSTATUS status;
    int refresh_now = 0;

    if (sink->output == NULL || sink->dither == NULL) {
        return -1;
    }
    if (rgb == NULL || width <= 0 || height <= 0) {
        return -1;
    }

    if (sink->frame_count == 0) {
        /* First frame: derive the palette from it and emit it. */
        if (build_palette(sink, rgb, width, height) != 0) {
            return -1;
        }
        sink->body_only = 0;
    } else if (sink->palette_refresh > 0 &&
               (sink->frame_count % sink->palette_refresh) == 0) {
        /*
         * Optional: the screen contents may have drifted away from the
         * palette we derived at startup. Re-derive it occasionally and
         * re-emit the colour registers for this frame.
         */
        refresh_now = 1;
        if (build_palette(sink, rgb, width, height) != 0) {
            return -1;
        }
        sink->body_only = 0;
    }

    /*
     * body_only mode suppresses the colour register definitions. It is
     * only valid when the terminal keeps a shared palette across frames
     * (DECRST 1070). If the user disabled it, always send the palette.
     */
    if (!sink->body_only_enabled) {
        sink->body_only = 0;
    }

    sixel_dither_set_body_only(sink->dither, sink->body_only);

    status = sixel_encode((unsigned char *)rgb, width, height, 3,
                           sink->dither, sink->output);
    if (SIXEL_FAILED(status)) {
        return -1;
    }

    /*
     * From the second frame on we only send the body; the terminal still
     * holds the colour registers we sent with the first frame.
     */
    if (sink->body_only_enabled) {
        sink->body_only = 1;
    }
    sink->frame_count++;

    (void)refresh_now;
    return 0;
}
