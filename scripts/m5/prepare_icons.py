"""Generate Android mipmap launcher icons from the read-only iOS OH2 app icon."""

from __future__ import annotations

import argparse
from pathlib import Path
from PIL import Image

MIPMAP_SIZES: dict[str, int] = {
    "mipmap-mdpi": 48,
    "mipmap-hdpi": 72,
    "mipmap-xhdpi": 96,
    "mipmap-xxhdpi": 144,
    "mipmap-xxxhdpi": 192,
}


def generate_icons(src_path: Path, res_dir: Path) -> None:
    if not src_path.is_file():
        raise FileNotFoundError(f"Source icon not found: {src_path}")

    resampling = getattr(Image, "Resampling", Image).LANCZOS
    with Image.open(src_path) as img:
        rgba = img.convert("RGBA")
        for folder, size in MIPMAP_SIZES.items():
            out_dir = res_dir / folder
            out_dir.mkdir(parents=True, exist_ok=True)
            resized = rgba.resize((size, size), resampling)
            resized.save(out_dir / "ic_launcher.png", format="PNG", optimize=True)


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
    args = parser.parse_args()
    generate_icons(args.src, args.out_dir)
    print(f"Generated {len(MIPMAP_SIZES)} mipmap icons in {args.out_dir} from {args.src.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
