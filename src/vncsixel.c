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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>

#include <rfb/rfbclient.h>
#include <rfb/keysym.h>
#include <rfb/rfbconfig.h>

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

    /* Framebuffer / connection diagnostics. */
    long connect_ms;
    unsigned long update_messages;
    unsigned long rects_received;
    unsigned long long pixels_received;
    int black_checked;
    int black_refresh_requests;
    int black_warning_printed;
    int no_update_warning_printed;
    int raw_pixels_logged;
    int raw_fallback_allowed;
    int raw_fallback_done;
    int jpeg_fallback_done;

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

/*
 * Connection diagnostics. These are only used by -V/--verbose; the normal
 * output path is unchanged.
 */
static const char *g_username = NULL;
static int g_password_attempted = 0;
static int g_credential_attempted = 0;
static int g_use_server_format = 0;
static int g_force_rgbx = 0;
static int g_safe_mode = 0;
static int g_no_jpeg = 0;
static int g_native_format_logged = 0;
static rfbBool (*g_default_malloc_frame_buffer)(rfbClient *) = NULL;
static char g_last_libvnc_message[512];
static char g_last_libvnc_error[512];

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
/* Connection / negotiation diagnostics                               */
/* ------------------------------------------------------------------ */

static void vlog(const char *fmt, ...)
{
    va_list ap;

    if (!g_verbose) {
        return;
    }

    fputs(PROG ": ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static const char *security_type_name(uint32_t type)
{
    switch (type) {
    case rfbNoAuth:          return "None";
    case rfbVncAuth:         return "VNC authentication";
    case rfbRA2:             return "RA2";
    case rfbRA2ne:           return "RA2ne";
    case rfbTight:           return "Tight";
    case rfbUltra:           return "Ultra";
    case rfbTLS:             return "Anonymous TLS";
    case rfbVeNCrypt:        return "VeNCrypt";
#ifdef rfbSASL
    case rfbSASL:            return "SASL";
#endif
    case rfbARD:             return "Apple Remote Desktop";
    case rfbUltraMSLogonI:   return "UltraVNC MS-Logon I";
    case rfbUltraMSLogonII:  return "UltraVNC MS-Logon II";
    case rfbMSLogon:         return "MS-Logon";
    default:                 return "unknown";
    }
}

static const char *sub_auth_name(uint32_t type)
{
    switch (type) {
    case rfbVeNCryptPlain:    return "Plain";
    case rfbVeNCryptTLSNone:  return "TLSNone";
    case rfbVeNCryptTLSVNC:   return "TLSVNC";
    case rfbVeNCryptTLSPlain: return "TLSPlain";
    case rfbVeNCryptX509None: return "X509None";
    case rfbVeNCryptX509VNC:  return "X509VNC";
    case rfbVeNCryptX509Plain:return "X509Plain";
#ifdef rfbVeNCryptX509SASL
    case rfbVeNCryptX509SASL: return "X509SASL";
#endif
#ifdef rfbVeNCryptTLSSASL
    case rfbVeNCryptTLSSASL:  return "TLSSASL";
#endif
    default:                  return "unknown/none";
    }
}

static void log_pixel_format(const char *label, const rfbPixelFormat *fmt)
{
    vlog("%s: %d bpp, depth %d, %s endian, %s colour, "
         "max %u/%u/%u, shift %u/%u/%u",
         label, (unsigned)fmt->bitsPerPixel, (unsigned)fmt->depth,
         fmt->bigEndian ? "big" : "little",
         fmt->trueColour ? "true" : "palette",
         (unsigned)fmt->redMax, (unsigned)fmt->greenMax,
         (unsigned)fmt->blueMax,
         (unsigned)fmt->redShift, (unsigned)fmt->greenShift,
         (unsigned)fmt->blueShift);
}

static void log_negotiated_security(const rfbClient *client)
{
    if (client->authScheme == rfbVeNCrypt) {
        vlog("security: %s (%u), sub-auth %s (%u)",
             security_type_name(client->authScheme),
             (unsigned)client->authScheme,
             sub_auth_name(client->subAuthScheme),
             (unsigned)client->subAuthScheme);
    } else {
        vlog("security: %s (%u)",
             security_type_name(client->authScheme),
             (unsigned)client->authScheme);
    }
}

/* Strip trailing newlines so the two log hooks can always add exactly one. */
static void format_libvnc_message(char *buf, size_t buflen,
                                  const char *fmt, va_list ap)
{
    int n;

    if (buflen == 0) {
        return;
    }
    n = vsnprintf(buf, buflen, fmt, ap);
    if (n < 0) {
        buf[0] = '\0';
        return;
    }
    if ((size_t)n >= buflen) {
        n = (int)buflen - 1;
    }
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = '\0';
    }
}

/* Bounded append helper: never lets `used` run past the buffer. */
static size_t append_limited(char *buf, size_t size, size_t used,
                             const char *fmt, ...)
{
    va_list ap;
    int n;

    if (size == 0 || used >= size - 1) {
        return size == 0 ? 0 : size - 1;
    }

    va_start(ap, fmt);
    n = vsnprintf(buf + used, size - used, fmt, ap);
    va_end(ap);

    if (n < 0) {
        return used;
    }
    if ((size_t)n >= size - used) {
        return size - 1;
    }
    return used + (size_t)n;
}

static void libvnc_log_common(const char *fmt, va_list ap, int is_error)
{
    char buf[1024];

    format_libvnc_message(buf, sizeof(buf), fmt, ap);

    snprintf(g_last_libvnc_message, sizeof(g_last_libvnc_message), "%s", buf);
    if (is_error) {
        snprintf(g_last_libvnc_error, sizeof(g_last_libvnc_error), "%s", buf);
    }

    if (!g_verbose) {
        return;
    }

    fprintf(stderr, PROG ": libvnc%s: %s\n", is_error ? " error" : "", buf);
    fflush(stderr);
}

static void libvnc_log(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    libvnc_log_common(fmt, ap, 0);
    va_end(ap);
}

static void libvnc_error(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    libvnc_log_common(fmt, ap, 1);
    va_end(ap);
}

static char *dup_string(const char *s)
{
    size_t len;
    char *copy;

    if (s == NULL) {
        return NULL;
    }
    len = strlen(s);
    copy = (char *)malloc(len + 1);
    if (copy != NULL) {
        memcpy(copy, s, len + 1);
    }
    return copy;
}

/*
 * Read one line from stdin. If the prompt is non-NULL, echo is disabled
 * while it is read and the prompt is written to stderr.
 */
static char *read_line(const char *prompt)
{
    char buf[128];
    size_t len;
    struct termios saved, raw;
    int have_termios = 0;

    if (prompt != NULL) {
        fprintf(stderr, "%s", prompt);
        fflush(stderr);

        if (tcgetattr(STDIN_FILENO, &saved) == 0) {
            have_termios = 1;
            raw = saved;
            raw.c_lflag &= ~(unsigned int)(ECHO | ECHONL);
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        } else if (g_verbose) {
            vlog("stdin is not a terminal; reading without echo suppression");
        }
    }

    if (fgets(buf, sizeof(buf), stdin) == NULL) {
        buf[0] = '\0';
    }

    if (have_termios) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
    }
    if (prompt != NULL) {
        fprintf(stderr, "\n");
    }

    len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }

    return dup_string(buf);
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
    view->rects_received++;
    view->pixels_received += (unsigned long long)w * (unsigned long long)h;

    if (g_verbose) {
        fprintf(stderr, PROG ": rect update %dx%d at (%d,%d), rows covered %d/%d\n",
                w, h, x, y, view->rows_covered, client->height);
    }
}

