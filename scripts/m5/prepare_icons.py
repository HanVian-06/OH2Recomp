"""Generate Android mipmap launcher icons (from local iOS icon or original Arcadian emblem)."""

from __future__ import annotations

import argparse
import math
from pathlib import Path
from PIL import Image, ImageDraw

MIPMAP_SIZES: dict[str, int] = {
    "mipmap-mdpi": 48,
    "mipmap-hdpi": 72,
    "mipmap-xhdpi": 96,
    "mipmap-xxhdpi": 144,
    "mipmap-xxxhdpi": 192,
}


def render_arcadian_emblem(size: int = 512) -> Image.Image:
    """Render an original Arcadian-themed emblem icon (Aegean blue + antique gold)."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    px = img.load()
    cx, cy = size * 0.5, size * 0.5
    max_r = size * 0.48

    # Deep Aegean radial-vertical gradient backdrop inside rounded/circular badge
    for y in range(size):
        t = y / max(1, size - 1)
        for x in range(size):
            dx = (x - cx) / max_r
            dy = (y - cy) / max_r
            dist = math.hypot(dx, dy)
            if dist <= 1.0:
                glow = max(0.0, 1.0 - math.hypot(dx, dy + 0.25) * 0.85)
                r = int(min(255, 8 + 18 * (1.0 - t) + 26 * glow))
                g = int(min(255, 28 + 44 * (1.0 - t) + 64 * glow))
                b = int(min(255, 52 + 64 * (1.0 - t) + 88 * glow))
                px[x, y] = (r, g, b, 255)

    draw = ImageDraw.Draw(img, "RGBA")
    gold_bright = (244, 209, 122, 255)
    gold_mid = (212, 164, 70, 255)
    gold_soft = (212, 164, 70, 140)
    deep_navy = (11, 35, 58, 255)

    # Outer & inner gold rings
    pad1 = size * 0.045
    draw.ellipse([pad1, pad1, size - pad1, size - pad1], outline=gold_mid, width=max(4, size // 42))
    pad2 = size * 0.115
    draw.ellipse([pad2, pad2, size - pad2, size - pad2], outline=gold_soft, width=max(2, size // 128))

    # Stylized Arcadian triangle crest
    tr = size * 0.28
    outer_tri = [
        (cx, cy - tr * 0.96),
        (cx + tr * 0.90, cy + tr * 0.66),
        (cx - tr * 0.90, cy + tr * 0.66),
    ]
    draw.polygon(outer_tri, fill=gold_bright, outline=gold_mid)

    # Inner cut-out triangle
    itr = tr * 0.44
    inner_tri = [
        (cx, cy + itr * 0.86),
        (cx + itr * 0.86, cy - itr * 0.34),
        (cx - itr * 0.86, cy - itr * 0.34),
    ]
    draw.polygon(inner_tri, fill=deep_navy)

    # Cardinal diamond accents on outer ring
    ring_r = size * 0.455
    d = size * 0.026
    for ang in (0, 90, 180, 270):
        rad = math.radians(ang)
        x0 = cx + math.cos(rad) * ring_r
        y0 = cy + math.sin(rad) * ring_r
        draw.polygon([(x0, y0 - d), (x0 + d, y0), (x0, y0 + d), (x0 - d, y0)], fill=gold_bright)

    return img


def generate_icons(src_path: Path, res_dir: Path, force_generated: bool = False) -> str:
    resampling = getattr(Image, "Resampling", Image).LANCZOS
    if not force_generated and src_path.is_file():
        with Image.open(src_path) as img:
            base = img.convert("RGBA")
            source_label = src_path.name
    else:
        base = render_arcadian_emblem(512)
        source_label = "generated Arcadian emblem"

    for folder, size in MIPMAP_SIZES.items():
        out_dir = res_dir / folder
        out_dir.mkdir(parents=True, exist_ok=True)
        resized = base.resize((size, size), resampling)
        resized.save(out_dir / "ic_launcher.png", format="PNG", optimize=True)
    return source_label


def main() -> int:
    repo_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description="Generate Android mipmap launcher icons.")
    parser.add_argument(
        "--src",
        type=Path,
        default=repo_root / "Payload" / "OH2.app" / "AppIcon60x60@3x.png",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=repo_root / "build" / "m5" / "res",
    )
    parser.add_argument(
        "--generated",
        action="store_true",
        help="Always generate original open-source Arcadian emblem icon (no copyrighted assets).",
    )
    args = parser.parse_args()
    label = generate_icons(args.src, args.out_dir, force_generated=args.generated)
    print(f"Generated {len(MIPMAP_SIZES)} mipmap icons in {args.out_dir} from {label}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
