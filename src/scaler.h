/*
 * scaler.h - convert and rescale framebuffer pixels for sixel encoding.
 *
 * Two independent stages:
 *   1. fb_to_rgb24()  : libvncclient framebuffer (arbitrary rfbPixelFormat)
 *                       -> tightly packed RGB24.
 *   2. scale_rgb24()  : RGB24 at remote resolution -> RGB24 at terminal
 *                       resolution (box filter down, bilinear up).
 */
#ifndef VDCSX_SCALER_H
#define VDCSX_SCALER_H

#include <stdint.h>

/*
 * Description of the pixel layout of a libvncclient framebuffer. These are the
 * rfbPixelFormat fields, copied out so that this module does not have to
 * depend on rfbproto.h.
 */
typedef struct {
    int bits_per_pixel;  /* 8, 16 or 32 */
    int big_endian;      /* multi-byte pixels are big endian */
    int red_shift;
    int green_shift;
    int blue_shift;
    int red_max;
    int green_max;
    int blue_max;
} fb_format_t;

/*
 * Convert `src` (src_w * src_h pixels, tightly packed, in the given format)
 * into `dst` (src_w * src_h * 3 bytes, RGB order).
 *
 * Returns 0 on success, -1 if the format cannot be handled.
 */
int fb_to_rgb24(const uint8_t *src, int src_w, int src_h,
                const fb_format_t *fmt, uint8_t *dst);

/*
 * Rescale an RGB24 image. `dst` must hold dst_w * dst_h * 3 bytes.
 * Downscaling uses a box filter (keeps small text readable), upscaling uses
 * bilinear interpolation.
 */
void scale_rgb24(const uint8_t *src, int src_w, int src_h,
                 int dst_w, int dst_h, uint8_t *dst);

#endif /* VDCSX_SCALER_H */
