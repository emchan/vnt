#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <sys/ioctl.h>

#include "terminal.h"

/*
 * Terminal control strings.
 *
 *  ?1049h/l  alternate screen buffer
 *  ?25l/h    hide / show cursor
 *  ?80h/l    DECSDM: sixel display mode. When set, a sixel graphic is
 *            placed at the top left corner of the screen and the text
 *            cursor is not moved. That is exactly what a full screen
 *            viewer wants: we never have to know where the previous
 *            frame ended up.
 *  ?1070l    DECRST 1070: use the *shared* colour register palette
 *            instead of a private palette per graphic. Required for the
 *            "body only" optimisation, otherwise every frame would have
 *            to re-send its colour register definitions.
 *  ?1002h    button event tracking mouse reporting
 *  ?1006h    SGR extended mouse reporting (so coordinates can exceed 223)
 *  ?7l       disable auto-wrap, so writing the status bar on the last
 *            row cannot scroll the screen
 */
#define TERM_ENTER_GRAPHICS \
    "\x1b[?1049h"  /* alternate screen */ \
    "\x1b[?25l"    /* hide cursor */    \
    "\x1b[?80h"    /* sixel display mode */ \
    "\x1b[?1070l"  /* shared colour registers */ \
    "\x1b[?7l"     /* no auto-wrap */

#define TERM_LEAVE_GRAPHICS \
    "\x1b[?1002l\x1b[?1006l\x1b[?1000l"  /* mouse reporting off */ \
    "\x1b[?7h"     /* auto-wrap back on */ \
    "\x1b[?80l"    /* sixel scrolling mode */ \
    "\x1b[?25h"    /* show cursor */ \
    "\x1b[?1049l"  /* leave alternate screen */

#define TERM_ENABLE_MOUSE "\x1b[?1000h\x1b[?1002h\x1b[?1006h"

#define DEFAULT_CELL_W 8
#define DEFAULT_CELL_H 16

static struct termios g_saved_termios;
static int g_termios_saved = 0;
static int g_initialised = 0;
static int g_mouse_enabled = 0;

/* Cached cell metrics. 0 means "not known yet". */
static int g_cell_w = 0;
static int g_cell_h = 0;
static int g_pixels_queried = 0;

static void query_pixel_size(void);

void term_set_cell_size(int w, int h)
{
    int changed = 0;
    if (w >= 4 && w <= 64) {
        g_cell_w = w;
        changed = 1;
    }
    if (h >= 8 && h <= 64) {
        g_cell_h = h;
        changed = 1;
    }
    if (changed) {
        g_pixels_queried = 1;
    }
}

/* Input parsing state. */
static unsigned char g_inbuf[512];
static size_t g_inlen = 0;

void term_write(const char *data, size_t len)
{
    if (data == NULL || len == 0) {
        return;
    }
    if (fwrite(data, 1, len, stdout) != len) {
        return;
    }
    fflush(stdout);
}

int term_init(int enable_mouse)
{
    struct termios raw;

    if (tcgetattr(STDIN_FILENO, &g_saved_termios) == 0) {
        g_termios_saved = 1;
        raw = g_saved_termios;
        /*
         * We want raw input: no echo, no line buffering and no signal
         * generation, so that Ctrl-C reaches the viewer instead of killing
         * it. Signals are handled explicitly.
         */
        raw.c_iflag &= ~(unsigned int)(IGNBRK | BRKINT | PARMRK |
                                       ISTRIP | INLCR | IGNCR | ICRNL |
                                       IXON);
        raw.c_oflag &= ~(unsigned int)(OPOST);
        raw.c_lflag &= ~(unsigned int)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    }

    term_write(TERM_ENTER_GRAPHICS, sizeof(TERM_ENTER_GRAPHICS) - 1);
    if (enable_mouse) {
        term_write(TERM_ENABLE_MOUSE, sizeof(TERM_ENABLE_MOUSE) - 1);
        g_mouse_enabled = 1;
    }

    g_initialised = 1;

    /*
     * Learn the real cell size up front so that the first frame already
     * uses the correct metrics (important on ConPTY / Windows Terminal).
     */
    if (isatty(STDOUT_FILENO)) {
        struct winsize ws;
        memset(&ws, 0, sizeof(ws));
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
        if (ws.ws_xpixel == 0 || ws.ws_ypixel == 0) {
            query_pixel_size();
        }
    }

    return 0;
}

