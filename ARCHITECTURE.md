# vncsixel — architecture notes

This document describes how `vncsixel` is built, why it is built the way it
is, and the problems that were actually hit while constructing it. It is a
companion to `README.md` (which is the user-facing description).

## 1. What the program does

`vncsixel` is a VNC client whose framebuffer is rendered into the terminal
using DEC SIXEL graphics. The data flow is:

```
remote desktop
   │  RFB (raw / copyrect / hextile / zlib / ZRLE / tight)
   ▼
libvncclient  →  32 bpp framebuffer (client->frameBuffer)
   │
   ▼
fb_to_rgb24()  (any rfbPixelFormat → tightly packed RGB24)
   │
   ▼
scale_rgb24()  (aspect-fit into the terminal pixel area)
   │
   ▼
sixel_sink_frame()  (libsixel: fixed 256-colour palette + SIXEL body)
   │
   ▼
terminal (alternate screen, cursor hidden, DECSDM, shared palette)
```

The terminal is put into raw mode, the alternate screen buffer, sixel
display mode (DECSDM) and shared colour register mode. On exit all of that is
undone. Keyboard and mouse input are forwarded to the remote desktop.

## 2. Repository layout

```
src/
  vncsixel.c    main client, event loop, input, rendering orchestration
  scaler.c      rfbPixelFormat -> RGB24, box/bilinear scaling
  sixel_out.c   libsixel wrapper (stable palette, body-only, tmux passthrough)
  terminal.c    raw mode, size, key/mouse decoding, sixel detection
  *.h           public headers
tests/
  vnctestserver.c         headless VNC server (animated test pattern)
  run_e2e.sh              test runner
  verify_pipeline.py      sixel decode round trip vs. reference downscale
  verify_terminal.py      escape sequences via a pseudo terminal
  verify_interactive.py   live frames + input forwarding
  verify_detect.py        sixel capability probe
Makefile
README.md
ARCHITECTURE.md
```

## 3. Module responsibilities

### 3.1 `scaler.c` — format conversion and resampling

`fb_to_rgb24()` converts a libvncclient framebuffer into tightly packed
RGB24. It has a fast path for the 32bpp little-endian, R/G/B/X layout that
we request from every server (just drop the fourth byte), and a generic
fallback that uses the `*Shift`/`*Max` fields of `rfbPixelFormat`. The
fallback is what keeps the client working if a server forces a different
format.

`scale_rgb24()` aspect-fits an RGB24 image into a target size. Downscaling
uses a box filter (averaging blocks of source pixels) because point sampling
destroys small text; upscaling uses bilinear interpolation.

### 3.2 `sixel_out.c` — libsixel wrapper

The interesting part of driving libsixel for a live stream is palette
handling. `sixel_dither_initialize()` builds a palette from a pixel buffer and
marks the dither as "optimised". From then on `sixel_encode()` does **not**
recompute the palette, so every frame is quantised against the same colour
registers. Because the palette is now stable, we can set "body only" mode
after the first frame, which suppresses the `#Pc;2;R;G;B` colour register
definitions (about 3.3 KB per 256-colour frame).

