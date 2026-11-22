#!/usr/bin/env python3
"""
M3 Screenshot & Pixel Verification Script.
Captures a live PNG screenshot from the target Android device via ADB and verifies:
  1. Valid PNG header and dimensions (>= 1080x1080).
  2. Clear color dominance (RGB(16, 48, 80) +/- 12) in the central safe ROI.
  3. Non-clear rendered ASTC-textured triangle presence and chromatic variance.
  4. Negative-viewport Y-flip upright triangle geometry (lower half wider than upper half)
     and distinct top-apex vs bottom-base ASTC color distribution.
"""

import argparse
import io
import os
import subprocess
import sys

import numpy as np
from PIL import Image

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def capture_png_bytes(adb_path: str, serial: str, out_path: str) -> bytes:
    cmd = [adb_path, "-s", serial, "exec-out", "screencap", "-p"]
    try:
        raw = subprocess.check_output(cmd, timeout=15)
        if len(raw) > 64 and raw[:8] == PNG_MAGIC:
            return raw
    except Exception:
        pass

    remote_tmp = "/data/local/tmp/m3_screencap.png"
    subprocess.check_call([adb_path, "-s", serial, "shell", "screencap", "-p", remote_tmp], timeout=15)
    subprocess.check_call([adb_path, "-s", serial, "pull", remote_tmp, out_path], timeout=15)
    subprocess.call([adb_path, "-s", serial, "shell", "rm", "-f", remote_tmp], timeout=10)
    with open(out_path, "rb") as f:
        return f.read()


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify M3 Vulkan screencap")
    parser.add_argument("--adb", default="adb", help="Path to adb executable")
    parser.add_argument("--serial", required=True, help="ADB device serial")
    parser.add_argument("--out", required=True, help="Output PNG path")
    parser.add_argument("--clear-rgb", default="16,48,80", help="Expected clear RGB")
    args = parser.parse_args()

    clear_parts = [int(x.strip()) for x in args.clear_rgb.split(",")]
    if len(clear_parts) != 3:
        print("[FAIL] Invalid --clear-rgb format", file=sys.stderr)
        return 1
    clear_rgb = np.array(clear_parts, dtype=np.int32)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    raw = capture_png_bytes(args.adb, args.serial, args.out)

    if len(raw) < 64 or raw[:8] != PNG_MAGIC:
        print(f"[FAIL] Invalid PNG header or empty screencap ({len(raw)} bytes)", file=sys.stderr)
        return 1

    with open(args.out, "wb") as f:
        f.write(raw)

    im = Image.open(io.BytesIO(raw)).convert("RGB")
    width, height = im.size
    if width < 1080 or height < 1080:
        print(f"[FAIL] Unexpected screencap dimensions: {width}x{height}", file=sys.stderr)
        return 1

    arr = np.asarray(im, dtype=np.int32)

    # If the screenshot was captured while the framebuffer buffer was still portrait-oriented
    # (width < height) even though the app is landscape, rotate 90 degrees CCW so landscape
    # coordinates are normalized. On Android landscape, screencap is normally width > height.
    if width < height:
        arr = np.rot90(arr, k=1)
        height, width = arr.shape[0], arr.shape[1]

    y0, y1 = int(0.12 * height), int(0.88 * height)
    x0, x1 = int(0.12 * width), int(0.88 * width)
    roi = arr[y0:y1, x0:x1, :3]
    roi_h, roi_w = roi.shape[0], roi.shape[1]

    max_diff = np.max(np.abs(roi - clear_rgb.reshape((1, 1, 3))), axis=2)
    is_clear = max_diff <= 12
    is_tri = ~is_clear

    clear_ratio = float(np.mean(is_clear))
    tri_ratio = float(np.mean(is_tri))

    if clear_ratio < 0.45:
        print(
            f"[FAIL] Clear color RGB({clear_parts[0]},{clear_parts[1]},{clear_parts[2]}) "
            f"ratio too low: {clear_ratio:.3f} (expected >= 0.45)",
            file=sys.stderr,
        )
        return 1

    if not (0.05 <= tri_ratio <= 0.45):
        print(
            f"[FAIL] Non-clear triangle ratio out of range: {tri_ratio:.3f} (expected 0.05..0.45)",
            file=sys.stderr,
        )
        return 1

    tri_pixels = roi[is_tri]
    color_std = float(np.std(tri_pixels))
    if color_std <= 15.0:
        print(
            f"[FAIL] Triangle pixel chromatic std too low: {color_std:.2f} (expected > 15.0)",
            file=sys.stderr,
        )
        return 1

    ys, xs = np.where(is_tri)
    ymin, ymax = int(np.min(ys)), int(np.max(ys))
    xmin, xmax = int(np.min(xs)), int(np.max(xs))
    ymid = (ymin + ymax) // 2

    cx = float(np.mean(xs)) / float(roi_w)
    cy = float(np.mean(ys)) / float(roi_h)
    if not (0.30 <= cx <= 0.70 and 0.30 <= cy <= 0.75):
        print(
            f"[FAIL] Triangle centroid ({cx:.3f}, {cy:.3f}) not centered in ROI",
            file=sys.stderr,
        )
        return 1

    upper_mask = ys < ymid
    lower_mask = ys >= ymid
    upper_count = int(np.sum(upper_mask))
    lower_count = int(np.sum(lower_mask))

    if upper_count == 0 or lower_count < int(1.5 * upper_count):
        print(
            f"[FAIL] Y-flip orientation check failed: upper_count={upper_count}, "
            f"lower_count={lower_count} (expected lower_count >= 1.5 * upper_count for upright apex-up triangle)",
            file=sys.stderr,
        )
        return 1

    y_third_1 = ymin + (ymax - ymin) // 3
    y_third_2 = ymin + (2 * (ymax - ymin)) // 3
    top_third_pixels = tri_pixels[ys < y_third_1]
    bot_third_pixels = tri_pixels[ys >= y_third_2]
    top_mean = np.mean(top_third_pixels, axis=0)
    bot_mean = np.mean(bot_third_pixels, axis=0)
    rgb_dist = float(np.linalg.norm(top_mean - bot_mean))

    if rgb_dist < 20.0:
        print(
            f"[FAIL] Top-third vs bottom-third RGB mean distance too small ({rgb_dist:.2f}); "
            f"top={top_mean.tolist()} bot={bot_mean.tolist()}",
            file=sys.stderr,
        )
        return 1

    print(
        f"[PASS] Screencap verified: size={width}x{height} clear_ratio={clear_ratio:.3f} "
        f"tri_ratio={tri_ratio:.3f} std={color_std:.1f} lower/upper={lower_count}/{upper_count} "
        f"({lower_count / max(1, upper_count):.2f}x) top_bot_rgb_dist={rgb_dist:.1f}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
