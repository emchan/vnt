/*
 * vncsixel - a VNC client that draws the remote desktop in the terminal
 *            using SIXEL graphics.
 *
 *   remote desktop ──RFB──> libvncclient ──> 32bpp framebuffer
 *                                              │
 *                                              v
 *                                   fb_to_rgb24()  (format conversion)
 *                                              │
 *                                              v
 *                                   scale_rgb24()  (fit to terminal)
 *                                              │
 *                                              v
 *                                   libsixel       (palette + sixel)
 *                                              │
 *                                              v
 *                                        terminal
 *
 * Build:  make
 * Run:    ./vncsixel host[:port] [options]
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>

#include <rfb/rfbclient.h>
#include <rfb/keysym.h>

#include "scaler.h"
#include "sixel_out.h"
#include "terminal.h"

#define PROG "vnt"

typedef struct {
    rfbClient *client;

    sixel_sink_t sink;
    FILE *out;

    uint8_t *rgb;      /* full resolution RGB24 of the remote framebuffer */
    size_t rgb_capacity;

    uint8_t *scaled;   /* terminal sized RGB24 */
    size_t scaled_capacity;
    int scaled_w, scaled_h;

    int img_w, img_h;  /* dimensions of the sixel image we last drew */

    int mouse_px;      /* image pixel X of the last mouse event */
    int mouse_py;      /* image pixel Y of the last mouse event */
    int cursor_rx;     /* remote framebuffer X sent to the server */
    int cursor_ry;     /* remote framebuffer Y sent to the server */
    int has_mouse;     /* whether a mouse event has been received */

    int last_px_w;     /* terminal pixel width at last render */
    int last_px_h;     /* terminal pixel height at last render */
    int needs_clear;   /* clear the screen before the next render (resize) */
    int pixels_dirty;  /* new pixel data arrived */
    int rendered_once; /* at least one frame has been drawn */

    /*
     * Coverage tracking. libvncclient hands us the framebuffer one update
     * message at a time, and a full screen refresh is split into several
     * messages (the server's update buffer is finite). We therefore keep
     * track of which rows of the screen have been received so that we know
     * when a frame is complete and worth drawing.
     */
    uint8_t *row_seen; /* one byte per row of the remote framebuffer */
    int row_seen_cap;
    int rows_covered;

    long last_update_ms;

    const char *ppm_path; /* if set, write the received framebuffer here */
    FILE *ppm;
} viewer_t;

static volatile sig_atomic_t g_resized = 0;
static volatile sig_atomic_t g_interrupted = 0;
static int g_verbose = 0;
static int g_debug_mouse = 0;
static int g_draw_cursor = 1;   /* draw a cursor on the image by default */
static int g_calibrate = 0;
static int g_stretch = 0;
static int g_center = 0;
static int g_mouse_offset_x = 0;
static int g_mouse_offset_y = 0;

static void on_sigwinch(int sig)
{
    (void)sig;
    g_resized = 1;
}

static void on_interrupt(int sig)
{
    (void)sig;
    g_interrupted = 1;
}

/* ------------------------------------------------------------------ */
/* libvncclient callbacks                                             */
/* ------------------------------------------------------------------ */

static long now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/*
 * Called by libvncclient for every rectangle that actually carries pixels.
 *
 * Note that the pseudo encodings (NewFBSize, ExtDesktopSize, cursor shapes,
 * ...) are handled inside libvncclient and never reach this callback, so
 * this is a reliable "pixels arrived" signal. FinishedFrameBufferUpdate is
 * *not* - it also fires for the pseudo encoding only messages.
 */
static void got_framebuffer_update(rfbClient *client, int x, int y, int w, int h)
{
    viewer_t *view = (viewer_t *)rfbClientGetClientData(client, client);
    int r;

    if (view == NULL || h <= 0 || w <= 0) {
        return;
    }

    for (r = y; r < y + h; r++) {
        if (r >= 0 && r < view->row_seen_cap && !view->row_seen[r]) {
            view->row_seen[r] = 1;
            view->rows_covered++;
        }
    }

    view->pixels_dirty = 1;
    view->last_update_ms = now_ms();

    if (g_verbose) {
        fprintf(stderr, PROG ": rect update %dx%d at (%d,%d), rows covered %d/%d\n",
                w, h, x, y, view->rows_covered, client->height);
    }
}