static void finished_framebuffer_update(rfbClient *client)
{
    viewer_t *view = (viewer_t *)rfbClientGetClientData(client, client);

    if (view != NULL) {
        view->update_messages++;
    }
    /* The screen is updated, but it may not be complete - see the row
     * coverage tracking in got_framebuffer_update(). */
}

/*
 * Password callback. libvncclient frees the returned string.
 */
static char *password_getter(rfbClient *client)
{
    char *pw;

    (void)client;

    g_password_attempted = 1;
    vlog("server requested a VNC password; prompting on stdin");

    pw = read_line("Password: ");
    if (pw == NULL) {
        vlog("failed to read the password");
        return NULL;
    }

    vlog("read a %zu-byte password%s", strlen(pw),
         pw[0] == '\0' ? " (empty!)" : "");
    return pw;
}

/*
 * Credential callback for security types that carry a user name and password
 * inside the protocol (VeNCrypt, MS-Logon, Apple Remote Desktop, ...).
 *
 * libvncclient only considers these security types when this callback is set,
 * so it is installed only when --user is given. That keeps the default
 * security-type selection exactly as before.
 */
static rfbCredential *credential_getter(rfbClient *client, int credentialType)
{
    rfbCredential *cred;

    (void)client;

    g_credential_attempted = 1;

    if (credentialType == rfbCredentialTypeX509) {
        fprintf(stderr, PROG ": the server requested an X.509 client "
                "certificate, which this client does not support\n");
        vlog("credential request: X.509 (unsupported, refusing)");
        return NULL;
    }
    if (credentialType != rfbCredentialTypeUser) {
        fprintf(stderr, PROG ": the server requested unknown credential "
                "type %d\n", credentialType);
        return NULL;
    }

    vlog("server requested user name + password credentials "
         "(VeNCrypt/MS-Logon/ARD style authentication)");

    if (g_username == NULL) {
        vlog("no user name available; refusing the credential request");
        return NULL;
    }

    cred = (rfbCredential *)calloc(1, sizeof(*cred));
    if (cred == NULL) {
        vlog("out of memory while building credentials");
        return NULL;
    }

    cred->userCredential.username = dup_string(g_username);
    cred->userCredential.password = read_line("Password: ");

    if (cred->userCredential.username == NULL ||
        cred->userCredential.password == NULL) {
        vlog("failed to read the user credentials");
        free(cred->userCredential.username);
        free(cred->userCredential.password);
        free(cred);
        return NULL;
    }

    vlog("supplied user name '%s' and a %zu-byte password",
         cred->userCredential.username,
         strlen(cred->userCredential.password));
    return cred;
}