void term_restore(void)
{
    if (g_initialised) {
        term_write(TERM_LEAVE_GRAPHICS, sizeof(TERM_LEAVE_GRAPHICS) - 1);
        g_initialised = 0;
    }
    if (g_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_termios);
        g_termios_saved = 0;
    }
}

/*
 * Query the terminal for its text area size in pixels using the XTWINOPS
 * "report text area size in pixels" request (CSI 14 t). The reply has the
 * form  CSI 4 ; <height> ; <width> t  and lets us learn the real cell size
 * even when TIOCGWINSZ reports zero pixels (common under ConPTY / Windows
 * Terminal).
 */
static void query_pixel_size(void)
{
    unsigned char buf[256];
    size_t len = 0;
    struct pollfd pfd;
    int i;

    if (g_pixels_queried) {
        return;
    }
    g_pixels_queried = 1;

    term_write("\x1b[16t", 5);

    for (i = 0; i < 50 && len < sizeof(buf) - 1; i++) {
        ssize_t n;
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 20) <= 0) {
            /* keep trying for a while even on timeouts */
            if (i > 5) {
                break;
            }
            continue;
        }
        n = read(STDIN_FILENO, buf + len, sizeof(buf) - 1 - len);
        if (n <= 0) {
            break;
        }
        len += (size_t)n;
        if (memchr(buf, 't', len) != NULL) {
            break;
        }
    }
    buf[len] = '\0';

    /* Parse  CSI 4 ; <ph> ; <pw> t  (height then width). */
    for (i = 0; i + 4 < (int)len; i++) {
        if (buf[i] == 0x1b && buf[i + 1] == '[') {
            int field = 0;
            int vals[8] = {0};
            int vi = 0;
            int j = i + 2;
            int ok = 0;
            while (j < (int)len && buf[j] != 't') {
                if (buf[j] == ';') {
                    if (vi < 8) {
                        vals[vi++] = field;
                    }
                    field = 0;
                } else if (buf[j] >= '0' && buf[j] <= '9') {
                    field = field * 10 + (buf[j] - '0');
                } else {
                    break;
                }
                j++;
            }
            if (j < (int)len && buf[j] == 't') {
                if (vi < 8) {
                    vals[vi++] = field;
                }
                ok = 1;
            }
            /* Expect: 6 ; cell_height ; cell_width */
            if (ok && vi >= 3 && vals[0] == 6) {
                int ch = vals[1];
                int cw = vals[2];
                if (cw >= 4 && cw <= 64 && ch >= 8 && ch <= 64) {
                    g_cell_w = cw;
                    g_cell_h = ch;
                }
            }
            break;
        }
    }

    /* Fallback: some terminals answer 14t (text area in pixels) but not 16t. */
    if (g_cell_w == 0) {
        unsigned char buf2[256];
        size_t len2 = 0;
        int i2;
        term_write("\x1b[14t", 5);
        for (i2 = 0; i2 < 10 && len2 < sizeof(buf2) - 1; i2++) {
            ssize_t n;
            struct pollfd pfd2;
            pfd2.fd = STDIN_FILENO;
            pfd2.events = POLLIN;
            pfd2.revents = 0;
            if (poll(&pfd2, 1, 20) <= 0) {
                if (i2 > 5) break;
                continue;
            }
            n = read(STDIN_FILENO, buf2 + len2, sizeof(buf2) - 1 - len2);
            if (n <= 0) break;
            len2 += (size_t)n;
            if (memchr(buf2, 't', len2) != NULL) break;
        }
        buf2[len2] = '\0';
        for (i2 = 0; i2 + 4 < (int)len2; i2++) {
            if (buf2[i2] == 0x1b && buf2[i2 + 1] == '[') {
                int field = 0, vals[8] = {0}, vi = 0, j = i2 + 2, ok = 0;
                while (j < (int)len2 && buf2[j] != 't') {
                    if (buf2[j] == ';') { if (vi < 8) vals[vi++] = field; field = 0; }
                    else if (buf2[j] >= '0' && buf2[j] <= '9') field = field * 10 + (buf2[j] - '0');
                    else break;
                    j++;
                }
                if (j < (int)len2 && buf2[j] == 't') { if (vi < 8) vals[vi++] = field; ok = 1; }
                if (ok && vi >= 3 && vals[0] == 4) {
                    struct winsize ws;
                    memset(&ws, 0, sizeof(ws));
                    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
                    if (ws.ws_col > 0 && ws.ws_row > 0) {
                        int cw = vals[2] / ws.ws_col;
                        int ch = vals[1] / ws.ws_row;
                        if (cw >= 4 && cw <= 64 && ch >= 8 && ch <= 64) {
                            g_cell_w = cw;
                            g_cell_h = ch;
                        }
                    }
                }
                break;
            }
        }
    }
}

