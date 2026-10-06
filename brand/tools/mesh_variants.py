"""Render a comparison grid of mesh parameter sets, to choose the mark's mesh.

Each tile is labelled ``spacing / jitter / margin / relax steps``. The first row
shows what relaxation buys you: without it the Delaunay result keeps the jittered
grid's starbursts and slivers.

Run:  python brand/tools/mesh_variants.py
"""

from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from PIL import Image, ImageDraw, ImageFont

import build_brand as B

TILE_W = 400
TILE_H = 150
PAD = 18
LABEL = 40
COLS = 3

# (spacing, jitter, margin, relax steps)
PARAMS = [
    (24.0, 8.0, 24.0, 0),
    (24.0, 8.0, 24.0, 1),
    (24.0, 8.0, 24.0, 3),
    (18.0, 6.0, 20.0, 3),
    (32.0, 11.0, 28.0, 3),
    (24.0, 8.0, 24.0, 6),
]
SEEDS = [B.MESH_SEED, 991]

ROWS = len(SEEDS)
WIDTH = PAD + COLS * (TILE_W + PAD)
HEIGHT = PAD + ROWS * (TILE_H + LABEL + PAD)
sheet = Image.new("RGBA", (WIDTH, HEIGHT), (11, 18, 32, 255))
draw = ImageDraw.Draw(sheet)
font = ImageFont.truetype(B.FONT_REGULAR, 15)

for row, seed in enumerate(SEEDS):
    for col, (spacing, jitter, margin, steps) in enumerate(PARAMS):
        facets = B.build_facets(spacing, jitter, margin, seed, steps)
        tile = B.mark_image(TILE_W, supersample=3, facets=facets)
        x = PAD + col * (TILE_W + PAD)
        y = PAD + row * (TILE_H + LABEL + PAD)
        sheet.alpha_composite(tile, (x, y))
        draw.text(
            (x, y + TILE_H + 12),
            "s%.0f j%.1f m%.0f  relax %d  facets %d" % (spacing, jitter, margin, steps, len(facets)),
            font=font,
            fill=(148, 163, 184, 255),
        )

os.makedirs(B.PNG_DIR, exist_ok=True)
out = os.path.join(B.PNG_DIR, "_mesh-variants.png")
sheet.convert("RGB").save(out, "PNG")
print("wrote", out)
