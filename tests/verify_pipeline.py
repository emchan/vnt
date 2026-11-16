#!/usr/bin/env python3
"""
Verify the vncsixel pipeline end to end.

  1. run the client against the static upstream libvncserver example server
     (it dumps the received framebuffer as a PPM and the sixel stream)
  2. decode the sixel stream back to a PNG with libsixel's sixel2png
  3. independently box-filter the received framebuffer down to the size the
     client used, and compare it with the decoded PNG

A correct pipeline produces a decoded image that closely matches the
reference (the residual is the colour quantisation to 256 registers plus
Floyd-Steinberg dithering).
"""
import os
import subprocess
import sys

from PIL import Image
import numpy as np

WORK = os.path.join(os.path.dirname(__file__), "..", ".verify")
os.makedirs(WORK, exist_ok=True)

SIXEL2PNG = os.environ.get(
    "SIXEL2PNG", "/tmp/opencode/src/libsixel-master/converters/sixel2png")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLIENT = os.path.join(ROOT, "vnt")
SERVER = os.environ.get(
    "SERVER", "/tmp/opencode/src/libvncserver-master/build/examples/server/example")


def box_downscale(arr, dw, dh):
    """Mirror of scale_rgb24()'s box filter in scaler.c."""
    sh, sw = arr.shape[:2]
    out = np.zeros((dh, dw, 3), dtype=np.float64)
    for dy in range(dh):
        y0 = (dy * sh) // dh
        y1 = ((dy + 1) * sh + dh - 1) // dh
        if y1 <= y0:
            y1 = y0 + 1
        y1 = min(y1, sh)
        for dx in range(dw):
            x0 = (dx * sw) // dw
            x1 = ((dx + 1) * sw + dw - 1) // dw
            if x1 <= x0:
                x1 = x0 + 1
            x1 = min(x1, sw)
            out[dy, dx] = arr[y0:y1, x0:x1].reshape(-1, 3).mean(axis=0)
    return out


def main():
    server = subprocess.Popen([SERVER], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)
    import time
    time.sleep(1.5)
    try:
        ppm = os.path.join(WORK, "recv.ppm")
        six = os.path.join(WORK, "out.sixel")
        png = os.path.join(WORK, "out.png")
        for f in (ppm, six, png):
            if os.path.exists(f):
                os.unlink(f)

        subprocess.run([CLIENT, "-e", "raw", "--no-cursor", "--dump", six,
                        "--dump-ppm", ppm, "--frames", "1",
                        "127.0.0.1:5900"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=30)
        subprocess.run([SIXEL2PNG, six], stdout=open(png, "wb"),
                       stderr=subprocess.DEVNULL, timeout=30)

        assert os.path.exists(png), "sixel2png produced no output"
        assert os.path.exists(ppm), "client produced no framebuffer dump"

        got = np.asarray(Image.open(png).convert("RGB"), dtype=np.float64)

        ref_img = Image.open(ppm)
        ref = np.asarray(ref_img.convert("RGB"), dtype=np.float64)
        ref_small = box_downscale(ref, got.shape[1], got.shape[0])

        err = np.abs(got - ref_small)
        mean_err = err.mean()
        max_err = err.max()

        # Structural correlation: ignore the palette quantisation noise.
        g1 = got.mean(axis=2).ravel()
        g2 = ref_small.mean(axis=2).ravel()
        corr = float(np.corrcoef(g1, g2)[0, 1])

        print("decoded sixel : %dx%d" % (got.shape[1], got.shape[0]))
        print("reference     : %dx%d (downscaled from %dx%d)"
              % (ref_small.shape[1], ref_small.shape[0], ref.shape[1], ref.shape[0]))
        print("mean abs err  : %.2f / 255" % mean_err)
        print("max  abs err  : %.2f / 255" % max_err)
        print("correlation   : %.4f" % corr)

        # The decoded image must not be blank.
        assert got.std() > 10.0, "decoded image is blank (std=%.2f)" % got.std()
        assert corr > 0.95, "decoded image does not match the reference"
        assert mean_err < 20.0, "mean error too large: %.2f" % mean_err

        print("\nPASS")
        return 0
    finally:
        server.terminate()
        server.wait()


if __name__ == "__main__":
    sys.exit(main())
