#include <stdlib.h>
#include <string.h>

#include "scaler.h"

/*
 * Extract one channel from a raw pixel word.
 *
 * `max` is the number of distinct levels the channel has (2^n - 1 in the
 * common case). We scale it up to 0..255 so that the result does not depend
 * on the server's preferred colour depth.
 */
static inline uint8_t scale_channel(unsigned int value, int shift, int max)
{
    unsigned int v = (value >> shift) & (unsigned int)max;
    if (max <= 0) {
        return 0;
    }
    if (max == 255) {
        return (uint8_t)v;
    }
    return (uint8_t)((v * 255u + (unsigned int)max / 2) / (unsigned int)max);
}

static inline unsigned int load_pixel(const uint8_t *p, int bpp, int big_endian)
{
    switch (bpp) {
    case 2:
        return big_endian ? ((unsigned int)p[0] << 8) | p[1]
                          : ((unsigned int)p[1] << 8) | p[0];
    case 4:
        return big_endian
                   ? ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
                         ((unsigned int)p[2] << 8) | p[3]
                   : ((unsigned int)p[3] << 24) | ((unsigned int)p[2] << 16) |
                         ((unsigned int)p[1] << 8) | p[0];
    default:
        return 0;
    }
}

int fb_to_rgb24(const uint8_t *src, int src_w, int src_h,
                const fb_format_t *fmt, uint8_t *dst)
{
    int bpp;
    size_t n;

    if (src == NULL || dst == NULL || fmt == NULL) {
        return -1;
    }
    if (src_w <= 0 || src_h <= 0) {
        return -1;
    }

    bpp = fmt->bits_per_pixel / 8;
    if (bpp != 2 && bpp != 4) {
        /* 8bpp true-colour-ish formats need a colour map; libvncclient
         * would have had to fill one in, which we do not support. */
        return -1;
    }

    n = (size_t)src_w * (size_t)src_h;

    if (bpp == 4 &&
        fmt->red_shift == 0 && fmt->green_shift == 8 && fmt->blue_shift == 16 &&
        fmt->red_max == 255 && fmt->green_max == 255 && fmt->blue_max == 255 &&
        !fmt->big_endian) {
        /*
         * Fast path: 32bpp little endian with R,G,B,X byte order, which is
         * what rfbGetClient(8, 3, 4) asks for on a little endian host.
         * This is the layout we request from every server, so it is the
         * common case. Just drop the fourth byte of every pixel.
         */
        size_t i;
        for (i = 0; i < n; i++) {
            const uint8_t *s = src + i * 4;
            uint8_t *d = dst + i * 3;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
        }
        return 0;
    }

    {
        size_t i;
        for (i = 0; i < n; i++) {
            unsigned int v = load_pixel(src + i * (size_t)bpp, bpp,
                                        fmt->big_endian);
            uint8_t *d = dst + i * 3;
            d[0] = scale_channel(v, fmt->red_shift, fmt->red_max);
            d[1] = scale_channel(v, fmt->green_shift, fmt->green_max);
            d[2] = scale_channel(v, fmt->blue_shift, fmt->blue_max);
        }
    }
    return 0;
}

/*
 * Box filter downscale. For every destination pixel we average the source
 * rectangle that maps onto it. Averaging (rather than point sampling) keeps
 * small text legible when a 1920x1080 desktop is squeezed into a terminal.
 */
static void scale_down(const uint8_t *src, int sw, int sh,
                       int dw, int dh, uint8_t *dst)
{
    int dy;

    for (dy = 0; dy < dh; dy++) {
        int y0 = (int)(((long)dy * sh) / dh);
        int y1 = (int)(((long)(dy + 1) * sh + dh - 1) / dh);
        int dx;

        if (y1 <= y0) {
            y1 = y0 + 1;
        }
        if (y1 > sh) {
            y1 = sh;
        }

        for (dx = 0; dx < dw; dx++) {
            int x0 = (int)(((long)dx * sw) / dw);
            int x1 = (int)(((long)(dx + 1) * sw + dw - 1) / dw);
            unsigned long sum_r = 0, sum_g = 0, sum_b = 0;
            unsigned long count = 0;
            int y, x;

            if (x1 <= x0) {
                x1 = x0 + 1;
            }
            if (x1 > sw) {
                x1 = sw;
            }

            for (y = y0; y < y1; y++) {
                const uint8_t *row = src + ((size_t)y * sw + x0) * 3;
                for (x = x0; x < x1; x++) {
                    sum_r += row[0];
                    sum_g += row[1];
                    sum_b += row[2];
                    row += 3;
                }
                count += (unsigned long)(x1 - x0);
            }

            if (count == 0) {
                count = 1;
            }
            {
                uint8_t *d = dst + ((size_t)dy * dw + dx) * 3;
                d[0] = (uint8_t)((sum_r + count / 2) / count);
                d[1] = (uint8_t)((sum_g + count / 2) / count);
                d[2] = (uint8_t)((sum_b + count / 2) / count);
            }
        }
    }
}

/*
 * Bilinear upscale.
 */
static void scale_up(const uint8_t *src, int sw, int sh,
                     int dw, int dh, uint8_t *dst)
{
    int dy;

    for (dy = 0; dy < dh; dy++) {
        double fy = ((double)dy + 0.5) * sh / dh - 0.5;
        int y0 = (int)fy;
        double wy = fy - y0;
        int y1;
        int dx;

        if (y0 < 0) {
            y0 = 0;
            wy = 0.0;
        }
        if (y0 > sh - 1) {
            y0 = sh - 1;
            wy = 0.0;
        }
        y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;

        for (dx = 0; dx < dw; dx++) {
            double fx = ((double)dx + 0.5) * sw / dw - 0.5;
            int x0 = (int)fx;
            double wx = fx - x0;
            int x1;
            int c;

            if (x0 < 0) {
                x0 = 0;
                wx = 0.0;
            }
            if (x0 > sw - 1) {
                x0 = sw - 1;
                wx = 0.0;
            }
            x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;

            for (c = 0; c < 3; c++) {
                double p00 = src[((size_t)y0 * sw + x0) * 3 + c];
                double p01 = src[((size_t)y0 * sw + x1) * 3 + c];
                double p10 = src[((size_t)y1 * sw + x0) * 3 + c];
                double p11 = src[((size_t)y1 * sw + x1) * 3 + c];
                double top = p00 + (p01 - p00) * wx;
                double bot = p10 + (p11 - p10) * wx;
                double v = top + (bot - top) * wy;
                uint8_t *d = dst + ((size_t)dy * dw + dx) * 3;

                if (v < 0.0) {
                    v = 0.0;
                }
                if (v > 255.0) {
                    v = 255.0;
                }
                d[c] = (uint8_t)(v + 0.5);
            }
        }
    }
}

void scale_rgb24(const uint8_t *src, int src_w, int src_h,
                 int dst_w, int dst_h, uint8_t *dst)
{
    if (src == NULL || dst == NULL || dst_w <= 0 || dst_h <= 0) {
        return;
    }
    if (src_w <= 0 || src_h <= 0) {
        memset(dst, 0, (size_t)dst_w * (size_t)dst_h * 3);
        return;
    }
    if (src_w == dst_w && src_h == dst_h) {
        memcpy(dst, src, (size_t)src_w * (size_t)src_h * 3);
        return;
    }
    if (dst_w < src_w || dst_h < src_h) {
        scale_down(src, src_w, src_h, dst_w, dst_h, dst);
    } else {
        scale_up(src, src_w, src_h, dst_w, dst_h, dst);
    }
}
