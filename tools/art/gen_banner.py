"""Generate banner.png (256x128): large "3DP" as a stereo pair over a perspective floor grid.

Content stays inside x 40..216, y 14..114 (safe area of the 3D banner).
Usage: python3 tools/art/gen_banner.py [output_dir]   (default: repo root)
"""
import sys
from pathlib import Path

from art_common import Canvas, lettering, shifted, write_png

BACK_COLOR = (255, 96, 120)
FRONT_COLOR = (64, 224, 255)
GRID_COLOR = (90, 100, 180)


def floor_grid(canvas: Canvas) -> None:
    """Perspective grid under the lettering: converging lines plus receding horizontals."""
    horizon_y, bottom_y, vanish_x = 84.0, 114.0, 128.0
    for i in range(-6, 7):
        canvas.stroke([(vanish_x + i * 4.0, horizon_y), (vanish_x + i * 16.0, bottom_y)], 0.45, GRID_COLOR, 0.55)
    for k in range(5):
        depth = (k / 4.0) ** 1.6
        y = horizon_y + depth * (bottom_y - horizon_y)
        half = 24.0 + depth * 96.0
        canvas.stroke([(vanish_x - half, y), (vanish_x + half, y)], 0.45, GRID_COLOR, 0.55)


def render() -> bytes:
    """Return the banner as 256x128x3 RGB bytes."""
    canvas = Canvas(256, 128, (8, 10, 28), (34, 18, 70))
    floor_grid(canvas)
    lines, width = lettering(height=52.0, x_scale=0.9, stroke=9.0, origin_x=0.0, origin_y=24.0)
    lines = shifted(lines, (256 - width) / 2.0, 0.0)
    for poly in shifted(lines, -4.0, 2.5):
        canvas.stroke(poly, 4.5, BACK_COLOR)
    for poly in shifted(lines, 4.0, -2.5):
        canvas.stroke(poly, 4.5, FRONT_COLOR)
    return canvas.to_rgb()


def main() -> None:
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
    write_png(out_dir / "banner.png", 256, 128, render())


if __name__ == "__main__":
    main()