The output context uses a caller-supplied write callback, so we can either
write straight to stdout or wrap every chunk in a tmux passthrough DCS
(`ESC P tmux; <payload with doubled ESC> ESC \`). That wrapping is done in
C rather than relying on libsixel's own `penetrate_multiplexer` mode, which
in 1.8.7-r2 emits plain DCS envelopes without the `tmux;` prefix.

### 3.3 `terminal.c` — terminal plumbing

* `term_init()` switches to raw mode, the alternate screen, hides the cursor,
  enables DECSDM (`ESC [ ? 80 h`), requests the shared colour palette
  (`ESC [ ? 1070 l`), disables auto-wrap (`ESC [ ? 7 l`, so the bottom status
  bar cannot scroll the screen) and (optionally) SGR mouse reporting.
* `term_get_size()` derives the drawable pixel area from `TIOCGWINSZ`, falling
  back to 8x16 cells when the terminal reports no pixel size (the common case).
* `term_poll_event()` decodes keyboard and SGR mouse events from a persistent
  input buffer, handling partial escape sequences.
* `term_detect_sixel()` probes sixel support via the primary device attributes
  query (`CSI c`) and looks for "4" in the parameter list. There is no
  terminfo capability for sixel.

### 3.4 `vncsixel.c` — the client

The client uses libvncclient's default handlers (`GotBitmap = CopyRectangle`,
etc.) to fill `client->frameBuffer`. It overrides `GotFrameBufferUpdate`
(to track coverage) and `FinishedFrameBufferUpdate` (intentionally a no-op,
because it also fires for pseudo-encoding-only messages).

The event loop waits on `WaitForMessage`, handles RFB messages, polls
terminal input and forwards it, and draws a frame when the row coverage is
complete (or when the server went quiet for 200 ms).

The bottom terminal row is reserved for a status bar. `render()` sizes the
image to fit above it, and `draw_menu()` redraws the bar (host, bandwidth,
encoding, fps, Refresh and Quit buttons) every frame and twice a second while
idle. Mouse events on that row are intercepted and never reach the server;
`Ctrl-Q` disconnects while `Ctrl-C` is forwarded to the remote desktop.

## 4. How it was constructed

The work proceeded in stages, each one verified before moving on:

1. **Research.** Established that no existing VNC→SIXEL client exists
   (GitHub search returns zero repos; the libsixel maintainer said in 2019
   "as far as I know it does not exist"). The closest analogues (farsee,
   waytermirror, mpv's `vo_sixel`) were read to learn the layout/encoding
   patterns.
2. **Build the libraries.** libvncserver (0.9.15) and libsixel (1.8.7-r2)
   were built from source into the configured PREFIX, along with zlib and
   libjpeg-turbo, because the system only had runtime libraries, no headers.
3. **A headless test server.** `vnctestserver.c` uses libvncserver's server
   API with an animated framebuffer, so the whole pipeline can be exercised
   without an X server.
4. **The client, one stage at a time.** Format conversion, scaling, sixel
   encoding, terminal control, input forwarding — each added and tested
   before the next.
5. **End-to-end verification.** A Python test decodes the client's sixel
   stream with libsixel's own `sixel2png` and compares it against an
   independent box-filter downscale of the received framebuffer.

## 5. Issues worked through

### 5.1 Rendering happened before any pixels arrived

The first symptom was a completely black sixel. The initial framebuffer
update message from the server contains only the `ExtDesktopSize`
pseudo-encoding (which libvncclient `continue`s over, so it never reaches
`GotFrameBufferUpdate`), yet `FinishedFrameBufferUpdate` fires for it. I had
initially keyed rendering off `FinishedFrameBufferUpdate`, so the first
render ran on an empty framebuffer. Fix: key rendering off `GotFrameBufferUpdate`,
which only fires for real pixel rectangles.

### 5.2 A full screen refresh is several messages

Even after fixing the trigger, rendering on every update produced partial
frames because the server splits the screen across several messages (the
update buffer is finite). Fix: track which rows of the framebuffer have been
received with a per-row bitmap; only count a frame as complete when all rows
have arrived, and also draw when the server has been quiet for 200 ms so a
partial frame is still shown.

### 5.3 Uninitialised `winsize`

`term_get_size()` called `ioctl(TIOCGWINSZ, ...)` without zeroing the
structure first. When stdout was redirected to a file the ioctl failed but
`ws.ws_xpixel` contained stack garbage, producing cell sizes of 32, 8 or 1
pixels depending on the run. Fix: zero the structure and gate every field on
the ioctl's return value.

### 5.4 The extra `SetFormatAndEncodings()` call

The client was calling `SetFormatAndEncodings()` a second time after
`rfbInitClient()`. The server then applied the format twice and the
`ExtDesktopSize` pseudo-encoding was re-enabled, which is why "Switching from
raw to raw" appeared in the server log. The second call was removed; the
format set on the `rfbClient` before `rfbInitClient()` is sufficient.

### 5.5 `rfbInitClient` double-frees on failure

`rfbInitClient()` calls `rfbClientCleanup()` itself when it fails, so the
client was freeing the client structure twice and segfaulting. The redundant
cleanup call was removed.

### 5.6 Mouse coordinates were mapped to the wrong image size

The image is only scaled when `--upscale` is set, so for a small remote
desktop the displayed image is 1:1. The mapping code initially assumed the
image had been scaled, which produced off-by-a-scale-factor coordinates.
Fix: compute the mapping from the actual `img_w`/`img_h` that were published
before encoding, and publish them before the sixel bytes hit the terminal
(which is when a mouse event can arrive).

### 5.7 tmux passthrough

libsixel 1.8.7-r2's `penetrate_multiplexer` mode splits the stream into
plain DCS envelopes without the `tmux;` prefix that tmux expects. The
wrapping is done in the client's write callback instead: each chunk becomes
`ESC P tmux; <chunk with doubled ESC> ESC \`.

### 5.8 `fwrite` length

The tmux wrapper's first `fwrite` was given a length of 6 for the 7-byte
string `ESC P tmux;`, dropping the semicolon. Corrected to 7.

### 5.9 `ensure_buffers` confusion

The scaled buffer was sized to the framebuffer size rather than the target
image size, over-allocating memory and leaving `scaled_w`/`scaled_h` stale.
Rewritten to take both sizes.

## 6. Test suite and test cases

The test suite lives in `tests/` and is driven by `make test` (which calls
`tests/run_e2e.sh`). Every test case below is fully automated and runs in a
few seconds. They exist specifically so that regressions in the wiring
between libvncclient and libsixel are caught quickly.

### 6.1 `tests/vnctestserver.c` — the test fixture

A headless VNC server built on libvncserver's server API. It does not need
an X server. It renders an animated test pattern into a 32-bit framebuffer:

* a horizontal gradient background
* 8 colour bars along the top
* a moving orange box (so consecutive frames always differ)
* a grid
* a frame counter drawn with a 3x5 bitmap font

It exposes `kbdAddEvent` and `ptrAddEvent` hooks that log every keyboard and
pointer event to stderr, which the interactive test asserts against. It can
also dump its own framebuffer as a PPM (`-P`) for comparison.

### 6.2 `verify_pipeline.py` — end to end round trip

**What it checks:** the whole pipeline produces a SIXEL stream that decodes
back to the correct image.

**How:**
1. Start `vnctestserver` (640x480, 10 fps).
2. Run `vncsixel -e raw --dump out.sixel --dump-ppm recv.ppm --frames 1`.
3. Decode `out.sixel` with libsixel's own `sixel2png` → `out.png`.
4. Independently box-filter `recv.ppm` down to the sixel's dimensions using
   a Python reimplementation of the C box filter (no shared code with the
   client).
5. Compare the decoded PNG against the reference downscale.

**Assertions:** decoded size is 512x384; the image is not blank
(`std > 10`); correlation with the reference is > 0.95; mean absolute error
< 20/255.

**Result:** 512x384, mean error 3.97/255, max 126/255 (quantisation to
256 registers + Floyd-Steinberg), correlation 0.9941. The residual is pure
colour quantisation noise, which is expected.

### 6.3 `verify_terminal.py` — terminal control sequences

**What it checks:** the client emits the correct escape sequences and restores
the terminal on exit.

**How:** run the client through a pseudo terminal with a window size of
80x24 cells (8x16 pixels each) and capture the raw bytes.

**Assertions:** the terminal control sequences must be present:
* `ESC [ ? 1049 h` (alternate screen on)
* `ESC [ ? 25 l` (cursor hidden)
* `ESC [ ? 80 h` (sixel display mode — image at top left, cursor unmoved)
* `ESC [ ? 1070 l` (shared colour registers — required for body-only)
* `ESC [ ? 7 l` (auto-wrap disabled for the status bar)
* `ESC [ ? 1002 h` (mouse tracking on)
* `ESC P q` (SIXEL DCS start)
* `ESC \ ` (SIXEL DCS terminator)
* `ESC [ ? 1002 l`, `ESC [ ? 1006 l`, `ESC [ ? 1000 l` (mouse tracking off)
* `ESC [ ? 7 h`, `ESC [ ? 80 l`, `ESC [ ? 25 h`, `ESC [ ? 1049 l` (restore)
* the status bar labels `[ Refresh ]` and `[ Quit ^Q ]`
* raster attributes `ESC P q "1;1;W;H` (square pixels)
* at least one sixel frame emitted

### 6.4 `verify_interactive.py` — live updates and input forwarding

**What it checks:** the client keeps redrawing when the remote desktop
animates, and keyboard/mouse input reaches the server with correct
coordinates.

**How:**
1. Start `vnctestserver` (640x480, 10 fps, animated).
2. Run the client through a pseudo terminal.
3. Wait for several sixel frames, then send a mouse press (SGR `ESC [ < 0 ; 10 ; 5 M`),
   the `a` and Backspace keys, and `Ctrl-C`, then click the Refresh and Quit
   buttons in the bottom status bar.
4. Parse the server's stderr for the logged events.

**Assertions:**
* at least 3 sixel frames received
* at least 2 distinct frames (the desktop animates)
* the server logged `key down keysym=0x61`
* the server logged `key down keysym=0xff08` (Backspace remapped)
* the server logged `key down keysym=0x3` (Ctrl-C forwarded, not a quit)
* the Quit button exits the client
* the server logged `pointer mask=1 at (99,93)` — cell (10,5) of an
  80x24 terminal (8x16 cells) with one row reserved for the status bar is
  image pixel (76,72), which maps back through the 490x368 image to remote
  pixel (99,93). This exercises both the downscale and the inverse
  coordinate transform.

### 6.5 `verify_resize.py` — terminal resize handling

**What it checks:** the client catches a terminal resize (via SIGWINCH and a
polling fallback) and re-renders at the new size.

**How:** run the client through a pseudo terminal, wait for the first frame,
then shrink the pty window size with `TIOCSWINSZ`. Check that a second
frame appears with raster attributes no larger than the new cell grid.

**Result:** frames at 320x240 (initial) and 160x120 (after shrink) — the
image adapts to the new terminal dimensions.

### 6.6 `verify_detect.py` — sixel capability probe

**What it checks:** `--check` reports the terminal's sixel support correctly.

**How:** run the client in a pseudo terminal, answering its `CSI c` device
attributes query with three different replies:
* `CSI ? 62;1;4;6;8;9;15;18;21;22 c` (a real sixel terminal) → "supported"
* `CSI ? 62;1;6;8 c` (no sixel) → "not supported"
* no answer at all → "unknown"

**Assertions:** each of the three cases produces the expected verdict.

### 6.6 `verify_pipeline.py` (image comparison detail)

The comparison uses a Python reimplementation of the C box filter so that
no code is shared between the client and the verifier. The decoded sixel
has 256 colour registers (the client's default), and the residual between
the decoded image and the reference is the quantisation noise introduced by
Floyd-Steinberg dithering against those 256 registers. The mean error of
3.97/255 over 512x384 pixels is consistent with that expectation.

## 7. Known limitations

* The palette is derived from the first complete frame. A desktop whose
  contents change colour drastically over time will look wrong until the
  next `--refresh` interval.
* wezterm ignores the DECGRA aspect ratio, so a wezterm user sees the
  image stretched to the cell grid.
* kitty, Ghostty and Alacritty do not support sixel at all; only a separate
  kitty-graphics-protocol renderer would cover them.
* The client assumes the terminal reports a usable pixel size via
  `TIOCGWINSZ`; on terminals that do not, 8x16 cells are assumed.