/*
 * libvncclient calls this after ServerInit (so client->si.format is known)
 * but before it sends SetPixelFormat. That gives us a chance to see the
 * server's native format, and optionally ask for it instead of forcing a
 * translation to the 32bpp RGBX format.
 */
static rfbBool malloc_frame_buffer(rfbClient *client)
{
    if (g_verbose && !g_native_format_logged) {
        g_native_format_logged = 1;
        log_pixel_format("server native pixel format", &client->si.format);
        log_pixel_format("currently requested client format", &client->format);
    }

    /*
     * Prefer the server's native 32bpp format by default. This is what
     * other viewers normally do and it avoids asking the server to run a
     * pixel-format translation for every rectangle; some servers (notably
     * TightVNC) produce black output on some translation paths. For 16bpp
     * we keep the requested 32bpp format unless --server-format was given,
     * so we do not silently lose colour depth.
     */
    if (!g_force_rgbx &&
        (g_use_server_format ||
         (client->si.format.trueColour &&
          client->si.format.bitsPerPixel == 32))) {
        if (client->si.format.bitsPerPixel == 16 ||
            client->si.format.bitsPerPixel == 32) {
            client->format = client->si.format;
            vlog("using the server's native %d bpp pixel format",
                 client->si.format.bitsPerPixel);
        } else {
            vlog("server native format is %d bpp, which is not supported "
                 "without a colour map; keeping the requested format",
                 client->si.format.bitsPerPixel);
        }
    } else if (g_force_rgbx) {
        vlog("keeping the forced 32bpp RGBX pixel format");
    }

    if (g_default_malloc_frame_buffer == NULL) {
        return FALSE;
    }
    return g_default_malloc_frame_buffer(client);
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

/*
 * Classify a frame as "effectively black". A real desktop usually has at
 * least a mouse cursor or a window border somewhere, so treat a frame with
 * less than 0.1% non-black pixels as blank. Values below 8 are treated as
 * black to tolerate JPEG ringing around a black background.
 */
static int framebuffer_is_effectively_black(const uint8_t *rgb,
                                            size_t pixels,
                                            unsigned long *non_black_out)
{
    unsigned long non_black = 0;
    size_t i;

    for (i = 0; i < pixels; i++) {
        const uint8_t *p = rgb + i * 3;
        if (p[0] > 8 || p[1] > 8 || p[2] > 8) {
            non_black++;
        }
    }

    if (non_black_out != NULL) {
        *non_black_out = non_black;
    }
    return non_black <= pixels / 1000;
}

/*
 * Return true if the raw framebuffer contains any non-zero byte. Used to
 * distinguish "the server sent black pixels" from "the server sent data
 * that we decoded as black because the pixel format was misread".
 */
static int framebuffer_raw_has_data(const rfbClient *client)
{
    int bpp = client->format.bitsPerPixel / 8;
    size_t pixels, i;
    size_t non_zero = 0;

    if (client->frameBuffer == NULL) {
        return 0;
    }
    if (bpp < 1 || bpp > 4) {
        /* Unknown layout: do not claim the data is empty. */
        return 1;
    }

    pixels = (size_t)client->width * (size_t)client->height;
    for (i = 0; i < pixels; i++) {
        const uint8_t *p = client->frameBuffer + i * (size_t)bpp;
        int j;
        for (j = 0; j < bpp; j++) {
            if (p[j] > 8) {
                non_zero++;
                break;
            }
        }
    }

    return non_zero > pixels / 1000;
}

/*
 * Log raw (still server-encoded) pixel words from several places in the
 * framebuffer. If these are non-zero while the converted RGB image is black,
 * the negotiated pixel format is being misread.
 */
static void log_raw_pixel_samples(viewer_t *v)
{
    rfbClient *client = v->client;
    int bpp = client->format.bitsPerPixel / 8;
    size_t pixels = (size_t)client->width * (size_t)client->height;
    size_t sample[4];
    int i, j;
    char line[256];
    size_t used = 0;

    v->raw_pixels_logged = 1;
    if (bpp < 1 || bpp > 4) {
        bpp = 4;
    }
    if (pixels == 0) {
        return;
    }

    sample[0] = 0;
    sample[1] = pixels / 3;
    sample[2] = (2 * pixels) / 3;
    sample[3] = pixels - 1;

    used = append_limited(line, sizeof(line), used, "bpp=%d", bpp * 8);
    for (i = 0; i < 4; i++) {
        used = append_limited(line, sizeof(line), used, " | [%zu]", sample[i]);
        for (j = 0; j < bpp; j++) {
            used = append_limited(line, sizeof(line), used, " %02x",
                                  client->frameBuffer[sample[i] * (size_t)bpp +
                                                      (size_t)j]);
        }
    }
    vlog("raw framebuffer samples: %s", line);
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

static void print_version(void)
{
    printf(PROG " build information\n");
    printf("  built: %s %s\n", __DATE__, __TIME__);
    printf("  compiler: %s\n", __VERSION__);
    printf("  pointer size: %u-bit\n", (unsigned)(sizeof(void *) * 8));
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    defined(__ORDER_BIG_ENDIAN__)
    printf("  byte order: %s\n",
           __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ ? "little" : "big");
#endif
#ifdef LIBVNCSERVER_VERSION
    printf("  libvncclient headers: %s\n", LIBVNCSERVER_VERSION);
#endif
#ifdef LIBSIXEL_VERSION
    printf("  libsixel headers: %s\n", LIBSIXEL_VERSION);
#endif
}

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
        "  -U, --user NAME        user name for servers that need VeNCrypt/MS-Logon/\n"
        "                         ARD credentials (the password is prompted for)\n"
        "      --server-format    also accept the server's native 16bpp format\n"
        "                         (native 32bpp is preferred automatically)\n"
        "      --safe-mode        debugging profile: force raw encoding, the\n"
        "                         server's native pixel format, and no JPEG\n"
        "      --force-rgbx       keep the requested 32bpp RGBX format instead\n"
        "                         of using the server's native format\n"
        "      --no-jpeg          keep Tight encoding but disable its JPEG mode\n"
        "      --version          print build and library versions\n"
        "  -V, --verbose           print scaling, connection and negotiation details\n"
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
    int encodings_set = 0;
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
        } else if (strcmp(a, "--version") == 0) {
            print_version();
            return 0;
        } else if (strcmp(a, "-e") == 0 || strcmp(a, "--encodings") == 0) {
            if (++i >= argc) { usage(); return 1; }
            encodings = argv[i];
            encodings_set = 1;
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
        } else if (strcmp(a, "-U") == 0 || strcmp(a, "--user") == 0) {
            if (++i >= argc) { usage(); return 1; }
            g_username = argv[i];
        } else if (strcmp(a, "--server-format") == 0) {
            g_use_server_format = 1;
        } else if (strcmp(a, "--safe-mode") == 0) {
            g_safe_mode = 1;
        } else if (strcmp(a, "--force-rgbx") == 0) {
            g_force_rgbx = 1;
        } else if (strcmp(a, "--no-jpeg") == 0) {
            g_no_jpeg = 1;
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

    if (g_safe_mode) {
        if (!encodings_set) {
            encodings = "raw";
        }
        g_use_server_format = 1;
        fprintf(stderr, PROG ": safe mode: encoding '%s', "
                "server-native pixel format, JPEG disabled\n", encodings);
    }

    /*
     * If a compressed encoding is in use, a black first frame may mean the
     * decoder on this build is broken. Allow a one-time fallback to raw.
     */
    v.raw_fallback_allowed =
        strstr(encodings, "tight") != NULL ||
        strstr(encodings, "zrle") != NULL ||
        strstr(encodings, "hextile") != NULL ||
        strstr(encodings, "zlib") != NULL;

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
    if (g_verbose) {
        /*
         * libvncclient logs the whole handshake (protocol version, offered
         * security types, selected scheme, authentication result and the
         * server's default pixel format) through these hooks. Prefix the
         * messages ourselves and keep the last one so that a failed
         * rfbInitClient() can report the reason.
         */
        rfbEnableClientLogging = TRUE;
        rfbClientLog = libvnc_log;
        rfbClientErr = libvnc_error;
        vlog("libvncclient negotiation diagnostics enabled");
    }
    vlog("allocating VNC client");
    v.client = rfbGetClient(8, 3, 4);
    if (v.client == NULL) {
        fprintf(stderr, PROG ": failed to allocate the VNC client\n");
        return 1;
    }

    /*
     * The framebuffer allocation hook runs after ServerInit but before the
     * pixel format is sent, so it is the right place to inspect the server's
     * native format and optionally use it.
     */
    g_default_malloc_frame_buffer = v.client->MallocFrameBuffer;
    v.client->MallocFrameBuffer = malloc_frame_buffer;

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

    /*
     * Advertise the classic NewFBSize pseudo-encoding (-223). TightVNC's
     * server understands this but not ExtDesktopSize (-308), which is all
     * libvncclient sends by default. Without -223 TightVNC takes its
     * "desktop resize disabled" fallback for every full-screen request,
     * compositing the desktop into a temporary blank framebuffer. That path
     * is what some servers mishandle for non-default pixel formats.
     */
    v.client->canHandleNewFBSize = TRUE;

    if (g_no_jpeg || g_safe_mode) {
        v.client->appData.enableJPEG = FALSE;
    }
    if (g_safe_mode) {
        v.client->appData.compressLevel = 0;
        v.client->appData.qualityLevel = 0;
    }
    v.client->GetPassword = password_getter;
    if (g_username != NULL) {
        v.client->GetCredential = credential_getter;
        vlog("user name supplied; enabling credential-based security types");
    }

    v.client->GotFrameBufferUpdate = got_framebuffer_update;
    v.client->FinishedFrameBufferUpdate = finished_framebuffer_update;
    /*
     * Use the rfbClient pointer itself as the tag so that the callback can
     * find the viewer state again.
     */
    rfbClientSetClientData(v.client, (void *)v.client, (void *)&v);

    vlog("connecting to %s (encodings: %s, view-only: %s, shared: yes)",
         host, encodings, view_only ? "yes" : "no");
    vlog("handshake: TCP connect, RFB protocol version, security type, "
         "authentication, ServerInit");

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

            if (g_verbose) {
                if (g_last_libvnc_error[0] != '\0') {
                    vlog("last libvncclient error: %s", g_last_libvnc_error);
                }
                if (g_last_libvnc_message[0] != '\0' &&
                    strcmp(g_last_libvnc_message, g_last_libvnc_error) != 0) {
                    vlog("last libvncclient message: %s",
                         g_last_libvnc_message);
                }

                if (g_password_attempted || g_credential_attempted) {
                    vlog("diagnosis: the server requested credentials, so "
                         "the TCP connection and RFB protocol version were "
                         "accepted; authentication is the likely failure");
                    vlog("diagnosis: verify the password/user name and the "
                         "server's authentication method");
                } else {
                    vlog("diagnosis: no credentials were requested; the "
                         "failure happened during TCP connect, RFB protocol "
                         "version negotiation, or security-type selection");
                    if (g_username == NULL) {
                        vlog("diagnosis: if the server only offers "
                             "VeNCrypt/MS-Logon/ARD, retry with --user NAME");
                    }
                }
            }
            return 1;
        }
    }

    vlog("RFB handshake complete: protocol %d.%d, server '%s', desktop '%s'",
         v.client->major, v.client->minor,
         v.client->serverHost != NULL ? v.client->serverHost : host,
         v.client->desktopName != NULL ? v.client->desktopName : "(none)");
    log_negotiated_security(v.client);
    log_pixel_format("server pixel format", &v.client->format);

    /*
     * Do not call SetFormatAndEncodings() again here.
     *
     * rfbInitClient() -> rfbInitConnection() already calls it with the
     * format and encodingsString configured above, and then sends the first
     * FramebufferUpdateRequest. Sending a second SetPixelFormat/SetEncodings
     * while the first update is in flight can make a server reset its
     * encoder state mid-frame. If that corrupts the first Tight frame (often
     * the whole screen), a static desktop may never send another update to
     * repair it, leaving the viewer black. TightVNC servers are a known
     * source of first-frame corruption in this situation.
     */
    vlog("pixel format and encodings were sent by rfbInitClient()");
    log_pixel_format("requested pixel format", &v.client->format);
    vlog("requested encodings: %s", encodings);

    fprintf(stderr, PROG ": connected to %s, desktop %dx%d\n", host,
            v.client->width, v.client->height);

    v.connect_ms = now_ms();

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
            v.black_checked = 0;
            v.black_refresh_requests = 0;
            v.black_warning_printed = 0;
        }

        /*
         * If no pixels arrive at all, the problem is in encoding negotiation
         * or in the server, not in the sixel/rendering path. Say so instead
         * of leaving the user with a silent black screen.
         */
        if (v.rects_received == 0 && !v.no_update_warning_printed &&
            v.connect_ms > 0 && (nowms - v.connect_ms) > 5000) {
            v.no_update_warning_printed = 1;
            fprintf(stderr, PROG ": warning: no framebuffer updates received "
                    "in the first 5 seconds.\n");
            if (g_verbose) {
                vlog("no pixels received; the server accepted the connection "
                     "but is not sending usable framebuffer data");
                vlog("check the encoding list and try '-e raw' to bypass the "
                     "Tight/ZRLE decoders");
            }
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
                    if (g_verbose && !v.raw_pixels_logged) {
                        log_raw_pixel_samples(&v);
                    }

                    if (!v.black_checked) {
                        size_t fb_pixels = (size_t)v.client->width *
                                           (size_t)v.client->height;
                        unsigned long non_black = 0;
                        int black = framebuffer_is_effectively_black(
                            v.rgb, fb_pixels, &non_black);

                        v.black_checked = 1;
                        if (black) {
                            int raw_has_data =
                                framebuffer_raw_has_data(v.client);

                            vlog("complete frame is effectively black: "
                                 "%lu/%zu non-black pixels, raw data %s "
                                 "(updates=%lu rects=%lu pixels=%llu)",
                                 non_black, fb_pixels,
                                 raw_has_data ? "present" : "effectively empty",
                                 v.update_messages, v.rects_received,
                                 v.pixels_received);

                            if (v.black_refresh_requests < 2) {
                                v.black_refresh_requests++;
                                vlog("requesting a full non-incremental "
                                     "refresh (%d/2) to recover the frame",
                                     v.black_refresh_requests);
                                SendFramebufferUpdateRequest(v.client, 0, 0,
                                                             v.client->width,
                                                             v.client->height,
                                                             FALSE);
                                /* Check the refreshed frame as well. */
                                v.black_checked = 0;
                            } else if (!v.jpeg_fallback_done &&
                                       v.client->appData.enableJPEG) {
                                /*
                                 * Some libvncclient/libjpeg builds decode
                                 * Tight's JPEG mode incorrectly. Tight without
                                 * JPEG is still compressed and lossless, so
                                 * try that before dropping to raw.
                                 */
                                v.jpeg_fallback_done = 1;
                                vlog("still black after two refreshes; "
                                     "disabling Tight JPEG mode");
                                v.client->appData.enableJPEG = FALSE;
                                v.client->appData.qualityLevel = 0;
                                if (SetFormatAndEncodings(v.client)) {
                                    SendFramebufferUpdateRequest(v.client, 0, 0,
                                                                 v.client->width,
                                                                 v.client->height,
                                                                 FALSE);
                                    v.black_checked = 0;
                                    v.black_refresh_requests = 0;
                                } else {
                                    v.black_warning_printed = 1;
                                    fprintf(stderr, PROG ": warning: failed "
                                            "to renegotiate encodings without "
                                            "JPEG\n");
                                }
                            } else if (v.raw_fallback_allowed &&
                                       !v.raw_fallback_done) {
                                /*
                                 * The compressed decoder on this build may
                                 * be broken. Raw is always supported and
                                 * lets us confirm whether the frame itself
                                 * is good.
                                 */
                                v.raw_fallback_done = 1;
                                vlog("still black after disabling JPEG; "
                                     "switching to raw encoding");
                                v.client->appData.encodingsString = "raw";
                                if (SetFormatAndEncodings(v.client)) {
                                    SendFramebufferUpdateRequest(v.client, 0, 0,
                                                                 v.client->width,
                                                                 v.client->height,
                                                                 FALSE);
                                    v.black_checked = 0;
                                    v.black_refresh_requests = 0;
                                } else {
                                    v.black_warning_printed = 1;
                                    fprintf(stderr, PROG ": warning: failed "
                                            "to switch to raw encoding\n");
                                }
                            } else if (!v.black_warning_printed) {
                                v.black_warning_printed = 1;
                                if (raw_has_data) {
                                    fprintf(stderr, PROG ": warning: the "
                                            "converted frame is black, but the "
                                            "raw framebuffer contains data.\n");
                                    fprintf(stderr, PROG ": the pixel format "
                                            "is probably being misapplied; "
                                            "retry with '--server-format'.\n");
                                } else {
                                    fprintf(stderr, PROG ": warning: the "
                                            "server is sending an all-black "
                                            "framebuffer (the raw framebuffer "
                                            "is effectively empty).\n");
                                    fprintf(stderr, PROG ": the remote "
                                            "session may be locked, TightVNC "
                                            "may be capturing a different or "
                                            "disconnected session, Desktop "
                                            "Duplication may be returning "
                                            "black frames, or the machine may "
                                            "be headless.\n");
                                }
                                fprintf(stderr, PROG ": check the TightVNC "
                                        "server log and try another VNC viewer "
                                        "from this machine to confirm the "
                                        "server side.\n");
                            }
                        } else if (g_verbose) {
                            vlog("complete frame is not black: "
                                 "%lu/%zu non-black pixels "
                                 "(updates=%lu rects=%lu pixels=%llu)",
                                 non_black, fb_pixels, v.update_messages,
                                 v.rects_received, v.pixels_received);
                        }
                    }

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
