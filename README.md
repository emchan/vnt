# vnt

> **Proof of concept for educational purposes only.** This project is AI-coded
> and may contain bugs. It is **not** intended for production use.

A VNC client that renders the remote desktop in the terminal using **SIXEL
graphics**. The whole pipeline is:

```
remote desktop
   │  RFB
   ▼
libvncclient  →  32 bpp framebuffer
                      │
                      ▼
              fb_to_rgb24()  (any rfbPixelFormat -> RGB24)
                      │
                      ▼
              scale_rgb24()  (box filter down, bilinear up)
                      │
                      ▼
              libsixel       (fixed 256 colour palette + sixel DCS)
                      │
                      ▼
                  terminal
```

It connects with libvncclient (raw / copyrect / hextile / zlib / ZRLE /
tight), converts the framebuffer to RGB24, fits it into the terminal's pixel
area, and emits a SIXEL image per frame. The terminal cursor is hidden and
sixel display mode (DECSDM) is enabled so that every frame lands at the top
left corner without scrolling.

## Quick start

```sh
make
./vnt --check          # verify sixel support
./vnt host:5900        # connect
```

On Windows Terminal / ConPTY, if the image does not fill the grid, override the
cell size and stretch:

```sh
./vnt --cell-size 9,20 --stretch --debug-mouse host:5900
```

## Building

Dependencies: libvncclient, libsixel, zlib, libjpeg (for tight encoding).

```sh
make
make PREFIX=/usr/local   # for a system install
```

```sh
make
make test          # run the end to end test suite
```

## Usage

```
vnt [options] host[:port]
  -e, --encodings LIST   RFB encodings to request
                          (default: "tight zrle hextile raw copyrect")
  -c, --colors N         palette size, 2..256 (default: 256)
  -f, --fps N            maximum frames per second (default: 30)
  -q, --quality POLICY   sixel encode policy: auto|fast|size (default: size)
  -r, --refresh N        re-derive the palette every N frames (default: 0)
  -u, --upscale          scale up small remote desktops to fill the terminal
      --stretch           scale the image to fill the terminal (distorts aspect
                          ratio; simplifies mouse mapping)
      --center            centre the image in the terminal instead of
                          top-left aligning it
  -v, --view-only        do not send keyboard or mouse events
      --tmux             wrap the sixel output for tmux passthrough
      --full-palette     re-send the colour palette every frame (default)
      --body-only        reuse the palette across frames (smaller output,
                         requires a terminal with a shared colour register bank)
      --dump FILE        write the sixel stream to FILE instead of the terminal
      --dump-ppm FILE    write the received framebuffer to FILE as a PPM
      --frames N         exit after N complete frames (for testing)
      --check            report whether the terminal supports sixel and exit
      --debug-mouse       print mouse coordinate mapping to stderr
      --draw-cursor       draw crosshairs on the image (default)
      --no-cursor         disable the crosshairs
      --mouse-offset X,Y  manual calibration offset in terminal cells
      --cell-size W,H     override the terminal cell size in pixels
      --calibrate         draw a green grid + red/blue markers to verify image
                          position
  -V, --verbose          print scaling information
```

Keyboard and mouse input are forwarded to the remote desktop. Mouse events
are mapped from terminal cells back into remote framebuffer coordinates
through the same scale transform that is used for the image, so they land on
the pixel under the pointer. Mouse reporting uses the SGR extended mode so
that coordinates larger than 223 work.

A local cursor is shown at the last mouse position so the user can see
where clicks will land. By default two crosshairs are drawn on the image:

* **red** — the mouse click location (terminal cell → image pixel)
* **yellow** — the VNC cursor location (remote framebuffer → image pixel)

This makes it immediately visible whether the coordinate mapping is correct:
if the red and yellow crosshairs coincide, the mapping is right; if they
are offset, the offset is in the mapping. Use `--debug-mouse` to print the
full coordinate mapping to stderr. Use `--no-cursor` to disable.

## Mouse calibration

Mouse reporting in a terminal is cell-granular, and the displayed image is
usually aspect-fit into the terminal, so the mapping from a cell to a remote
pixel is not always obvious. To make it verifiable:

* `--draw-cursor` (on by default) draws a **red** crosshair at the mouse
  click location and a **yellow** crosshair at the VNC cursor location
  directly on the image. If they coincide, the mapping is correct.
* `--debug-mouse` prints `cell → px → img → fb → remote` to stderr.
* `--calibrate` overlays a green grid at terminal cell boundaries plus a red
  square at image (0,0) and a blue square at the bottom-right, so you can
  confirm the image is where the code thinks it is.
* `--mouse-offset X,Y` shifts the reported cell by a constant (for
  terminals whose mouse origin is shifted, e.g. by a border).
