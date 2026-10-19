"""Value-box pictures for XDuck and sForm (stock "style 12" knob graphic).

The firmware draws one frame of the knob graphic in each value box, picked
from the value of the knob that box shows. XDuck and sForm have one box,
which shows the first knob of the current page, so the frame set is made for
the page-1 selector: XDuck Type (Duck / Gate / Tex), sForm Wave (7 shapes).
One frame per value (knob_frames = max + 1), so no value-to-frame rounding.

Frames are 20 x 15 pixels; the LCD stretches rows ~1.4x, so shapes are drawn
a little flat. Output layout per frame: 20 columns x 2 pages, page-major,
bit0 = top row of the page (same as knob_arrow.py).
"""

from __future__ import annotations

import math

W, H = 20, 15

# Icons as text: '#' = ink. 20 columns x 15 rows each.
XDUCK_TYPE = {
    "Duck": [
        "....................",
        "............###.....",
        "...........#####....",
        "...........#.####...",
        "...........#######..",
        "............########",
        "............###.....",
        "..#.........###.....",
        "..##.......#####....",
        "..##############....",
        "..###############...",
        "...##############...",
        "....############....",
        ".....#########......",
        "....................",
    ],
    "Gate": [
        "....................",
        "....................",
        "......########......",
        "......#......#......",
        "......#......#......",
        "......#......#......",
        "......#......#......",
        "......#......#......",
        "......#......#......",
        "......#......#......",
        "......#......#......",
        "......#......#......",
        "#######......#######",
        "....................",
        "....................",
    ],
    "Tex": [
        "....................",
        "....................",
        "..###..........###..",
        ".#...#........#...#.",
        "#.....#......#.....#",
        "......#......#......",
        ".......#....#.......",
        "........####........",
        "....................",
        "..###..........###..",
        ".#...#........#...#.",
        "#.....#......#.....#",
        "......#......#......",
        ".......#....#.......",
        "........####........",
    ],
}


SMALL_TRI = [
    ".........##.........",
    "........#..#........",
    ".......#....#.......",
    "......#......#......",
    ".....#........#.....",
    "....#..........#....",
    "...#............#...",
]
SMALL_SAW_UP = [
    "...............#....",
    ".............###....",
    "...........##..#....",
    ".........##....#....",
    ".......##......#....",
    ".....##........#....",
    "...##..........#....",
]
SMALL_SAW_DN = [
    "....#...............",
    "....###.............",
    "....#..##...........",
    "....#....##.........",
    "....#......##.......",
    "....#........##.....",
    "....#..........##...",
]


def _plot(fn, y0=2, y1=12, x0=1, x1=18):
    """One-pixel line of fn(t) (0..1, 1 = top) column by column; vertical
    runs join neighbouring columns, so steps and edges stay crisp."""
    px = [[False] * W for _ in range(H)]
    prev = None
    for x in range(x0, x1 + 1):
        t = (x - x0 + 0.5) / (x1 - x0 + 1)
        y = int(round(y1 - fn(t) * (y1 - y0)))
        if prev is None:
            px[y][x] = True
        else:
            lo, hi = sorted((prev, y))
            step = 1 if y > prev else -1
            for yy in range(prev + step, y + step, step) if y != prev else (y,):
                px[yy][x] = True
        prev = y
    return px


def _sform_waves():
    def sine(t): return 0.5 + 0.5 * math.sin(2 * math.pi * t)
    def tri(t): return 2 * t if t < 0.5 else 2 - 2 * t
    def saw_up(t): return t          # one tooth: ramp + edge (two lines)
    def saw_dn(t): return 1.0 - t
    def square(t): return 1.0 if ((2 * t) % 1.0) < 0.5 else 0.0
    steps = [0.3, 0.9, 0.1, 0.65, 0.45, 1.0]
    def sh(t): return steps[min(5, int(t * 6))]
    drift_pts = [0.4, 0.95, 0.15, 0.75, 0.35]
    def drift(t):
        x = t * 4
        i = min(3, int(x))
        f = x - i
        s = f * f * (3 - 2 * f)
        return drift_pts[i] + s * (drift_pts[i + 1] - drift_pts[i])
    # Triangle and saws are drawn by hand a size smaller than the rest
    # (user's request); the saws are one tooth, two lines.
    frames = [_plot(f) for f in (sine, tri, saw_up, saw_dn, square, sh, drift)]
    for k, rows in ((1, SMALL_TRI), (2, SMALL_SAW_UP), (3, SMALL_SAW_DN)):
        frames[k] = _from_text(["." * W] * 4 + rows + ["." * W] * 4)
    return frames


def _from_text(rows):
    assert len(rows) == H and all(len(r) == W for r in rows), rows
    return [[c == "#" for c in r] for r in rows]


def _pack(frames) -> bytes:
    pages = (H + 7) // 8
    out = bytearray()
    for px in frames:
        for p in range(pages):
            for x in range(W):
                b = 0
                for bit in range(8):
                    y = p * 8 + bit
                    if y < H and px[y][x]:
                        b |= 1 << bit
                out.append(b)
    return bytes(out)


def xduck_type_frames():
    return [_from_text(XDUCK_TYPE[k]) for k in ("Duck", "Gate", "Tex")]


def xduck_type_icons() -> tuple[bytes, int]:
    return _pack(xduck_type_frames()), 3


def sform_wave_frames():
    return _sform_waves()


def sform_wave_icons() -> tuple[bytes, int]:
    return _pack(sform_wave_frames()), 7


if __name__ == "__main__":
    for name, frames in (("xduck", xduck_type_frames()), ("sform", sform_wave_frames())):
        for px in frames:
            print(name)
            for row in px:
                print("  " + "".join("#" if v else "." for v in row))