int term_get_size(term_size_t *out)
{
    struct winsize ws;
    int have_size;

    memset(out, 0, sizeof(*out));

    /*
     * Zero the structure first: if the ioctl fails (stdout is not a
     * terminal, e.g. when redirecting to a file) the contents are
     * undefined and we must not look at them.
     */
    memset(&ws, 0, sizeof(ws));
    have_size = (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0);

    if (have_size && ws.ws_col > 0) {
        out->cols = ws.ws_col;
    } else {
        out->cols = 80;
    }
    if (have_size && ws.ws_row > 0) {
        out->rows = ws.ws_row;
    } else {
        out->rows = 24;
    }

    /*
     * Learn the real cell size if we don't know it yet. Prefer an explicit
     * XTWINOPS pixel-size query (needed for terminals such as Windows
     * Terminal / ConPTY that report 0 in TIOCGWINSZ), then TIOCGWINSZ's own
     * pixel fields, then the classic 8x16 fallback.
     */
    if (g_cell_w == 0 || g_cell_h == 0) {
        if (!g_pixels_queried) {
            query_pixel_size();
        }
        if (g_cell_w == 0 && have_size && ws.ws_xpixel > 0 && ws.ws_col > 0) {
            g_cell_w = ws.ws_xpixel / ws.ws_col;
        }
        if (g_cell_h == 0 && have_size && ws.ws_ypixel > 0 && ws.ws_row > 0) {
            g_cell_h = ws.ws_ypixel / ws.ws_row;
        }
        if (g_cell_w < 4) {
            g_cell_w = DEFAULT_CELL_W;
        }
        if (g_cell_h < 8) {
            g_cell_h = DEFAULT_CELL_H;
        }
    }

    out->cell_w = g_cell_w;
    out->cell_h = g_cell_h;
    if (out->cell_w < 1) {
        out->cell_w = DEFAULT_CELL_W;
    }
    if (out->cell_h < 1) {
        out->cell_h = DEFAULT_CELL_H;
    }

    out->pixel_w = out->cols * out->cell_w;
    out->pixel_h = out->rows * out->cell_h;

    return 0;
}

/*
 * Detect sixel support.
 *
 * There is no terminfo capability for sixel, so the primary device
 * attributes query (CSI c) is used: a terminal with sixel graphics
 * includes "4" in its parameter list, e.g.
 *
 *     ESC [ ? 62 ; 1 ; 4 ; 6 ; 8 ; 9 ; 15 ; 18 ; 21 ; 22 c
 *
 * Returns 1 (supported), 0 (not supported) or -1 (no answer).
 */
