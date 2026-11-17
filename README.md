# vnt

> **Proof of concept for educational purposes only.** This project is AI-coded
> and may contain bugs. It is **not** intended for production use.

A VNC client that renders the remote desktop in the terminal using **SIXEL
graphics**. The whole pipeline is:

```
remote desktop
   │  RFB
   ▼
libvncclient  →  framebuffer (16/32 bpp)
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

One line Docker command for testing in a sandbox:
```
docker run -it --rm ubuntu sh -c 'export DEBIAN_FRONTEND=noninteractive; apt-get update && apt-get install -y --no-install-recommends git ca-certificates build-essential pkg-config libvncserver-dev libsixel-dev zlib1g-dev libjpeg-dev && git clone https://github.com/emchan/vnt /vnt && cd /vnt && make -j"$(nproc)" && exec bash'

./vnt <hostname>
```

Standard Linux build:

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
sudo make install                      # installs vnt to /usr/local/bin
make install BINDIR=$HOME/.local/bin   # alternative destination
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
      --no-menu           do not draw the status bar at the bottom
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
  -U, --user NAME        user name for servers that need VeNCrypt/MS-Logon/
                         ARD credentials (the password is prompted for)
      --server-format    also accept the server's native 16bpp format
                         (native 32bpp is already preferred automatically)
      --safe-mode        debugging profile: raw encoding, server-native pixel
                         format, JPEG/compression disabled
      --force-rgbx       keep the requested 32bpp RGBX format instead of the
                         server's native format (for A/B testing)
      --no-jpeg          keep Tight encoding but disable its JPEG mode
      --version          print build and library versions
  -V, --verbose          print scaling, connection and negotiation details
```

### Connection troubleshooting

`-V` turns on a full transcript of the RFB negotiation. libvncclient reports
the protocol version, every security type offered by the server, which one
was selected, and whether authentication succeeded. `vnt` adds the negotiated
security scheme, the server and client pixel formats, and the encodings that
were requested, so the failing step is visible.

Typical failures:

* **`Unable to connect to VNC server`** before any protocol line: the TCP
  connection failed. Check the host/port, DNS, and firewall.
* **A protocol-version line but no selected security type**: the server only
  offers a security type this client cannot use (or offers none). Look at the
  `Received security type` lines.
* **`GetCredential callback is not set`**: the server only offers
  VeNCrypt/MS-Logon/Apple Remote Desktop, which need a user name as well as a
  password. Retry with `--user NAME`.
* **Authentication failure after the password prompt**: the password (or
  user name) was rejected. Check credentials and the server's authentication
  method. The client prints hints after a failed `rfbInitClient()` call.
* **X.509 credentials requested**: this client does not implement client
  certificates; use a server that offers VNC/VeNCrypt/Plain authentication.
* **The image is black**: `vnt` now checks completed frames and reports whether
  the framebuffer is effectively black. It also advertises the classic
  `NewFBSize` pseudo-encoding, which keeps TightVNC off its full-screen
  fallback path, and forces a full refresh up to twice, because a corrupted
  first frame would otherwise stay black until the desktop changes. If the
  frame is still black it retries Tight with JPEG disabled, then falls back to
  raw encoding. If the warning persists, try `-e raw` to bypass the
  Tight/ZRLE decoders and `--dump-ppm frame.ppm` to inspect what the server
  actually sent. `-V` also logs the server's native pixel format
  and raw pixel samples. The warning after the retries states which of the
  two cases applies:
* **Converted frame black, raw data present**: the client is reading the
  server's bytes with the wrong format. The client already prefers a native
  32bpp format; `--server-format` additionally accepts a native 16bpp format.
  If it is still black, send the `-V` format/raw-pixel lines.
* **Converted frame black, raw data effectively empty**: the server is sending
  a blank desktop. Common causes are a locked session, TightVNC attached to a
  disconnected RDP/service session, Desktop Duplication returning black
  frames, or a headless machine with no monitor/display attached. Check the
  TightVNC server log and its session/desktop-duplication settings, and try
  another VNC viewer from the same client machine to confirm it is server-side.
* **Works on one architecture but is black on another**: RFB itself does not
  depend on the CPU, so compare the builds and terminals. Run `--version` on
  both (it prints the build date, byte order and libvncclient headers),
  `--check` on the failing one, and compare the `-V` format/raw-pixel lines.
  A stale binary, a different libvncclient/libsixel build, and a terminal
  without sixel support are the usual causes. `--force-rgbx` and
  `--safe-mode` isolate the pixel-format and encoding paths.

For a minimal, conservative negotiation use `--safe-mode`:

