/*
 * terminal.h - terminal plumbing for the sixel VNC viewer.
 *
 * Responsibilities:
 *   - put the terminal into raw mode and into a known graphic state
 *   - report the size of the drawable area in pixels
 *   - decode keyboard and mouse input into events
 */
#ifndef VDCSX_TERMINAL_H
#define VDCSX_TERMINAL_H

#include <stdint.h>

/* Terminal size, in cells and in pixels. */
typedef struct {
    int cols;
    int rows;
    int cell_w;   /* cell width in pixels (estimated if unknown) */
    int cell_h;   /* cell height in pixels (estimated if unknown) */
    int pixel_w;  /* usable width in pixels  */
    int pixel_h;  /* usable height in pixels */
} term_size_t;

typedef enum {
    TERM_EV_NONE = 0,
    TERM_EV_KEY,
    TERM_EV_MOUSE
} term_event_kind_t;

typedef struct {
    term_event_kind_t kind;
    uint32_t keysym;   /* X11 keysym for TERM_EV_KEY */
    int x, y;          /* mouse position, 1-based cell coordinates */
    int button_mask;   /* VNC button mask */
    int pressed;       /* 1 = press, 0 = release */
} term_event_t;

/*
 * Enter raw mode, switch to the alternate screen, hide the cursor and put
 * the terminal into sixel display mode. Returns 0 on success.
 */
int term_init(int enable_mouse);

/* Undo everything term_init() did. Safe to call when term_init() failed. */
void term_restore(void);

/* Query the terminal size. Returns 0 on success. */
int term_get_size(term_size_t *out);

/*
 * Override the cell size (in pixels) used for all subsequent size queries.
 * Useful when the terminal does not report a usable pixel size (e.g. under
 * ConPTY / Windows Terminal). Pass 0 for either value to leave it unchanged.
 */
void term_set_cell_size(int w, int h);

/*
 * Detect whether the terminal supports SIXEL graphics.
 *
 * There is no terminfo capability for this. The only portable probe is the
 * primary device attributes query (CSI c): a terminal with sixel graphics
 * includes "4" in its parameter list.
 *
 * Returns 1 if sixel is supported, 0 if it is not, -1 if the terminal did
 * not answer the query (in which case it may still support sixel).
 */
int term_detect_sixel(void);

/*
 * Read at most one pending input event. Returns 1 if an event was produced,
 * 0 if no event was pending.
 */
int term_poll_event(term_event_t *ev);

/* Write raw bytes to the terminal. */
void term_write(const char *data, size_t len);

#endif /* VDCSX_TERMINAL_H */
