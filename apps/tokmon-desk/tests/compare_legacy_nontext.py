#!/usr/bin/env python3
"""Compare the frozen legacy shell while separating renderer rasterization.

The legacy Slint capture and the RmlUi/FreeType/Skia capture do not share a
text or vector antialiaser. This comparator therefore:

* excludes only the 12-pixel operating-system shadow gutter;
* masks glyph pixels inside frozen, named text regions and validates their
  bounding geometry independently;
* masks at most a two-pixel fringe around edges found in both captures;
* continues to compare fills, unmatched edges, spacing, colors and shadows.

The edge mask cannot hide a displaced or missing control because it is formed
from the intersection of nearby edges in both images. Element box geometry is
also covered by the separate UI contract report.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import cv2
import numpy as np
from PIL import Image


BASELINE_SIZE = (2472, 1688)
NATIVE_GUTTER = 12

# Tight regions used for explicit text-geometry checks. Coordinates are in the
# frozen 2472x1688 legacy capture, not application layout units.
TEXT_CHECK_REGIONS: dict[str, tuple[int, int, int, int]] = {
    "brand": (95, 35, 245, 78),
    "session-title": (480, 28, 780, 78),
    "mode-tabs": (850, 30, 1025, 75),
    "new-session": (165, 135, 330, 180),
    "navigation-search": (92, 232, 365, 278),
    "tree-caption": (42, 314, 255, 352),
    "starter-heading": (810, 565, 1680, 630),
    "starter-1-title": (550, 805, 820, 850),
    "starter-1-detail": (550, 855, 820, 900),
    "starter-2-title": (930, 805, 1210, 850),
    "starter-2-detail": (930, 855, 1210, 900),
    "starter-3-title": (1310, 805, 1590, 850),
    "starter-3-detail": (1310, 855, 1590, 900),
    "starter-4-title": (1690, 805, 1970, 850),
    "starter-4-detail": (1690, 855, 1970, 900),
    "composer-placeholder": (550, 1488, 1100, 1540),
    "launcher-review-label": (2160, 795, 2250, 875),
    "launcher-review-shortcut": (2250, 795, 2410, 875),
    "launcher-files-label": (2160, 895, 2250, 980),
    "launcher-files-shortcut": (2250, 895, 2410, 980),
}

# Broader text-only lanes used to remove glyph interiors from the non-text
# comparison. Icons are deliberately outside these lanes where practical.
TEXT_MASK_REGIONS = list(TEXT_CHECK_REGIONS.values()) + [
    (120, 355, 365, 1595),       # navigation labels, excluding icons
    (94, 1625, 180, 1665),       # settings label
    (620, 1408, 1120, 1460),     # workspace context labels
    (640, 1560, 820, 1630),      # access label
    (1480, 1560, 1770, 1630),    # model label
    (1770, 1560, 1900, 1630),    # effort label
]


def load_rgb(path: Path) -> np.ndarray:
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.uint8)


def glyph_mask(crop: np.ndarray) -> np.ndarray:
    gray = cv2.cvtColor(crop, cv2.COLOR_RGB2GRAY)
    local_light = cv2.dilate(gray, np.ones((7, 7), np.uint8))
    return ((local_light.astype(np.int16) - gray.astype(np.int16)) > 8) & (
        gray < 205
    )


def bbox(mask: np.ndarray, x: int, y: int) -> tuple[int, int, int, int] | None:
    rows, columns = np.where(mask)
    if columns.size == 0:
        return None
    return (
        x + int(columns.min()),
        y + int(rows.min()),
        x + int(columns.max()) + 1,
        y + int(rows.max()) + 1,
    )


def row_bands(mask: np.ndarray, y: int) -> list[tuple[int, int]]:
    rows = np.where(mask.sum(axis=1) > 2)[0] + y
    bands: list[list[int]] = []
    for row in rows:
        if not bands or row > bands[-1][-1] + 1:
            bands.append([int(row)])
        else:
            bands[-1].append(int(row))
    return [(band[0], band[-1]) for band in bands if len(band) > 2]


def coincident_edge_fringe(legacy: np.ndarray, current: np.ndarray) -> np.ndarray:
    kernel = np.ones((3, 3), np.uint8)

    def transitions(image: np.ndarray) -> np.ndarray:
        gray = cv2.cvtColor(image, cv2.COLOR_RGB2GRAY)
        low = cv2.erode(gray, kernel)
        high = cv2.dilate(gray, kernel)
        return (high.astype(np.int16) - low.astype(np.int16)) >= 12

    legacy_edges = cv2.dilate(transitions(legacy).astype(np.uint8), kernel)
    current_edges = cv2.dilate(transitions(current).astype(np.uint8), kernel)
    nearby_in_both = (legacy_edges & current_edges).astype(np.uint8)
    return cv2.dilate(nearby_in_both, kernel).astype(bool)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("legacy", type=Path)
    parser.add_argument("current", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--channel-tolerance", type=int, default=6)
    parser.add_argument("--max-difference-percent", type=float, default=0.5)
    parser.add_argument("--max-text-edge-delta", type=int, default=4)
    args = parser.parse_args()

    legacy = load_rgb(args.legacy)
    current = load_rgb(args.current)
    if legacy.shape != current.shape:
        raise SystemExit(
            f"capture dimensions differ: {legacy.shape[:2]} != {current.shape[:2]}"
        )
    height, width = legacy.shape[:2]
    if (width, height) != BASELINE_SIZE:
        raise SystemExit(
            f"this frozen comparator requires {BASELINE_SIZE}, got {(width, height)}"
        )

    text_mask = np.zeros((height, width), dtype=bool)
    for x1, y1, x2, y2 in TEXT_MASK_REGIONS:
        for image in (legacy, current):
            region_mask = glyph_mask(image[y1:y2, x1:x2])
            expanded = cv2.dilate(
                region_mask.astype(np.uint8), np.ones((3, 3), np.uint8)
            ).astype(bool)
            text_mask[y1:y2, x1:x2] |= expanded

    text_checks = []
    text_geometry_passed = True
    for name, (x1, y1, x2, y2) in TEXT_CHECK_REGIONS.items():
        legacy_mask = glyph_mask(legacy[y1:y2, x1:x2])
        current_mask = glyph_mask(current[y1:y2, x1:x2])
        legacy_box = bbox(legacy_mask, x1, y1)
        current_box = bbox(current_mask, x1, y1)
        deltas = (
            [abs(left - right) for left, right in zip(legacy_box, current_box)]
            if legacy_box and current_box
            else []
        )
        passed = bool(deltas) and max(deltas) <= args.max_text_edge_delta
        text_geometry_passed &= passed
        text_checks.append(
            {
                "name": name,
                "legacyBox": legacy_box,
                "currentBox": current_box,
                "edgeDeltas": deltas,
                "passed": passed,
            }
        )

    navigation_region = (120, 355, 352, 1595)
    x1, y1, x2, y2 = navigation_region
    legacy_bands = row_bands(glyph_mask(legacy[y1:y2, x1:x2]), y1)
    current_bands = row_bands(glyph_mask(current[y1:y2, x1:x2]), y1)
    navigation_deltas = []
    if len(legacy_bands) == len(current_bands):
        navigation_deltas = [
            abs((left[0] + left[1]) - (right[0] + right[1])) / 2.0
            for left, right in zip(legacy_bands, current_bands)
        ]
    navigation_passed = bool(navigation_deltas) and max(navigation_deltas) <= 3
    text_geometry_passed &= navigation_passed

    edge_mask = coincident_edge_fringe(legacy, current)
    roi = np.zeros((height, width), dtype=bool)
    roi[NATIVE_GUTTER:-NATIVE_GUTTER, NATIVE_GUTTER:-NATIVE_GUTTER] = True
    compared = roi & ~text_mask & ~edge_mask
    delta = np.abs(legacy.astype(np.int16) - current.astype(np.int16))
    different = np.max(delta, axis=2) > args.channel_tolerance
    compared_count = int(np.count_nonzero(compared))
    different_count = int(np.count_nonzero(different & compared))
    difference_percent = 100.0 * different_count / max(compared_count, 1)
    pixels_passed = difference_percent <= args.max_difference_percent

    args.output_dir.mkdir(parents=True, exist_ok=True)
    mask_visual = np.zeros((height, width, 3), dtype=np.uint8)
    mask_visual[text_mask] = (58, 151, 255)
    mask_visual[edge_mask & ~text_mask] = (250, 181, 39)
    mask_visual[~roi] = (120, 120, 120)
    Image.fromarray(mask_visual, mode="RGB").save(args.output_dir / "mask.png")
    difference_visual = np.zeros_like(legacy)
    difference_visual[different & compared] = (255, 48, 48)
    Image.fromarray(difference_visual, mode="RGB").save(
        args.output_dir / "nontext-difference-map.png"
    )

    report = {
        "schema": 1,
        "legacy": str(args.legacy.resolve()),
        "current": str(args.current.resolve()),
        "dimensions": {"width": width, "height": height},
        "nativeShadowGutterPixels": NATIVE_GUTTER,
        "channelTolerance": args.channel_tolerance,
        "maskPolicy": {
            "text": "adaptive glyph pixels in frozen named regions, dilated one pixel",
            "edges": "two-pixel fringe around edges present within one pixel in both captures",
            "unmatchedGeometryRemainsCompared": True,
        },
        "textMaskedPixels": int(np.count_nonzero(text_mask)),
        "edgeMaskedPixels": int(np.count_nonzero(edge_mask & ~text_mask)),
        "comparedPixels": compared_count,
        "differentNonTextPixels": different_count,
        "nonTextDifferencePercent": difference_percent,
        "maximumDifferencePercent": args.max_difference_percent,
        "textGeometry": {
            "maximumEdgeDeltaPixels": args.max_text_edge_delta,
            "checks": text_checks,
            "navigationLegacyBands": legacy_bands,
            "navigationCurrentBands": current_bands,
            "navigationCenterDeltas": navigation_deltas,
            "navigationPassed": navigation_passed,
            "passed": text_geometry_passed,
        },
        "pixelsPassed": pixels_passed,
        "passed": pixels_passed and text_geometry_passed,
    }
    (args.output_dir / "report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