```sh
./vnt -V --safe-mode <host>:5900
```

This forces raw encoding, requests the server's native pixel format, and
disables JPEG/compression. It does not force RFB 3.3 (libvncclient does not
expose a protocol-version override), but it removes the Tight/ZRLE decoders,
format translation, and JPEG from the equation.

Collect the transcript with:

```sh
./vnt -V host:5900 2>vnt-negotiation.log
```

Keyboard and mouse input are forwarded to the remote desktop. Mouse events
are mapped from terminal cells back into remote framebuffer coordinates
through the same scale transform that is used for the image, so they land on
the pixel under the pointer. Mouse reporting uses the SGR extended mode so
that coordinates larger than 223 work.

The bottom row is a status bar showing the connected host, the network
bandwidth in use (and total received), the encoding requested from the
server, and the current frame rate. Click **[ Refresh ]** to request a full
non-incremental framebuffer update, or **[ Quit ^Q ]** to disconnect. The
same actions are available from the keyboard with `Ctrl-Q`; `Ctrl-C` is
forwarded to the remote desktop, so it works as copy in applications that
use it. Use `--no-menu` to hide the bar (the image then uses the full
terminal height).

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

`rfbGetClient(8, 3, 4)` gives a 32 bpp, little endian R/G/B/X fallback. The
`MallocFrameBuffer` hook runs after `ServerInit` and before the pixel format
is sent, so the client adopts the server's native 32 bpp format when it has
one (as other viewers do). This avoids asking the server to translate every
rectangle, which some servers mishandle. For 16 bpp natives the client keeps
its 32 bpp request unless `--server-format` is given. `rfbInitClient()` sends
the format and encodings itself; they are deliberately not sent a second time
while the first update is in flight. The conversion from the framebuffer to
RGB24 is a byte drop for R/G/B/X, with a generic fallback using the
`redShift`/`redMax` fields for every other layout.

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
pseudo encoding (a NewFBSize/ExtDesktopSize marker) with no pixels at all. The
client therefore tracks which *rows* of the framebuffer have been received and
only draws when the frame is complete (or when the server went quiet for 200 ms,
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

## TightVNC / Windows black-screen fix

A 1920x1080 Windows desktop served by TightVNC connected but rendered black,
while the same server worked in other VNC viewers. `-V` showed that TCP, RFB
3.8 and authentication all completed, so the failure was in the pixel path.
Because other viewers worked against the same server, the difference was
client-side negotiation:

* **The format/encodings message was sent twice.** `rfbInitClient()` already
  calls `SetFormatAndEncodings()` and queues the first framebuffer update. The
  client then sent it again while that update was in flight. TightVNC can
  reset its encoder state at that point, corrupting the first frame; on a
  static desktop no later full frame arrives to repair it. The duplicate call
  has been removed.
* **`NewFBSize` was not advertised.** libvncclient announces `ExtDesktopSize`
  (`-308`) by default, but TightVNC only understands the classic `NewFBSize`
  (`-223`). Without it, TightVNC treats every full-screen request as a
  desktop-resize-disabled case and composites the desktop through a temporary
  blank framebuffer. The client now sets `canHandleNewFBSize = TRUE`.
* **32bpp RGBX was forced.** TightVNC's native format is 32bpp BGRX
  (`redShift=16`, `greenShift=8`, `blueShift=0`), while the client requested
  RGBX (`0/8/16`), forcing server-side translation. The `MallocFrameBuffer`
  hook now runs after ServerInit and adopts the server's native 32bpp format.
  `--server-format` additionally accepts a native 16bpp format.

Diagnostics added while tracking this down:

* `-V` captures and prefixes libvncclient's full handshake transcript and
  reports the negotiated protocol version, security scheme, server/client
  pixel formats, encodings, and the last libvncclient error.
* Completed frames are classified as effectively black (less than 0.1%
  non-black pixels). The client requests up to two full refreshes, retries
  Tight with JPEG disabled, and finally falls back to raw encoding before
  reporting whether the raw framebuffer contained data or was effectively
  empty.
* `-V` logs the server's native pixel format and raw pixel samples from
  several points in the framebuffer, distinguishing a format-conversion bug
  from a server that is actually sending black pixels.
* `--safe-mode` forces raw encoding and the server-native pixel format and
  disables JPEG/compression, for a minimal negotiation.
* `--dump-ppm` writes the converted framebuffer and `--dump` writes the sixel
  stream, so the received image and the rendered output can be compared
  offline.

See [Connection troubleshooting](#connection-troubleshooting) for how to read
the black-frame warnings.

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