static void finished_framebuffer_update(rfbClient *client)
{
    (void)client;
    /* The screen is updated, but it may not be complete - see the row
     * coverage tracking in got_framebuffer_update(). */
}

/*
 * Password callback. libvncclient frees the returned string.
 */
static char *password_getter(rfbClient *client)
{
    char *pw;
    char buf[128];
    size_t len;
    struct termios saved, raw;
    int have_termios = 0;

    (void)client;

    fprintf(stderr, "Password: ");
    fflush(stderr);

    /*
     * Disable echo while reading the password so that it is not displayed
     * on the terminal. Restore the original settings afterwards.
     */
    if (tcgetattr(STDIN_FILENO, &saved) == 0) {
        have_termios = 1;
        raw = saved;
        raw.c_lflag &= ~(unsigned int)(ECHO | ECHONL);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    }

    if (fgets(buf, sizeof(buf), stdin) == NULL) {
        buf[0] = '\0';
    }

    if (have_termios) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
    }

    len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }
    fprintf(stderr, "\n");

    pw = (char *)malloc(len + 1);
    if (pw == NULL) {
        return NULL;
    }
    memcpy(pw, buf, len + 1);
    return pw;
}

/* ------------------------------------------------------------------ */
/* Rendering                                                          */
/* ------------------------------------------------------------------ */

static int ensure_row_seen(viewer_t *v, int height)
{
    if (height <= 0) {
        return -1;
    }
    if (v->row_seen_cap != height) {
        uint8_t *p = (uint8_t *)realloc(v->row_seen, (size_t)height);
        if (p == NULL) {
            return -1;
        }
        v->row_seen = p;
        v->row_seen_cap = height;
    }
    memset(v->row_seen, 0, (size_t)height);
    v->rows_covered = 0;
    return 0;
}

static double now_seconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0.0;
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int ensure_buffers(viewer_t *v, int fb_w, int fb_h, int tw, int th)
{
    size_t need_rgb = (size_t)fb_w * (size_t)fb_h * 3;
    size_t need_scaled = (size_t)tw * (size_t)th * 3;

    if (need_rgb > v->rgb_capacity) {
        uint8_t *p = (uint8_t *)realloc(v->rgb, need_rgb);
        if (p == NULL) {
            return -1;
        }
        v->rgb = p;
        v->rgb_capacity = need_rgb;
    }
    if (need_scaled > v->scaled_capacity) {
        uint8_t *p = (uint8_t *)realloc(v->scaled, need_scaled);
        if (p == NULL) {
            return -1;
        }
        v->scaled = p;
        v->scaled_capacity = need_scaled;
    }
    v->scaled_w = tw;
    v->scaled_h = th;
    return 0;
}

static void dump_ppm(viewer_t *v, const uint8_t *rgb, int w, int h)
{
    FILE *f;
    int x, y;

    if (v->ppm_path == NULL) {
        return;
    }
    if (v->ppm == NULL) {
        v->ppm = fopen(v->ppm_path, "wb");
        if (v->ppm == NULL) {
            fprintf(stderr, PROG ": cannot write %s\n", v->ppm_path);
            v->ppm_path = NULL;
            return;
        }
    }
    f = v->ppm;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const uint8_t *p = rgb + ((size_t)y * w + x) * 3;
            fputc(p[0], f);
            fputc(p[1], f);
            fputc(p[2], f);
        }
    }
    fflush(f);
}

