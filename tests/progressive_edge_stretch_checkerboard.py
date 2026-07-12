#!/usr/bin/env python3
"""Generate a standalone visual oracle for clip-owned progressive edge stretch.

The image intentionally does not use the live Vulkan renderer.  It documents the
sampling contract we want from the shader:

* the source clip is drawn once in the center;
* pixels outside the transformed clip sample from the nearest media edge;
* the sample walks progressively into a small edge band as it moves toward the
  output boundary;
* no outside pixel is produced by tiling the whole source image.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw


SOURCE_W = 160
SOURCE_H = 96
OUTPUT_W = 384
OUTPUT_H = 320
CLIP_LEFT = 112
CLIP_TOP = 112
CLIP_W = SOURCE_W
CLIP_H = SOURCE_H
EDGE_PIXELS = 18
POWER = 1.35
CELL = 16


def checker_color(x: int, y: int) -> tuple[int, int, int]:
    # Colored checkerboard: still recognizably a checkerboard, but edge samples
    # are easy to trace visually.
    parity = ((x // CELL) + (y // CELL)) & 1
    if parity:
        return (236, 239, 244)
    return (36, 44, 56)


def make_source() -> Image.Image:
    image = Image.new("RGB", (SOURCE_W, SOURCE_H))
    pixels = image.load()
    assert pixels is not None
    for y in range(SOURCE_H):
        for x in range(SOURCE_W):
            pixels[x, y] = checker_color(x, y)
    draw = ImageDraw.Draw(image)
    # One-pixel colored rim makes it obvious which boundary is being extended.
    draw.line([(0, 0), (SOURCE_W - 1, 0)], fill=(240, 80, 80))
    draw.line([(0, SOURCE_H - 1), (SOURCE_W - 1, SOURCE_H - 1)], fill=(80, 210, 110))
    draw.line([(0, 0), (0, SOURCE_H - 1)], fill=(80, 130, 245))
    draw.line([(SOURCE_W - 1, 0), (SOURCE_W - 1, SOURCE_H - 1)], fill=(245, 206, 80))
    return image


def clamp(value: int, low: int, high: int) -> int:
    return max(low, min(high, value))


def sample_source(source: Image.Image, sx: int, sy: int) -> tuple[int, int, int]:
    sx = clamp(sx, 0, SOURCE_W - 1)
    sy = clamp(sy, 0, SOURCE_H - 1)
    return source.getpixel((sx, sy))


def stretched_sample(source: Image.Image, x: int, y: int) -> tuple[int, int, int]:
    u = (x - CLIP_LEFT) / CLIP_W
    v = (y - CLIP_TOP) / CLIP_H
    inside = 0.0 <= u <= 1.0 and 0.0 <= v <= 1.0
    if inside:
        sx = clamp(int(u * (SOURCE_W - 1)), 0, SOURCE_W - 1)
        sy = clamp(int(v * (SOURCE_H - 1)), 0, SOURCE_H - 1)
        return sample_source(source, sx, sy)

    outside_x = max(-u, u - 1.0, 0.0)
    outside_y = max(-v, v - 1.0, 0.0)
    if outside_x >= outside_y:
        if u < 0.0:
            distance = CLIP_LEFT - x
            max_distance = max(1, CLIP_LEFT)
            t = min(1.0, distance / max_distance) ** POWER
            sx = int(t * (EDGE_PIXELS - 1))
        else:
            distance = x - (CLIP_LEFT + CLIP_W - 1)
            max_distance = max(1, OUTPUT_W - (CLIP_LEFT + CLIP_W))
            t = min(1.0, distance / max_distance) ** POWER
            sx = SOURCE_W - 1 - int(t * (EDGE_PIXELS - 1))
        sy = clamp(int(v * (SOURCE_H - 1)), 0, SOURCE_H - 1)
    else:
        if v < 0.0:
            distance = CLIP_TOP - y
            max_distance = max(1, CLIP_TOP)
            t = min(1.0, distance / max_distance) ** POWER
            sy = int(t * (EDGE_PIXELS - 1))
        else:
            distance = y - (CLIP_TOP + CLIP_H - 1)
            max_distance = max(1, OUTPUT_H - (CLIP_TOP + CLIP_H))
            t = min(1.0, distance / max_distance) ** POWER
            sy = SOURCE_H - 1 - int(t * (EDGE_PIXELS - 1))
        sx = clamp(int(u * (SOURCE_W - 1)), 0, SOURCE_W - 1)
    return sample_source(source, sx, sy)


def make_output(source: Image.Image) -> Image.Image:
    image = Image.new("RGB", (OUTPUT_W, OUTPUT_H), (12, 16, 22))
    pixels = image.load()
    assert pixels is not None
    for y in range(OUTPUT_H):
        for x in range(OUTPUT_W):
            pixels[x, y] = stretched_sample(source, x, y)
    draw = ImageDraw.Draw(image)
    draw.rectangle(
        [CLIP_LEFT, CLIP_TOP, CLIP_LEFT + CLIP_W - 1, CLIP_TOP + CLIP_H - 1],
        outline=(255, 255, 255),
        width=2,
    )
    return image


def assert_sampling_contract(source: Image.Image, output: Image.Image) -> None:
    # The clip interior must be the source image, not an effect-generated tile.
    assert output.getpixel((CLIP_LEFT + 24, CLIP_TOP + 24)) == source.getpixel((24, 24))
    assert output.getpixel((CLIP_LEFT + 96, CLIP_TOP + 48)) == source.getpixel((96, 48))

    # A row left of the clip may only draw from the left edge band, never from
    # arbitrary source columns.  This catches the old SourceTile-style failure.
    y = CLIP_TOP + 40
    observed = {output.getpixel((x, y)) for x in range(0, CLIP_LEFT - 4, 7)}
    allowed = {source.getpixel((sx, 40)) for sx in range(EDGE_PIXELS)}
    assert observed <= allowed, f"left stretch sampled outside edge band: {observed - allowed}"

    # The same invariant for the top edge.
    x = CLIP_LEFT + 72
    observed = {output.getpixel((x, y)) for y in range(0, CLIP_TOP - 4, 7)}
    allowed = {source.getpixel((72, sy)) for sy in range(EDGE_PIXELS)}
    assert observed <= allowed, f"top stretch sampled outside edge band: {observed - allowed}"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--out-dir",
        default="build/test_artifacts/progressive_edge_stretch_checkerboard",
        help="Directory for generated PNG files.",
    )
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    source = make_source()
    output = make_output(source)
    assert_sampling_contract(source, output)

    source_path = out_dir / "checkerboard_source.png"
    output_path = out_dir / "progressive_edge_stretch_checkerboard.png"
    source.save(source_path)
    output.save(output_path)
    print(output_path)


if __name__ == "__main__":
    main()