int term_detect_sixel(void)
{
    unsigned char buf[128];
    size_t len = 0;
    const char *term;
    size_t i;

    /* yaft is vt102 compatible and cannot answer DA1, but it does sixel. */
    term = getenv("TERM");
    if (term != NULL && strncmp(term, "yaft", 4) == 0) {
        return 1;
    }

    term_write("\x1b[c", 3);

    while (len < sizeof(buf) - 1) {
        struct pollfd pfd;
        ssize_t n;

        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;

        if (poll(&pfd, 1, 400) <= 0) {
            break;
        }
        n = read(STDIN_FILENO, buf + len, sizeof(buf) - 1 - len);
        if (n <= 0) {
            break;
        }
        len += (size_t)n;
        if (memchr(buf + len - (size_t)n, 'c', (size_t)n) != NULL) {
            break; /* final byte of the DA1 reply */
        }
    }
    buf[len] = '\0';

    if (len == 0) {
        return -1; /* the terminal did not answer */
    }

    for (i = 0; i < len; i++) {
        int value = 0;
        int have_digits = 0;

        if (buf[i] != '?') {
            continue;
        }
        for (i++; i < len; i++) {
            if (buf[i] >= '0' && buf[i] <= '9') {
                value = value * 10 + (buf[i] - '0');
                have_digits = 1;
                continue;
            }
            if (have_digits && value == 4) {
                return 1;
            }
            value = 0;
            have_digits = 0;
            if (buf[i] == 'c') {
                break;
            }
        }
        break;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Input decoding                                                     */
/* ------------------------------------------------------------------ */

#define KEY_BACKSPACE 0xFF08
#define KEY_TAB       0xFF09
#define KEY_RETURN    0xFF0D
#define KEY_ESCAPE    0xFF1B
#define KEY_HOME      0xFF50
#define KEY_LEFT      0xFF51
#define KEY_UP        0xFF52
#define KEY_RIGHT     0xFF53
#define KEY_DOWN      0xFF54
#define KEY_PAGE_UP   0xFF55
#define KEY_PAGE_DOWN 0xFF56
#define KEY_END       0xFF57
#define KEY_INSERT    0xFF63
#define KEY_DELETE    0xFFFF

/*
 * Map a final byte of a CSI sequence to a keysym.
 */
static uint32_t csi_final_to_keysym(char final_byte, const char *params)
{
    /* "1~" style (Home/End/Insert/Delete/PageUp/PageDown) */
    if (params[0] == '1' && params[1] == '~') {
        switch (params[0]) {
        case '1': return KEY_HOME;
        case '2': return KEY_INSERT;
        case '3': return KEY_DELETE;
        case '4': return KEY_END;
        case '5': return KEY_PAGE_UP;
        case '6': return KEY_PAGE_DOWN;
        default: break;
        }
    }
    switch (final_byte) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case 'Z': return 0xFF5B; /* Shift+Tab */
    case 'P': return 0xFFBE; /* F1 */
    case 'Q': return 0xFFBF; /* F2 */
    case 'R': return 0xFFC0; /* F3 */
    case 'S': return 0xFFC1; /* F4 */
    default: break;
    }
    return 0;
}

/*
 * Parse an SGR mouse report: ESC [ < b ; x ; y (M|m)
 */
static int parse_sgr_mouse(const unsigned char *buf, size_t len,
                           term_event_t *ev)
{
    size_t i = 2; /* skip ESC [ */
    int button = 0, x = 0, y = 0;
    int field = 0;
    int values[3] = {0, 0, 0};

    if (len < 4 || buf[2] != '<') {
        return 0;
    }
    i = 3;

    for (; i < len; i++) {
        if (buf[i] == ';') {
            if (field >= 3) {
                return 0;
            }
            values[field++] = button;
            button = 0;
            continue;
        }
        if (buf[i] >= '0' && buf[i] <= '9') {
            button = button * 10 + (buf[i] - '0');
            continue;
        }
        break;
    }
    if (i >= len) {
        return 0; /* incomplete */
    }
    if (field >= 3) {
        return 0;
    }
    values[field++] = button;

    if (buf[i] != 'M' && buf[i] != 'm') {
        return 0;
    }

    button = values[0];
    x = values[1];
    y = values[2];

    ev->kind = TERM_EV_MOUSE;
    ev->x = x;
    ev->y = y;
    ev->pressed = (buf[i] == 'M');

    if (button & 0x40) {
        /* wheel: 64 = up, 65 = down */
        ev->button_mask = (button & 1) ? 8 : 16;
    } else {
        switch (button & 0x03) {
        case 0: ev->button_mask = 1; break; /* left   */
        case 1: ev->button_mask = 2; break; /* middle */
        case 2: ev->button_mask = 4; break; /* right  */
        default: ev->button_mask = 0; break;
        }
    }
    if (!ev->pressed) {
        ev->button_mask = 0;
    }

    return (int)(i + 1);
}

/*
 * Try to decode one event from the front of g_inbuf.
 *
 * Returns the number of bytes consumed, 0 if more input is needed, or -1 on
 * an unrecoverable error.
 */
static int decode_event(term_event_t *ev)
{
    unsigned char c;

    if (g_inlen == 0) {
        return 0;
    }

    c = g_inbuf[0];

    if (c != 0x1b) {
        /* Plain key. Control characters map onto themselves, which matches
         * the X11 keysym values for Ctrl+A .. Ctrl+Z. Terminals send DEL
         * (0x7f) or BS (0x08) for Backspace, but VNC servers expect the
         * X11 keysym XK_BackSpace, so translate those explicitly. */
        ev->kind = TERM_EV_KEY;
        if (c == 0x7f || c == 0x08) {
            ev->keysym = KEY_BACKSPACE;
        } else {
            ev->keysym = (uint32_t)c;
        }
        return 1;
    }

    /* ESC ... */
    if (g_inlen == 1) {
        return 0; /* need more bytes to decide */
    }

    if (g_inbuf[1] == '[') {
        if (g_inlen >= 3 && g_inbuf[2] == '<') {
            return parse_sgr_mouse(g_inbuf, g_inlen, ev);
        }

        /* CSI: parameters, then a final byte in 0x40..0x7e. */
        {
            size_t i = 2;
            char params[32];
            size_t plen = 0;

            while (i < g_inlen && plen < sizeof(params) - 1) {
                unsigned char p = g_inbuf[i];
                if (p >= 0x30 && p <= 0x3f) {
                    params[plen++] = (char)p;
                    i++;
                    continue;
                }
                if (p >= 0x40 && p <= 0x7e) {
                    uint32_t ks;
                    params[plen] = '\0';
                    ks = csi_final_to_keysym((char)p, params);
                    if (ks == 0) {
                        /* Unknown sequence: swallow it. */
                        return (int)(i + 1);
                    }
                    ev->kind = TERM_EV_KEY;
                    ev->keysym = ks;
                    return (int)(i + 1);
                }
                /* Something unexpected; swallow the whole CSI. */
                return (int)(i + 1);
            }
            return 0; /* incomplete */
        }
    }

    if (g_inbuf[1] == 'O') {
        /* SS3: F1..F4 and friends. */
        if (g_inlen < 3) {
            return 0;
        }
        switch (g_inbuf[2]) {
        case 'P': ev->keysym = 0xFFBE; break; /* F1 */
        case 'Q': ev->keysym = 0xFFBF; break; /* F2 */
        case 'R': ev->keysym = 0xFFC0; break; /* F3 */
        case 'S': ev->keysym = 0xFFC1; break; /* F4 */
        case 'A': ev->keysym = KEY_UP; break;
        case 'B': ev->keysym = KEY_DOWN; break;
        case 'C': ev->keysym = KEY_RIGHT; break;
        case 'D': ev->keysym = KEY_LEFT; break;
        case 'H': ev->keysym = KEY_HOME; break;
        case 'F': ev->keysym = KEY_END; break;
        default: return 3; /* swallow */
        }
        ev->kind = TERM_EV_KEY;
        return 3;
    }

    /* ESC followed by something else: treat ESC as the Escape key and let
     * the next byte be decoded separately. */
    ev->kind = TERM_EV_KEY;
    ev->keysym = KEY_ESCAPE;
    return 1;
}

static int fill_input(void)
{
    struct pollfd pfd;
    ssize_t n;

    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    pfd.revents = 0;

    if (poll(&pfd, 1, 0) <= 0) {
        return 0;
    }
    if (g_inlen >= sizeof(g_inbuf)) {
        return 0;
    }

    n = read(STDIN_FILENO, g_inbuf + g_inlen, sizeof(g_inbuf) - g_inlen);
    if (n <= 0) {
        return 0;
    }
    g_inlen += (size_t)n;
    return 1;
}

int term_poll_event(term_event_t *ev)
{
    int consumed;

    memset(ev, 0, sizeof(*ev));

    for (;;) {
        consumed = decode_event(ev);
        if (consumed > 0) {
            memmove(g_inbuf, g_inbuf + consumed, g_inlen - (size_t)consumed);
            g_inlen -= (size_t)consumed;
            return 1;
        }
        if (consumed < 0) {
            g_inlen = 0;
            return 0;
        }

        /* Need more data. */
        if (fill_input()) {
            continue;
        }

        /*
         * No more input available right now. If the buffer holds a lone
         * ESC, it was a real Escape key press.
         */
        if (g_inlen == 1 && g_inbuf[0] == 0x1b) {
            ev->kind = TERM_EV_KEY;
            ev->keysym = KEY_ESCAPE;
            g_inlen = 0;
            return 1;
        }
        return 0;
    }
}