* `--cell-size W,H` overrides the cell size in pixels (needed on ConPTY /
  Windows Terminal when `TIOCGWINSZ` reports zero pixels).
* `--stretch` scales the image to fill the whole terminal, removing
  letterboxing and making the mapping 1:1.

## Why this is non-trivial

The hard parts are all in the wiring between the two libraries, not in the
libraries themselves:

### 1. Pixel format

`rfbGetClient(8, 3, 4)` already gives the layout we want (32 bpp, depth 24,
little endian, R/G/B/X bytes in memory). The client re-asserts it with
`SetFormatAndEncodings()` so that the server converts to it. The conversion
from the framebuffer to RGB24 is then a straight byte drop, with a generic
fallback using the `redShift`/`redMax` fields so that it keeps working if a
server forces a different format.

### 2. Sixel palette reuse

libsixel's `sixel_dither_initialize()` builds a colour palette from a pixel
buffer and marks the dither as "optimised". From then on `sixel_encode()`
does **not** recompute the palette, so every frame is quantised against the
same colour registers. Because the palette no longer changes, we can set
"body only" mode (`sixel_dither_set_body_only(dither, 1)`) after the first
frame: this suppresses the `#Pc;2;R;G;B` colour register definitions, which
for a 256 colour palette is roughly 3.3 KB of pure overhead per frame.

This requires the terminal to use a **shared** colour register palette rather
than a private one per graphic. The client sends `DECRST 1070` (`ESC [ ? 1070 l`)
for exactly that reason: foot, for example, uses a private palette by default
and would otherwise force us to re-send the palette every frame.

### 3. Region tracking

libvncclient delivers a full screen refresh as several update messages (the
server's update buffer is finite), and the first message may contain only a
pseudo encoding (the ExtDesktopSize marker) with no pixels at all. The client
therefore tracks which *rows* of the framebuffer have been received and only
draws when the frame is complete (or when the server went quiet for 200 ms,
so that a partial frame is still shown). `FinishedFrameBufferUpdate` is
deliberately **not** used as the trigger: it fires for pseudo-encoding only
messages too.

### 4. Terminal sizing

The terminal's pixel area is derived from `TIOCGWINSZ` (columns/rows plus
the optional `ws_xpixel/ws_ypixel`). Most terminals report 0 for the pixel
size, so the client falls back to 8x16 cells. The remote desktop is then
aspect-fit into that area (box filter down, bilinear up) and the same scale
transform is inverted for mouse coordinates.

Window resizes are caught both via `SIGWINCH` and a polling fallback (since
`SIGWINCH` is not always delivered, e.g. when the process is not in the
foreground process group of the pty). On resize the screen is cleared and the
image is re-encoded at the new size.

### 5. Sixel terminal facts

* The DCS envelope is `\x1bP` ... `\x1b\\` (7-bit) with `q"1;1;W;H`
  raster attributes, which forces square pixels. xterm and foot honour
  this; wezterm does not, so a wezterm user should use `--upscale` or
  pre-scale.
* `\x1b[?80h` (DECSDM set) pins the image to the top left corner and
  leaves the text cursor untouched - exactly what a full screen viewer
  wants.
* `\x1b[2J` is the portable "wipe the slate" for sixels; `CSI 3 J` does
  **not** clear them.
* There is no terminfo capability for sixel. Detection uses the primary
  device attributes query (`CSI c`) and looks for "4" in the parameter
  list. `--check` reports the result.
* kitty does **not** support sixel - it uses its own kitty graphics
  protocol. The client ships a separate `--tmux` mode for passthrough.

## License

MIT. See `LICENSE`. Note that the linked libraries (libvncclient, libsixel)
carry their own licenses; distributed binaries are subject to those terms.

## Files

```
src/vncsixel.c     main client, event loop, scaling + sixel orchestration
src/scaler.c       rfbPixelFormat -> RGB24, box/bilinear scaling
src/sixel_out.c    libsixel wrapper with a stable palette across frames
src/terminal.c     raw mode, size, key/mouse decoding, sixel detection
tests/vnctestserver.c   headless VNC server used for testing
tests/run_e2e.sh        end to end test runner
tests/verify_*.py       test cases
```

## Test suite

`make test` runs:

* `verify_pipeline.py` - decodes a sixel produced by the client with
  libsixel's own `sixel2png` and compares it against an independent
  reference downscale of the received framebuffer (correlation 0.994,
  mean error 3.97/255).
* `verify_terminal.py` - drives the client through a pseudo terminal and
  checks the alternate screen, cursor, DECSDM, shared palette, mouse
  reporting and restore sequences.
* `verify_interactive.py` - live frames differ, and keyboard and mouse
  events reach the server with correctly mapped coordinates.
* `verify_detect.py` - the sixel capability probe.
