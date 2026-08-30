#!/usr/bin/env python3
"""Compare synchronized legacy/new desktop captures without hiding layout bugs.

The only automatic exclusion is a one-pixel antialias fringe around pixels that
are already glyph/icon edges in both images. Solid glyph interiors, controls,
fills, geometry, colors, spacing and any displaced text remain compared.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import cv2
import numpy as np
from PIL import Image


def load_rgb(path: Path) -> np.ndarray:
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.uint8)


def antialias_fringe(image: np.ndarray) -> np.ndarray:
    gray = cv2.cvtColor(image, cv2.COLOR_RGB2GRAY)
    # MiSans and the monochrome SVGs have dark cores. Their partially covered
    # edge pixels are neither close to the core nor close to the local light
    # background. This intentionally does not mask solid glyph interiors.
    local_min = cv2.erode(gray, np.ones((3, 3), np.uint8))
    local_max = cv2.dilate(gray, np.ones((3, 3), np.uint8))
    transition = (local_max.astype(np.int16) - local_min.astype(np.int16)) >= 20
    partial = (gray > local_min + 5) & (gray + 5 < local_max)
    return transition & partial


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("legacy", type=Path)
    parser.add_argument("current", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--channel-tolerance", type=int, default=6)
    parser.add_argument("--max-difference-percent", type=float, default=0.5)
    args = parser.parse_args()

    legacy = load_rgb(args.legacy)
    current = load_rgb(args.current)
    if legacy.shape != current.shape:
        raise SystemExit(
            f"capture dimensions differ: {legacy.shape[:2]} != {current.shape[:2]}"
        )

    fringe = antialias_fringe(legacy) & antialias_fringe(current)
    compared = ~fringe
    delta = np.abs(legacy.astype(np.int16) - current.astype(np.int16))
    different = np.max(delta, axis=2) > args.channel_tolerance
    compared_count = int(np.count_nonzero(compared))
    different_count = int(np.count_nonzero(different & compared))
    ratio = 100.0 * different_count / max(compared_count, 1)

    args.output_dir.mkdir(parents=True, exist_ok=True)
    mask_image = np.where(fringe, 255, 0).astype(np.uint8)
    Image.fromarray(mask_image, mode="L").save(args.output_dir / "aa-fringe-mask.png")
    heat = np.zeros_like(legacy)
    heat[different & compared] = (255, 48, 48)
    heat[fringe] = (58, 151, 255)
    Image.fromarray(heat, mode="RGB").save(args.output_dir / "difference-map.png")
    blend = ((legacy.astype(np.uint16) + current.astype(np.uint16)) // 2).astype(
        np.uint8
    )
    blend[different & compared] = (
        blend[different & compared].astype(np.uint16) // 3
    ).astype(np.uint8)
    blend[different & compared, 0] = 255
    Image.fromarray(blend, mode="RGB").save(args.output_dir / "overlay.png")

    report = {
        "schema": 1,
        "legacy": str(args.legacy.resolve()),
        "current": str(args.current.resolve()),
        "dimensions": {"width": int(legacy.shape[1]), "height": int(legacy.shape[0])},
        "channelTolerance": args.channel_tolerance,
        "antialiasMask": "one-pixel intersection fringe; solid glyphs remain compared",
        "maskedPixels": int(np.count_nonzero(fringe)),
        "comparedPixels": compared_count,
        "differentPixels": different_count,
        "differencePercent": ratio,
        "maximumDifferencePercent": args.max_difference_percent,
        "passed": ratio <= args.max_difference_percent,
    }
    (args.output_dir / "report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