static int render(viewer_t *v, int upscale)
{
    rfbClient *client = v->client;
    term_size_t ts;
    fb_format_t fmt;
    int fb_w = client->width;
    int fb_h = client->height;
    double sx, sy, s;
    int tw, th;

    if (fb_w <= 0 || fb_h <= 0 || client->frameBuffer == NULL) {
        return -1;
    }

    term_get_size(&ts);
    if (ts.pixel_w <= 0 || ts.pixel_h <= 0) {
        return -1;
    }

    if (g_stretch) {
        /*
         * Stretch the remote desktop to fill the whole terminal pixel area.
         * This distorts the aspect ratio but makes the image and the
         * terminal grid coincide exactly, which simplifies mouse mapping.
         */
        tw = ts.pixel_w;
        th = ts.pixel_h;
        if (tw < 1) {
            tw = 1;
        }
        if (th < 1) {
            th = 1;
        }
        s = 1.0;
    } else {
        /*
         * Fit the remote desktop into the terminal's pixel area, preserving
         * the aspect ratio. The terminal area is derived from the cell grid
         * times the (possibly estimated) cell size.
         */
        sx = (double)ts.pixel_w / (double)fb_w;
        sy = (double)ts.pixel_h / (double)fb_h;
        s = (sx < sy) ? sx : sy;
        if (!upscale && s > 1.0) {
            s = 1.0;
        }
        if (s <= 0.0) {
            return -1;
        }

        tw = (int)((double)fb_w * s);
        th = (int)((double)fb_h * s);
        if (tw < 1) {
            tw = 1;
        }
        if (th < 1) {
            th = 1;
        }
        if (tw > ts.pixel_w) {
            tw = ts.pixel_w;
        }
        if (th > ts.pixel_h) {
            th = ts.pixel_h;
        }
    }

    if (g_verbose) {
        fprintf(stderr, PROG ": terminal %dx%d cells (%dx%d px cell), "
                "area %dx%d px; remote %dx%d -> image %dx%d (scale %.3f)\n",
                ts.cols, ts.rows, ts.cell_w, ts.cell_h,
                ts.pixel_w, ts.pixel_h, fb_w, fb_h, tw, th, s);
    }

    if (ensure_buffers(v, fb_w, fb_h, tw, th) != 0) {
        fprintf(stderr, PROG ": out of memory\n");
        return -1;
    }

    /* 1. framebuffer -> RGB24 */
    fmt.bits_per_pixel = client->format.bitsPerPixel;
    fmt.big_endian = client->format.bigEndian ? 1 : 0;
    fmt.red_shift = client->format.redShift;
    fmt.green_shift = client->format.greenShift;
    fmt.blue_shift = client->format.blueShift;
    fmt.red_max = client->format.redMax;
    fmt.green_max = client->format.greenMax;
    fmt.blue_max = client->format.blueMax;

    if (fb_to_rgb24(client->frameBuffer, fb_w, fb_h, &fmt, v->rgb) != 0) {
        fprintf(stderr, PROG ": unsupported pixel format (%d bpp)\n",
                fmt.bits_per_pixel);
        return -1;
    }

    dump_ppm(v, v->rgb, fb_w, fb_h);

    /* 2. RGB24 -> terminal sized RGB24 */
    scale_rgb24(v->rgb, fb_w, fb_h, tw, th, v->scaled);

    /*
     * Calibration mode: draw a grid showing terminal cell boundaries and
     * a marker at (0,0) to verify the image position in the terminal.
     */
    if (g_calibrate) {
        term_size_t ts;
        term_get_size(&ts);
        /* Draw vertical lines at cell boundaries. */
        {
            int x;
            for (x = 0; x < tw; x += ts.cell_w) {
                int y;
                for (y = 0; y < th; y++) {
                    v->scaled[(y * tw + x) * 3 + 0] = 0;
                    v->scaled[(y * tw + x) * 3 + 1] = 255;
                    v->scaled[(y * tw + x) * 3 + 2] = 0;
                }
            }
        }
        /* Draw horizontal lines at cell boundaries. */
        {
            int y;
            for (y = 0; y < th; y += ts.cell_h) {
                int x;
                for (x = 0; x < tw; x++) {
                    v->scaled[(y * tw + x) * 3 + 0] = 0;
                    v->scaled[(y * tw + x) * 3 + 1] = 255;
                    v->scaled[(y * tw + x) * 3 + 2] = 0;
                }
            }
        }
        /* Draw a red marker at (0,0) to verify image position. */
        {
            int r, c;
            for (r = 0; r < 8 && r < th; r++) {
                for (c = 0; c < 8 && c < tw; c++) {
                    v->scaled[(r * tw + c) * 3 + 0] = 255;
                    v->scaled[(r * tw + c) * 3 + 1] = 0;
                    v->scaled[(r * tw + c) * 3 + 2] = 0;
                }
            }
        }
        /* Draw a blue marker at (tw-8, th-8) for the bottom-right. */
        {
            int r, c;
            for (r = th - 8; r < th && r >= 0; r++) {
                for (c = tw - 8; c < tw && c >= 0; c++) {
                    v->scaled[(r * tw + c) * 3 + 0] = 0;
                    v->scaled[(r * tw + c) * 3 + 1] = 0;
                    v->scaled[(r * tw + c) * 3 + 2] = 255;
                }
            }
        }
    }

    /*
     * Debug: draw a visible marker on the image at the mapped mouse
     * position. This shows exactly where the system thinks the pointer is.
     */
    if (g_draw_cursor && v->has_mouse) {
        int px, py, r, c;

        /* Red crosshair at the mouse click location (terminal -> image). */
        px = v->mouse_px;
        py = v->mouse_py;
        if (px >= 0 && px < tw && py >= 0 && py < th) {
            for (r = -4; r <= 4; r++) {
                for (c = -4; c <= 4; c++) {
                    int x = px + c, y = py + r;
                    if (x >= 0 && x < tw && y >= 0 && y < th) {
                        if (r == 0 || c == 0 || r == -4 || r == 4 || c == -4 || c == 4) {
                            v->scaled[(y * tw + x) * 3 + 0] = 255;
                            v->scaled[(y * tw + x) * 3 + 1] = 0;
                            v->scaled[(y * tw + x) * 3 + 2] = 0;
                        }
                    }
                }
            }
        }

        /* Yellow crosshair at the VNC cursor location (remote -> image). */
        px = (int)(((long)v->cursor_rx * tw) / fb_w);
        py = (int)(((long)v->cursor_ry * th) / fb_h);
        if (px >= 0 && px < tw && py >= 0 && py < th) {
            for (r = -4; r <= 4; r++) {
                for (c = -4; c <= 4; c++) {
                    int x = px + c, y = py + r;
                    if (x >= 0 && x < tw && y >= 0 && y < th) {
                        if (r == 0 || c == 0 || r == -4 || r == 4 || c == -4 || c == 4) {
                            v->scaled[(y * tw + x) * 3 + 0] = 255;
                            v->scaled[(y * tw + x) * 3 + 1] = 255;
                            v->scaled[(y * tw + x) * 3 + 2] = 0;
                        }
                    }
                }
            }
        }
    }

    /*
     * On a terminal resize the new image may be smaller than the previous
     * one, leaving stale pixels at the right/bottom. Clear the screen
     * first so the new frame starts from a blank slate.
     */
    if (v->needs_clear) {
        term_write("\x1b[2J\x1b[H", 7);
        v->needs_clear = 0;
    }


    /* 3. RGB24 -> sixel */
    /*
     * Publish the new image dimensions *before* writing the frame out:
     * a mouse event may arrive as soon as the first bytes hit the
     * terminal, and the coordinates have to be mapped with the dimensions
     * of the image that is actually on screen.
     */
    v->img_w = tw;
    v->img_h = th;

    /*
     * When centering, move the text cursor (which is where DECSDM pins the
     * sixel) to the cell that contains the top-left corner of the image.
     */
    if (g_center && v->out == stdout) {
        int ox = (ts.pixel_w - tw) / 2;
        int oy = (ts.pixel_h - th) / 2;
        int col = ox / ts.cell_w + 1;
        int row = oy / ts.cell_h + 1;
        char buf[64];
        int n;
        if (col < 1) {
            col = 1;
        }
        if (row < 1) {
            row = 1;
        }
        n = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row, col);
        if (n > 0) {
            term_write(buf, (size_t)n);
        }
    }

    if (sixel_sink_frame(&v->sink, v->scaled, tw, th) != 0) {
        fprintf(stderr, PROG ": failed to encode frame as sixel\n");
        return -1;
    }
    fflush(v->out);

    /*
     * The crosshair drawn above is the local cursor: it is rendered on the
     * image itself at the exact mapped position, so it is always pixel-
     * accurate. The terminal text cursor is left hidden to avoid a
     * half-cell offset between the text cursor (cell granularity) and the
     * crosshair (pixel granularity).
     */

    return 0;
}

