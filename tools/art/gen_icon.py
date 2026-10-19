"""Generate icon.png (48x48): "3DP" as a stereo pair of offset layers on a dark tile.

Usage: python3 tools/art/gen_icon.py [output_dir]   (default: repo root)
"""
import sys
from pathlib import Path

from art_common import Canvas, lettering, shifted, write_png

BACK_COLOR = (255, 96, 120)    # coral layer (left-eye)
FRONT_COLOR = (64, 224, 255)   # cyan layer (right-eye)


def render() -> bytes:
    """Return the icon as 48x48x3 RGB bytes."""
    canvas = Canvas(48, 48, (10, 14, 34), (30, 18, 62))
    lines, width = lettering(height=18.0, x_scale=0.78, stroke=3.0, origin_x=0.0, origin_y=15.0)
    lines = shifted(lines, (48 - width) / 2.0, 0.0)
    canvas.stroke([(5.0, 40.5), (43.0, 40.5)], 0.7, (110, 120, 190), 0.6)   # ground line for depth
    for poly in shifted(lines, -1.3, 0.8):
        canvas.stroke(poly, 1.7, BACK_COLOR)
    for poly in shifted(lines, 1.3, -0.8):
        canvas.stroke(poly, 1.7, FRONT_COLOR)
    return canvas.to_rgb()


def main() -> None:
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
    write_png(out_dir / "icon.png", 48, 48, render())


if __name__ == "__main__":
    main()
