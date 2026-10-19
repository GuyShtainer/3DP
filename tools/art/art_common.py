"""Shared helpers for the 3DP art generators: PNG writer and a tiny stroke rasterizer.

Everything here is original geometry; no fonts, images or audio are read.
Standard library only. Output is deterministic (same bytes on every run).
"""
import math
import struct
import zlib
from pathlib import Path

Point = tuple[float, float]
Color = tuple[float, float, float]


def write_png(path: Path, width: int, height: int, rgb: bytes) -> None:
    """Write an 8-bit RGB PNG; `rgb` holds width*height*3 bytes. Raises ValueError on size mismatch."""
    if len(rgb) != width * height * 3:
        raise ValueError("pixel buffer size does not match width*height*3")

    def chunk(tag: bytes, data: bytes) -> bytes:
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    stride = width * 3
    raw = b"".join(b"\x00" + rgb[row * stride:(row + 1) * stride] for row in range(height))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    path.write_bytes(png)


def arc(cx: float, cy: float, rx: float, ry: float, start_deg: float, end_deg: float, steps: int = 16) -> list[Point]:
    """Polyline approximating an elliptical arc (screen coordinates, y down)."""
    return [(cx + rx * math.cos(math.radians(start_deg + (end_deg - start_deg) * i / steps)),
             cy + ry * math.sin(math.radians(start_deg + (end_deg - start_deg) * i / steps)))
            for i in range(steps + 1)]


# Glyphs for "3", "D", "P" as stroke centre-lines in a unit box (x right, y down, height 1).
GLYPH_3: list[list[Point]] = [arc(0.28, 0.25, 0.26, 0.25, 200, 450) + arc(0.28, 0.75, 0.26, 0.25, 270, 520)]
GLYPH_D: list[list[Point]] = [[(0.25, 0.0), (0.0, 0.0), (0.0, 1.0), (0.25, 1.0)] + []]
GLYPH_D.append(arc(0.25, 0.5, 0.45, 0.5, -90, 90))
GLYPH_P: list[list[Point]] = [[(0.0, 1.0), (0.0, 0.0), (0.25, 0.0)],
                              arc(0.25, 0.27, 0.27, 0.27, -90, 90) + [(0.0, 0.54)]]
GLYPHS = [(GLYPH_3, 0.55), (GLYPH_D, 0.72), (GLYPH_P, 0.52)]  # (strokes, advance width)
GLYPH_GAP = 0.22


def lettering(height: float, x_scale: float, stroke: float, origin_x: float, origin_y: float) -> tuple[list[list[Point]], float]:
    """Place "3DP" with its top-left at the origin. Returns (polylines in pixels, total pixel width)."""
    lines: list[list[Point]] = []
    cursor = 0.0
    for strokes, advance in GLYPHS:
        for poly in strokes:
            lines.append([(origin_x + (cursor + px) * height * x_scale, origin_y + py * height) for px, py in poly])
        cursor += advance + GLYPH_GAP
    total = (cursor - GLYPH_GAP) * height * x_scale
    return lines, total


class Canvas:
    """Float RGB canvas with anti-aliased stroke drawing (4x4 supersampling)."""

    def __init__(self, width: int, height: int, top: Color, bottom: Color) -> None:
        self.width, self.height = width, height
        self.px: list[list[float]] = []
        for y in range(height):
            t = y / (height - 1)
            row = [top[c] + (bottom[c] - top[c]) * t for c in range(3)]
            self.px.extend(list(row) for _ in range(width))

    def _blend(self, x: int, y: int, color: Color, alpha: float) -> None:
        cell = self.px[y * self.width + x]
        for c in range(3):
            cell[c] += (color[c] - cell[c]) * alpha

    def stroke(self, poly: list[Point], half_width: float, color: Color, opacity: float = 1.0, ss: int = 4) -> None:
        """Draw a round-capped polyline of the given half width."""
        for (ax, ay), (bx, by) in zip(poly, poly[1:]):
            self._segment(ax, ay, bx, by, half_width, color, opacity, ss)

    def _segment(self, ax: float, ay: float, bx: float, by: float, hw: float, color: Color, opacity: float, ss: int) -> None:
        x0 = max(0, int(math.floor(min(ax, bx) - hw)) - 1)
        x1 = min(self.width - 1, int(math.ceil(max(ax, bx) + hw)) + 1)
        y0 = max(0, int(math.floor(min(ay, by) - hw)) - 1)
        y1 = min(self.height - 1, int(math.ceil(max(ay, by) + hw)) + 1)
        dx, dy = bx - ax, by - ay
        length_sq = dx * dx + dy * dy
        hw_sq = hw * hw
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                hits = 0
                for sy in range(ss):
                    py = y + (sy + 0.5) / ss
                    for sx in range(ss):
                        px = x + (sx + 0.5) / ss
                        t = 0.0 if length_sq == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / length_sq))
                        ex, ey = px - (ax + t * dx), py - (ay + t * dy)
                        if ex * ex + ey * ey <= hw_sq:
                            hits += 1
                if hits:
                    self._blend(x, y, color, opacity * hits / (ss * ss))

    def to_rgb(self) -> bytes:
        """Quantize to 8-bit RGB bytes."""
        return bytes(max(0, min(255, int(round(v)))) for cell in self.px for v in cell)


def shifted(lines: list[list[Point]], dx: float, dy: float) -> list[list[Point]]:
    """Return the polylines translated by (dx, dy)."""
    return [[(x + dx, y + dy) for x, y in poly] for poly in lines]