/* ------------------------------------------------------------------ */
/* Input                                                              */
/* ------------------------------------------------------------------ */

static void send_mouse(viewer_t *v, const term_event_t *ev)
{
    rfbClient *client = v->client;
    int fb_w = client->width;
    int fb_h = client->height;
    term_size_t ts;
    int px, py, rx, ry;

    if (fb_w <= 0 || fb_h <= 0 || v->img_w <= 0 || v->img_h <= 0) {
        return;
    }

    term_get_size(&ts);

    /*
     * Apply a manual calibration offset (in terminal cells). This is for
     * terminals whose mouse reporting origin does not match the image
     * origin (e.g. a border or title bar).
     */
    {
        int cx = ev->x + g_mouse_offset_x;
        int cy = ev->y + g_mouse_offset_y;
        int ipx, ipy;
        int ox = 0, oy = 0;
        if (cx < 1) cx = 1;
        if (cy < 1) cy = 1;

        v->has_mouse = 1;

        /*
         * Terminal cell -> terminal pixel. Use the centre of the cell: the
         * terminal only tells us which cell the pointer is in, so the
         * centre is the least biased estimate of the actual pointer position.
         */
        px = (cx - 1) * ts.cell_w + ts.cell_w / 2;
        py = (cy - 1) * ts.cell_h + ts.cell_h / 2;

        /*
         * Convert the terminal pixel into an image pixel. In the default
         * top-left mode the image starts at (0,0); when centering it is
         * offset by half the letterbox margin.
         */
        if (g_center) {
            ox = (ts.pixel_w - v->img_w) / 2;
            oy = (ts.pixel_h - v->img_h) / 2;
        }
        ipx = px - ox;
        ipy = py - oy;
        if (ipx < 0) {
            ipx = 0;
        }
        if (ipy < 0) {
            ipy = 0;
        }
        if (ipx >= v->img_w) {
            ipx = v->img_w - 1;
        }
        if (ipy >= v->img_h) {
            ipy = v->img_h - 1;
        }

        /* Save the clamped image pixel for the debug crosshair. */
        v->mouse_px = ipx;
        v->mouse_py = ipy;

        /* Image pixel -> remote framebuffer pixel. */
        rx = (int)(((long)ipx * fb_w) / v->img_w);
        ry = (int)(((long)ipy * fb_h) / v->img_h);
    }
    if (rx < 0) {
        rx = 0;
    }
    if (ry < 0) {
        ry = 0;
    }
    if (rx >= fb_w) {
        rx = fb_w - 1;
    }
    if (ry >= fb_h) {
        ry = fb_h - 1;
    }

    /* Save the remote coordinates for the yellow cursor crosshair. */
    v->cursor_rx = rx;
    v->cursor_ry = ry;

    if (g_debug_mouse) {
        fprintf(stderr, PROG ": mouse cell=(%d,%d) px=(%d,%d) img=%dx%d fb=%dx%d -> remote=(%d,%d) btn=%d\\n",
                ev->x, ev->y, px, py, v->img_w, v->img_h, fb_w, fb_h, rx, ry, ev->button_mask);
        fflush(stderr);
    }

    SendPointerEvent(client, rx, ry, ev->button_mask);
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr,
        "usage: " PROG " [options] host[:port]\n"
        "\n"
        "  -e, --encodings LIST   RFB encodings to request (default: tight zrle hextile raw copyrect)\n"
        "  -c, --colors N         palette size, 2..256 (default: 256)\n"
        "  -f, --fps N            maximum frames per second (default: 30, 0 = unlimited)\n"
        "  -q, --quality POLICY   sixel encode policy: auto|fast|size (default: size)\n"
        "  -r, --refresh N        re-derive the palette every N frames (default: 0, never)\n"
        "  -u, --upscale          scale up small remote desktops to fill the terminal\n"
        "  -v, --view-only        do not send keyboard or mouse events\n"
        "      --tmux             wrap the sixel output for tmux passthrough\n"
        "      --full-palette     re-send the colour palette every frame (default)\n"
        "      --body-only        reuse the palette across frames (smaller output,\n"
        "                         requires a terminal with a shared colour register bank)\n"
        "      --dump FILE        write the sixel stream to FILE instead of the terminal\n"
        "      --dump-ppm FILE    write the received framebuffer to FILE as a PPM\n"
        "  -V, --verbose           print scaling information\n"
        "      --debug-mouse       print mouse coordinate mapping to stderr\n"
        "      --draw-cursor       draw a visible marker on the image at the\n"
        "                          mapped mouse position (for debugging)\n"
        "      --mouse-offset X,Y  manual calibration offset in terminal cells\n"
        "                          (for terminals whose mouse origin is shifted)\n"
        "      --cell-size W,H     override the terminal cell size in pixels\n"
        "                          (for terminals that misreport it, e.g. ConPTY)\n"
        "      --stretch           scale the image to fill the terminal (distorts\n"
        "                          aspect ratio; simplifies mouse mapping)\n"
        "      --center            centre the image in the terminal instead of\n"
        "                          top-left aligning it\n"
        "      --check             report whether the terminal supports sixel and exit\n"
        "      --frames N         exit after N frames (for testing)\n"
        "  -h, --help             this message\n");
}

