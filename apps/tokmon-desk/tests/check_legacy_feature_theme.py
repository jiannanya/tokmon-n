#!/usr/bin/env python3
"""Verify that non-trajectory feature surfaces use the Forest Sage light theme.

This check deliberately measures broad visual properties instead of individual
glyph pixels. It catches the regression that turned terminal, review, and file
surfaces into dark, unrelated panels while allowing renderer-specific text
antialiasing and small layout refinements.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


EXPECTED_SIZE = (2472, 1688)


def load_rgb(path: Path) -> np.ndarray:
    image = Image.open(path).convert("RGB")
    if image.size != EXPECTED_SIZE:
        raise SystemExit(
            f"{path}: expected {EXPECTED_SIZE[0]}x{EXPECTED_SIZE[1]}, "
            f"got {image.size[0]}x{image.size[1]}"
        )
    return np.asarray(image, dtype=np.uint8)


def surface_metrics(image: np.ndarray) -> dict[str, float]:
    # Ignore the native window shadow and title bar. The remaining area is the
    # feature surface and must follow the bright warm-white surface palette.
    region = image[150:-24, 24:-24].astype(np.int32)
    luminance = (
        region[:, :, 0] * 2126
        + region[:, :, 1] * 7152
        + region[:, :, 2] * 722
    ) / 10000.0
    return {
        "meanLuminance": float(luminance.mean()),
        "lightPixelRatio": float(np.mean(luminance >= 225.0)),
        "darkPixelRatio": float(np.mean(luminance < 80.0)),
    }


def accent_ratio(image: np.ndarray) -> float:
    pixels = image.astype(np.int16)
    red, green, blue = pixels[:, :, 0], pixels[:, :, 1], pixels[:, :, 2]
    # Includes the Forest Sage family around #2D5A43 and antialiased edges.
    accent = (
        (red >= 25)
        & (red <= 125)
        & (green >= 65)
        & (green <= 170)
        & (blue >= 35)
        & (blue <= 135)
        & (green >= red + 12)
        & (green >= blue + 8)
    )
    return float(np.mean(accent))


def tint_ratio(image: np.ndarray, kind: str) -> float:
    pixels = image.astype(np.int16)
    red, green, blue = pixels[:, :, 0], pixels[:, :, 1], pixels[:, :, 2]
    if kind == "green":
        tinted = (green >= red + 6) & (green >= blue + 5) & (green >= 150)
    elif kind == "red":
        tinted = (red >= green + 7) & (red >= blue + 7) & (red >= 170)
    else:
        raise ValueError(kind)
    return float(np.mean(tinted))


def check_surface(name: str, image: np.ndarray) -> dict[str, object]:
    metrics = surface_metrics(image)
    checks = {
        "meanLuminance": metrics["meanLuminance"] >= 235.0,
        "lightPixelRatio": metrics["lightPixelRatio"] >= 0.94,
        "darkPixelRatio": metrics["darkPixelRatio"] <= 0.01,
        "forestSagePresent": accent_ratio(image) >= 0.000005,
    }
    return {
        "name": name,
        **metrics,
        "forestSageRatio": accent_ratio(image),
        "checks": checks,
        "passed": all(checks.values()),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--review", type=Path, required=True)
    parser.add_argument("--files", type=Path, required=True)
    parser.add_argument("--terminal", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    review = load_rgb(args.review)
    files = load_rgb(args.files)
    terminal = load_rgb(args.terminal)

    surfaces = [
        check_surface("review", review),
        check_surface("files", files),
        check_surface("terminal", terminal),
    ]
    review_tints = {
        "additionRatio": tint_ratio(review, "green"),
        "deletionRatio": tint_ratio(review, "red"),
    }
    review_tints["passed"] = (
        review_tints["additionRatio"] >= 0.0001
        and review_tints["deletionRatio"] >= 0.0001
    )

    report = {
        "schema": 1,
        "policy": {
            "trajectory": "excluded; follows deepseek-harness visual language",
            "reviewFilesTerminal": (
                "Forest Sage warm-light palette with deep green accent"
            ),
        },
        "dimensions": {"width": EXPECTED_SIZE[0], "height": EXPECTED_SIZE[1]},
        "surfaces": surfaces,
        "reviewDiffTints": review_tints,
        "passed": all(surface["passed"] for surface in surfaces)
        and bool(review_tints["passed"]),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
