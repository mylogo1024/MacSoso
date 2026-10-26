#!/usr/bin/env python3
"""Generate the macOS AppIcon image set from a full-size source icon.

The source image may use a white or near-white page background. The script
removes only the connected outer background, crops the artwork, places it on
the standard 1024 px macOS icon grid, and writes every PNG required by
Assets.xcassets/AppIcon.appiconset.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter
from scipy import ndimage


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = ROOT / "assets/appicon/macsoso_icon.png"
DEFAULT_OUTPUT = ROOT / "MacSoSo/Assets.xcassets/AppIcon.appiconset"
CANVAS_SIZE = 1024
CONTENT_SIZE = 824
SIZES = (16, 32, 64, 128, 256, 512, 1024)


def background_mask(rgb: np.ndarray, threshold: int) -> np.ndarray:
    """Return pixels connected to the canvas edge that are near-white."""
    near_white = rgb.min(axis=2) >= threshold
    labels, component_count = ndimage.label(near_white)
    if component_count == 0:
        return np.zeros(rgb.shape[:2], dtype=bool)

    edge_labels = np.unique(
        np.concatenate(
            (
                labels[0, :],
                labels[-1, :],
                labels[:, 0],
                labels[:, -1],
            )
        )
    )
    edge_labels = edge_labels[edge_labels > 0]
    return np.isin(labels, edge_labels)


def create_master(source: Path, threshold: int) -> Image.Image:
    rgb = np.asarray(Image.open(source).convert("RGB"), dtype=np.uint8)
    background = background_mask(rgb, threshold)
    alpha = np.where(background, 0, 255).astype(np.uint8)

    # A one-pixel feather avoids stair-stepping when the icon is downscaled.
    feather = ndimage.gaussian_filter(alpha.astype(np.float32), sigma=0.65)
    rgba = np.dstack((rgb, np.clip(feather, 0, 255).astype(np.uint8)))
    image = Image.fromarray(rgba, "RGBA")

    bounding_box = image.getbbox()
    if bounding_box is None:
        raise SystemExit(f"source image contains no foreground: {source}")
    image = image.crop(bounding_box)

    scale = min(CONTENT_SIZE / image.width, CONTENT_SIZE / image.height)
    target = (
        max(1, round(image.width * scale)),
        max(1, round(image.height * scale)),
    )
    image = image.resize(target, Image.Resampling.LANCZOS)

    canvas = Image.new("RGBA", (CANVAS_SIZE, CANVAS_SIZE), (0, 0, 0, 0))
    canvas.paste(
        image,
        (
            (CANVAS_SIZE - image.width) // 2,
            (CANVAS_SIZE - image.height) // 2,
        ),
        image,
    )
    return canvas


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument(
        "--threshold",
        type=int,
        default=235,
        help="minimum RGB channel value treated as removable background (default: 235)",
    )
    args = parser.parse_args()

    source = args.source.expanduser().resolve()
    output_dir = args.output_dir.expanduser().resolve()
    if not source.is_file():
        raise SystemExit(f"source image not found: {source}")
    output_dir.mkdir(parents=True, exist_ok=True)

    master = create_master(source, args.threshold)
    for size in SIZES:
        icon = master.resize((size, size), Image.Resampling.LANCZOS)
        if size <= 64:
            icon = icon.filter(ImageFilter.UnsharpMask(radius=1, percent=70))
        filename = output_dir / f"icon_{size}.png"
        icon.save(filename, format="PNG", optimize=True)
        print(filename)


if __name__ == "__main__":
    main()