int main(int argc, char **argv)
{
    viewer_t v;
    const char *host = NULL;
    const char *encodings = "tight zrle hextile raw copyrect";
    const char *dump_path = NULL;
    const char *ppm_path = NULL;
    const char *quality = "size";
    int ncolors = 256;
    int fps = 30;
    int body_only = 0;   /* default: re-send the palette every frame.
                            Many terminals (Windows Terminal, foot by
                            default, ...) use a private colour register
                            bank per graphic, so body-only frames would
                            reference stale registers and dither. */
    int palette_refresh = 0;
    int upscale = 0;
    int view_only = 0;
    int tmux_passthrough = 0;
    int max_frames = 0;
    int check_only = 0;
    int encode_policy = SIXEL_ENCODEPOLICY_SIZE;
    int i;
    int frames_drawn = 0;
    double last_draw = 0.0;
    double min_interval;

    memset(&v, 0, sizeof(v));

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage();
            return 0;
        } else if (strcmp(a, "-e") == 0 || strcmp(a, "--encodings") == 0) {
            if (++i >= argc) { usage(); return 1; }
            encodings = argv[i];
        } else if (strcmp(a, "-c") == 0 || strcmp(a, "--colors") == 0) {
            if (++i >= argc) { usage(); return 1; }
            ncolors = atoi(argv[i]);
        } else if (strcmp(a, "-f") == 0 || strcmp(a, "--fps") == 0) {
            if (++i >= argc) { usage(); return 1; }
            fps = atoi(argv[i]);
        } else if (strcmp(a, "-q") == 0 || strcmp(a, "--quality") == 0) {
            if (++i >= argc) { usage(); return 1; }
            quality = argv[i];
        } else if (strcmp(a, "-r") == 0 || strcmp(a, "--refresh") == 0) {
            if (++i >= argc) { usage(); return 1; }
            palette_refresh = atoi(argv[i]);
        } else if (strcmp(a, "-u") == 0 || strcmp(a, "--upscale") == 0) {
            upscale = 1;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--view-only") == 0) {
            view_only = 1;
        } else if (strcmp(a, "--tmux") == 0) {
            tmux_passthrough = 1;
        } else if (strcmp(a, "--full-palette") == 0) {
            body_only = 0;   /* kept for backwards compatibility */
        } else if (strcmp(a, "--body-only") == 0) {
            body_only = 1;
        } else if (strcmp(a, "--debug-mouse") == 0) {
            g_debug_mouse = 1;
        } else if (strcmp(a, "--calibrate") == 0) {
            g_calibrate = 1;
        } else if (strcmp(a, "--stretch") == 0) {
            g_stretch = 1;
        } else if (strcmp(a, "--center") == 0) {
            g_center = 1;
        } else if (strcmp(a, "--cell-size") == 0) {
            int cw = 0, ch = 0;
            if (i + 1 >= argc) { usage(); return 1; }
            i++;
            if (sscanf(argv[i], "%d,%d", &cw, &ch) != 2 &&
                sscanf(argv[i], "%dx%d", &cw, &ch) != 2) {
                fprintf(stderr, PROG ": bad --cell-size (expected W,H or WxH)\n");
                return 1;
            }
            term_set_cell_size(cw, ch);
        } else if (strcmp(a, "--mouse-offset") == 0) {
            if (i + 1 >= argc) { usage(); return 1; }
            i++;
            if (sscanf(argv[i], "%d,%d", &g_mouse_offset_x, &g_mouse_offset_y) != 2) {
                fprintf(stderr, PROG ": bad --mouse-offset (expected X,Y)\n");
                return 1;
            }
        } else if (strcmp(a, "--draw-cursor") == 0) {
            g_draw_cursor = 1;
        } else if (strcmp(a, "--no-cursor") == 0) {
            g_draw_cursor = 0;
        } else if (strcmp(a, "--dump") == 0) {
            if (++i >= argc) { usage(); return 1; }
            dump_path = argv[i];
        } else if (strcmp(a, "--dump-ppm") == 0) {
            if (++i >= argc) { usage(); return 1; }
            ppm_path = argv[i];
        } else if (strcmp(a, "--frames") == 0) {
            if (++i >= argc) { usage(); return 1; }
            max_frames = atoi(argv[i]);
        } else if (strcmp(a, "-V") == 0 || strcmp(a, "--verbose") == 0) {
            g_verbose = 1;
        } else if (strcmp(a, "--check") == 0) {
            check_only = 1;
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, PROG ": unknown option '%s'\n", a);
            usage();
            return 1;
        } else {
            host = a;
        }
    }

    if (check_only) {
        int sixel;
        term_init(0);
        sixel = term_detect_sixel();
        term_restore();
        if (sixel == 1) {
            printf("sixel: supported\n");
            return 0;
        }
        if (sixel == 0) {
            printf("sixel: not supported\n");
            return 1;
        }
        printf("sixel: unknown (the terminal did not answer)\n");
        return 2;
    }

    if (host == NULL) {
        usage();
        return 1;
    }

    v.ppm_path = ppm_path;

    if (strcmp(quality, "auto") == 0) {
        encode_policy = SIXEL_ENCODEPOLICY_AUTO;
    } else if (strcmp(quality, "fast") == 0) {
        encode_policy = SIXEL_ENCODEPOLICY_FAST;
    } else if (strcmp(quality, "size") == 0) {
        encode_policy = SIXEL_ENCODEPOLICY_SIZE;
    } else {
        fprintf(stderr, PROG ": unknown quality '%s'\n", quality);
        return 1;
    }

    if (ncolors < 2) {
        ncolors = 2;
    }
    if (ncolors > 256) {
        ncolors = 256;
    }
    min_interval = (fps > 0) ? (1.0 / (double)fps) : 0.0;

    /* ---- output sink ---- */
    if (dump_path != NULL) {
        v.out = fopen(dump_path, "wb");
        if (v.out == NULL) {
            fprintf(stderr, PROG ": cannot open '%s': %s\n", dump_path,
                    strerror(errno));
            return 1;
        }
    } else {
        v.out = stdout;
    }

    /* ---- libvncclient ---- */
    v.client = rfbGetClient(8, 3, 4);
    if (v.client == NULL) {
        fprintf(stderr, PROG ": failed to allocate the VNC client\n");
        return 1;
    }

    /*
     * Force 32 bit true colour. With bigEndian = 0 and these shifts the
     * server always sends a little endian 32 bit word whose byte 0 is red,
     * byte 1 green, byte 2 blue and byte 3 unused - on any host.
     */
    v.client->format.bitsPerPixel = 32;
    v.client->format.depth = 24;
    v.client->format.trueColour = 1;
    v.client->format.bigEndian = 0;
    v.client->format.redMax = 255;
    v.client->format.greenMax = 255;
    v.client->format.blueMax = 255;
    v.client->format.redShift = 0;
    v.client->format.greenShift = 8;
    v.client->format.blueShift = 16;

    v.client->appData.encodingsString = encodings;
    v.client->appData.viewOnly = view_only ? TRUE : FALSE;
    v.client->appData.shareDesktop = TRUE;
    v.client->appData.useRemoteCursor = FALSE; /* server draws the cursor */
    v.client->GetPassword = password_getter;

    v.client->GotFrameBufferUpdate = got_framebuffer_update;
    v.client->FinishedFrameBufferUpdate = finished_framebuffer_update;
    /*
     * Use the rfbClient pointer itself as the tag so that the callback can
     * find the viewer state again.
     */
    rfbClientSetClientData(v.client, (void *)v.client, (void *)&v);

    {
        /* rfbInitClient() expects { program name, ..., host }. */
        char *myargv[3];
        int myargc = 2;

        myargv[0] = (char *)PROG;
        myargv[1] = (char *)host;
        myargv[2] = NULL;

        if (!rfbInitClient(v.client, &myargc, myargv)) {
            /*
             * rfbInitClient() calls rfbClientCleanup() itself when it
             * fails, so the client is already gone here.
             */
            fprintf(stderr, PROG ": failed to connect to %s\n", host);
            return 1;
        }
    }

    /* Re-assert our pixel format; the server has to convert to it. */
    SetFormatAndEncodings(v.client);

    fprintf(stderr, PROG ": connected to %s, desktop %dx%d\n", host,
            v.client->width, v.client->height);

    /* ---- sixel sink ---- */
    if (sixel_sink_init(&v.sink, v.out, ncolors, encode_policy,
                        tmux_passthrough, palette_refresh, body_only) != 0) {
        fprintf(stderr, PROG ": failed to initialise the sixel encoder\n");
        rfbClientCleanup(v.client);
        return 1;
    }

    /* ---- terminal ---- */
    if (dump_path == NULL) {
        struct sigaction sa;

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_sigwinch;
        sigaction(SIGWINCH, &sa, NULL);

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_interrupt;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);

        term_init(!view_only);

        /*
         * Warn if the terminal does not look like it can do sixel. This is
         * only a hint - some terminals never answer the device attributes
         * query - so we do not refuse to run.
         */
        {
            int sixel = term_detect_sixel();
            if (sixel == 0) {
                fprintf(stderr, PROG ": warning: the terminal does not seem to "
                        "support sixel graphics; the output will be garbage.\n");
            } else if (sixel < 0) {
                fprintf(stderr, PROG ": note: the terminal did not answer the "
                        "device attributes query; assuming sixel support.\n");
            }
        }
    }

    /* ---- main loop ---- */
    if (ensure_row_seen(&v, v.client->height) != 0) {
        fprintf(stderr, PROG ": out of memory\n");
        sixel_sink_destroy(&v.sink);
        rfbClientCleanup(v.client);
        return 1;
    }

    for (;;) {
        term_event_t ev;
        double now;
        long nowms;
        int frame_complete;

        if (g_interrupted) {
            break;
        }

        /* Handle terminal input. */
        if (!view_only && dump_path == NULL) {
            while (term_poll_event(&ev)) {
                if (ev.kind == TERM_EV_KEY) {
                    if (ev.keysym == 0x03) { /* Ctrl-C */
                        g_interrupted = 1;
                        break;
                    }
                    SendKeyEvent(v.client, ev.keysym, TRUE);
                    SendKeyEvent(v.client, ev.keysym, FALSE);
                } else if (ev.kind == TERM_EV_MOUSE) {
                    send_mouse(&v, &ev);
                }
            }
        }

        if (g_resized) {
            g_resized = 0;
            v.needs_clear = 1;
            v.pixels_dirty = 1;
        }

        /*
         * Poll the terminal size as well: SIGWINCH is not always delivered
         * (for example when the process is not in the foreground process
         * group of the pty), so we also detect a change here.
         */
        {
            term_size_t ts;
            term_get_size(&ts);
            if (ts.pixel_w > 0 && ts.pixel_h > 0 &&
                (ts.pixel_w != v.last_px_w || ts.pixel_h != v.last_px_h)) {
                v.last_px_w = ts.pixel_w;
                v.last_px_h = ts.pixel_h;
                v.needs_clear = 1;
                if (v.rendered_once) {
                    v.pixels_dirty = 1;
                }
            }
        }

        /*
         * Wait for a server message. The timeout keeps the input handling
         * responsive even when the remote desktop is idle.
         */
        if (WaitForMessage(v.client, 20000) > 0) {
            if (!HandleRFBServerMessage(v.client)) {
                fprintf(stderr, PROG ": connection lost\n");
                break;
            }
        }

        now = now_seconds();
        nowms = now_ms();

        if (v.client->height != v.row_seen_cap) {
            /* The remote desktop was resized. */
            ensure_row_seen(&v, v.client->height);
            v.pixels_dirty = 1;
        }

        frame_complete = (v.rows_covered >= v.client->height);

        if (v.pixels_dirty) {
            /*
             * Draw when the frame is complete, or when the server went
             * quiet (so that a partial frame still gets displayed), or
             * when the frame rate limit allows a progressive update.
             */
            int idle = (nowms - v.last_update_ms) > 200;

            if (frame_complete || idle ||
                (now - last_draw) >= min_interval) {
                if (render(&v, upscale) != 0) {
                    break;
                }
                v.pixels_dirty = 0;
                last_draw = now;
                v.rendered_once = 1;

                if (frame_complete) {
                    /* Start tracking coverage for the next frame. */
                    memset(v.row_seen, 0, (size_t)v.row_seen_cap);
                    v.rows_covered = 0;
                    frames_drawn++;
                    if (max_frames > 0 && frames_drawn >= max_frames) {
                        break;
                    }
                }
            }
        }
    }

    /* ---- teardown ---- */
    if (dump_path == NULL) {
        term_restore();
    }
    sixel_sink_destroy(&v.sink);
    free(v.rgb);
    free(v.scaled);
    free(v.row_seen);
    if (v.ppm != NULL) {
        fclose(v.ppm);
    }
    if (v.out != NULL && v.out != stdout) {
        fclose(v.out);
    }
    rfbClientCleanup(v.client);

    return 0;
}
