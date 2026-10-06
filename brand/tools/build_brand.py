"""Build the TacUI brand assets.

One geometry definition drives both the SVG and the PNG output, so the vector
and raster deliverables cannot drift apart.

The mark tells the library's story in three layers:

* a ``T`` silhouette, tessellated into an equilateral triangle lattice
  (triangulation),
* flat-shaded facets over a cyan -> blue / indigo -> violet sweep (GPU shading),
* a horizontal *and* a vertical translucent plane whose intersection is screened
  to a bright core -- that overlap is the layer-compositing cue.

Run:  python brand/tools/build_brand.py
"""

from __future__ import annotations

import math
import os
import random
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont
from ttf_outline import TrueTypeFont, contour_to_quadratics, contours_to_path, ink_bbox

# --------------------------------------------------------------------- paths

HERE = os.path.dirname(os.path.abspath(__file__))
BRAND = os.path.dirname(HERE)
PNG_DIR = os.path.join(BRAND, "png")
FONT_DIR = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts")
FONT_BOLD = os.path.join(FONT_DIR, "segoeuib.ttf")
FONT_REGULAR = os.path.join(FONT_DIR, "segoeui.ttf")
FONT_BOLD_ITALIC = os.path.join(FONT_DIR, "segoeuiz.ttf")

# ------------------------------------------------------------------ geometry
#
# The mark is the whole word "Tac", not just its initial: the graphic carries the
# first syllable and the lockup's type carries "UI", so mark + wordmark reads as
# one continuous "TacUI". The letterforms come from the same Segoe UI Bold the
# wordmark is outlined from, so both halves of that word agree.

MARK_PAD = 21.0
MARK_CAP = 170.0
WORD = "Tac"
WORD_TRACKING = -0.022         # em -- tight enough that the letters interlock
CHAMFER = 11.0                 # bevel on the T's convex corners

# The letters are set in Segoe UI Bold *Italic*: a real designed italic with a
# forward lean, not an upright face shoved over with a shear matrix. Its ``T`` is
# still a plain eight-point octagon, so the machined bevel survives.
FACE = TrueTypeFont(FONT_BOLD_ITALIC)


def _towards(point, target, distance):
    dx = target[0] - point[0]
    dy = target[1] - point[1]
    length = math.hypot(dx, dy) or 1.0
    return (point[0] + dx / length * distance, point[1] + dy / length * distance)


def _signed_area(poly):
    total = 0.0
    for index, point in enumerate(poly):
        nxt = poly[(index + 1) % len(poly)]
        total += point[0] * nxt[1] - nxt[0] * point[1]
    return total / 2.0


def chamfer(poly, distance):
    """Cut every convex corner with a straight bevel; concave corners stay sharp.

    Orientation-agnostic: the polygon's own winding decides which corners are
    convex, so it works on the silhouette whichever way round it is given.
    """
    sign = 1.0 if _signed_area(poly) > 0 else -1.0
    out = []
    count = len(poly)
    for index, point in enumerate(poly):
        prev = poly[index - 1]
        nxt = poly[(index + 1) % count]
        e1 = (point[0] - prev[0], point[1] - prev[1])
        e2 = (nxt[0] - point[0], nxt[1] - point[1])
        if sign * (e1[0] * e2[1] - e1[1] * e2[0]) <= 0.0:
            out.append(point)
            continue
        out.append(_towards(point, prev, distance))
        out.append(_towards(point, nxt, distance))
    return out


# ------------------------------------------------- convex region arithmetic

def _line(a, b):
    """Line through ``a`` and ``b`` as ``(A, B, C)`` with ``A*x + B*y + C = 0``."""
    return (a[1] - b[1], b[0] - a[0], a[0] * b[1] - b[0] * a[1])


def _side(line, point):
    return line[0] * point[0] + line[1] * point[1] + line[2]


def _crossing(line, a, b):
    va = _side(line, a)
    vb = _side(line, b)
    t = va / (va - vb) if va != vb else 0.0
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)


def clip_halfplane(poly, line, keep):
    """Keep the part of ``poly`` on the same side of ``line`` as ``keep``."""
    sign = 1.0 if _side(line, keep) >= 0.0 else -1.0
    out = []
    for index, current in enumerate(poly):
        previous = poly[index - 1]
        current_side = _side(line, current) * sign
        previous_side = _side(line, previous) * sign
        if current_side >= 0.0:
            if previous_side < 0.0:
                out.append(_crossing(line, previous, current))
            out.append(current)
        elif previous_side >= 0.0:
            out.append(_crossing(line, previous, current))
    return out


def clip_convex(poly, clip_poly):
    """Sutherland-Hodgman clip of a convex polygon against a convex polygon.

    The colour regions of an italic ``T`` are parallelograms rather than
    rectangles, so the region cut has to work against an arbitrary convex shape.
    """
    if len(clip_poly) < 3:
        return []
    centre = (
        sum(p[0] for p in clip_poly) / len(clip_poly),
        sum(p[1] for p in clip_poly) / len(clip_poly),
    )
    out = list(poly)
    for index, point in enumerate(clip_poly):
        if not out:
            return []
        nxt = clip_poly[(index + 1) % len(clip_poly)]
        out = clip_halfplane(out, _line(point, nxt), centre)
    return out


def _midpoint(a, b):
    return ((a[0] + b[0]) / 2.0, (a[1] + b[1]) / 2.0)


def _lay_out_word():
    """Place the word's glyphs in font units, at the mark's tracking."""
    tracking = WORD_TRACKING * FACE.units_per_em
    pen = 0.0
    placed = []
    for character in WORD:
        contours = FACE.contours(FACE.glyph_id(character))
        if contours:
            placed.append(
                (character, [[(x + pen, y, on) for x, y, on in c] for c in contours])
            )
        pen += FACE.advance_units(character) + tracking
    return placed


PLACED = _lay_out_word()

_FONT_X0 = min(p[0] for _, cs in PLACED for c in cs for p in c)
_FONT_X1 = max(p[0] for _, cs in PLACED for c in cs for p in c)
_FONT_Y0 = min(p[1] for _, cs in PLACED for c in cs for p in c)
_FONT_Y1 = max(p[1] for _, cs in PLACED for c in cs for p in c)

SCALE = MARK_CAP / (_FONT_Y1 - _FONT_Y0)
MARK_W = (_FONT_X1 - _FONT_X0) * SCALE + 2.0 * MARK_PAD
MARK_H = MARK_CAP + 2.0 * MARK_PAD
MARK_BASELINE = MARK_PAD + MARK_CAP
MARK_CX = MARK_W / 2.0
MARK_CY = MARK_H / 2.0


def to_mark(x, y):
    """Font units -> mark units: y flipped, baseline on ``MARK_BASELINE``."""
    return (MARK_PAD + (x - _FONT_X0) * SCALE, MARK_PAD + (_FONT_Y1 - y) * SCALE)


def contour_polyline(contour, steps=10):
    """Flatten a TrueType contour (quadratics sampled) into a polyline."""
    points = []
    current = (0.0, 0.0)
    for command, values in contour_to_quadratics(contour):
        if command == "M":
            current = values[0]
            points.append(current)
        elif command == "L":
            current = values[0]
            points.append(current)
        elif command == "Q":
            control, end = values
            for step in range(1, steps + 1):
                t = step / float(steps)
                inv = 1.0 - t
                points.append(
                    (
                        inv * inv * current[0] + 2 * inv * t * control[0] + t * t * end[0],
                        inv * inv * current[1] + 2 * inv * t * control[1] + t * t * end[1],
                    )
                )
            current = end
    return points


def _glyph_contours():
    """Placed glyph contours in mark space, with the ``T`` rebuilt and bevelled.

    Segoe UI Bold's ``T`` is a plain rectilinear octagon, so it can be rebuilt
    from its own corners and given the machined bevel; the round letters are kept
    exactly as drawn.
    """
    glyphs = []
    for character, contours in PLACED:
        if character == "T" and len(contours) == 1 and len(contours[0]) == 8:
            ring = [to_mark(p[0], p[1]) for p in contours[0]]
            cut = chamfer(ring, CHAMFER)
            glyphs.append((character, [[(x, y, True) for x, y in cut]]))
            continue
        mapped = []
        for contour in contours:
            mapped.append([to_mark(p[0], p[1]) + (p[2],) for p in contour])
        glyphs.append((character, mapped))
    return glyphs


GLYPHS = _glyph_contours()
GLYPH_CONTOURS = [c for _, contours in GLYPHS for c in contours]
GLYPH_BOX = {
    character: (
        min(p[0] for c in contours for p in c),
        min(p[1] for c in contours for p in c),
        max(p[0] for c in contours for p in c),
        max(p[1] for c in contours for p in c),
    )
    for character, contours in GLYPHS
}

WORD_PATH = "".join(
    contours_to_path(contours, lambda x, y: (x, y), 2) for _, contours in GLYPHS
)


def _t_outline():
    """The T's unbevelled outline in mark space, in glyph contour order.

    Segoe UI's ``T`` -- upright or italic -- is a plain eight-point octagon, so
    the index order below is stable and carries the whole frame of reference:
    ``[0]`` crossbar-bottom-right, ``[1]`` stem-top-right, ``[2]`` stem-bottom-right,
    ``[3]`` stem-bottom-left, ``[4]`` stem-top-left, ``[5]`` crossbar-bottom-left,
    ``[6]`` crossbar-top-left, ``[7]`` crossbar-top-right.
    """
    for character, contours in PLACED:
        if character == "T":
            return [to_mark(p[0], p[1]) for p in contours[0]]
    raise ValueError("the word has no T to build planes from")


T_RING = _t_outline()
if len(T_RING) != 8:
    raise ValueError("expected an eight-point T, got %d points" % len(T_RING))

# The two planes, and the three lines that carve them into disjoint regions.
BAR = [T_RING[6], T_RING[7], T_RING[0], T_RING[5]]     # crossbar quad
STEM = [T_RING[1], T_RING[2], T_RING[3], T_RING[4]]    # stem quad

BAR_BOTTOM_LINE = _line(T_RING[0], T_RING[5])
STEM_LEFT_LINE = _line(T_RING[3], T_RING[4])
STEM_RIGHT_LINE = _line(T_RING[1], T_RING[2])

_STEM_MID = _midpoint(T_RING[1], T_RING[3])
_STEM_FOOT = _midpoint(T_RING[2], T_RING[3])

# The four exclusive regions. All convex, so facets can be clipped straight into
# them: the bar either side of the stem, their screened overlap, and the stem
# below the crossbar.
BAR_LEFT = clip_halfplane(BAR, STEM_LEFT_LINE, _midpoint(T_RING[6], T_RING[5]))
BAR_RIGHT = clip_halfplane(BAR, STEM_RIGHT_LINE, _midpoint(T_RING[7], T_RING[0]))
OVERLAP = clip_halfplane(
    clip_halfplane(BAR, STEM_LEFT_LINE, _STEM_MID), STEM_RIGHT_LINE, _STEM_MID
)
STEM_ONLY = clip_halfplane(STEM, BAR_BOTTOM_LINE, _STEM_FOOT)

_STEM_TOP_Y = min(p[1] for p in STEM)
_STEM_BOTTOM_Y = max(p[1] for p in STEM)

# Signed distance below the crossbar, used for the contact shadow.
_CROSSBAR_SIGN = 1.0 if _side(BAR_BOTTOM_LINE, _STEM_FOOT) >= 0.0 else -1.0
_CROSSBAR_NORM = math.hypot(BAR_BOTTOM_LINE[0], BAR_BOTTOM_LINE[1]) or 1.0

# Colour regions: the T's two planes, plus one box per round letter. The exact
# letter shapes are cut by the silhouette mask, not by these boxes.
REGIONS = (
    (BAR_LEFT, "bar"),
    (BAR_RIGHT, "bar"),
    (OVERLAP, "overlap"),
    (STEM_ONLY, "stem"),
    ([(GLYPH_BOX["a"][0], GLYPH_BOX["a"][1]), (GLYPH_BOX["a"][2], GLYPH_BOX["a"][1]),
      (GLYPH_BOX["a"][2], GLYPH_BOX["a"][3]), (GLYPH_BOX["a"][0], GLYPH_BOX["a"][3])], "letter"),
    ([(GLYPH_BOX["c"][0], GLYPH_BOX["c"][1]), (GLYPH_BOX["c"][2], GLYPH_BOX["c"][1]),
      (GLYPH_BOX["c"][2], GLYPH_BOX["c"][3]), (GLYPH_BOX["c"][0], GLYPH_BOX["c"][3])], "letter"),
)

MESH_SPACING = 24.0
MESH_JITTER = 8.0
MESH_MARGIN = 24.0
MESH_FLOW = 0.20               # lattice shear: the triangles stream with the lean

# Depth. The word is a solid slab, not a decal: a copy of the silhouette pushed
# down-right carries the extrusion, so the face reads as something with a body.
EXTRUDE = (6.5, 7.5)
DEPTH_TINT = (9, 14, 36)       # what the side wall falls off toward
DEPTH_MIX = 0.55               # how far the side wall drops from the face

MARK_TOTAL_W = MARK_W + EXTRUDE[0]
MARK_TOTAL_H = MARK_H + EXTRUDE[1]
MESH_SEED = 11
RELAX_STEPS = 3
STROKE_WIDTH = 0.85
NODE_RADIUS = 1.05
RIM_WIDTH = 3.0
CORE_BLOOM = 44.0
CORE_BLEED = 92.0

# ------------------------------------------------------------------- palette

CYAN = (18, 190, 219)       # #12BEDB  deep cyan
BLUE = (37, 99, 235)        # #2563EB  royal blue
INDIGO = (67, 56, 202)      # #4338CA  indigo
VIOLET = (124, 58, 237)     # #7C3AED  violet

INK_DARK = (11, 18, 32)     # #0B1220  text on light
INK_LIGHT = (248, 250, 252)  # #F8FAFC text on dark
MUTED_DARK = (100, 116, 139)
MUTED_LIGHT = (148, 163, 184)
ACCENT_LIGHT = ((8, 145, 178), (124, 58, 237))   # deeper sweep, for light plates

SHEET_BG = (7, 11, 22)
CARD_DARK = (14, 20, 36)
CARD_LIGHT = (247, 249, 252)


def lerp(a, b, t):
    t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def screen(a, b):
    return tuple(255.0 - (255.0 - a[i]) * (255.0 - b[i]) / 255.0 for i in range(3))


def luminous(rgb, saturation=0.72, value=1.0):
    """Lift a screened colour to a vivid high-key tone (small HSV adjustment)."""
    r, g, b = (c / 255.0 for c in rgb)
    high = max(r, g, b)
    low = min(r, g, b)
    delta = high - low
    if delta <= 1e-6:
        return (value * 255.0,) * 3
    if high == r:
        hue = ((g - b) / delta) % 6.0
    elif high == g:
        hue = (b - r) / delta + 2.0
    else:
        hue = (r - g) / delta + 4.0
    hue /= 6.0
    sat = max(saturation, delta / high if high else 0.0)

    sector = int(hue * 6.0) % 6
    frac = hue * 6.0 - int(hue * 6.0)
    p = value * (1.0 - sat)
    q = value * (1.0 - frac * sat)
    t = value * (1.0 - (1.0 - frac) * sat)
    table = (
        (value, t, p), (q, value, p), (p, value, t),
        (p, q, value), (t, p, value), (value, p, q),
    )
    return tuple(c * 255.0 for c in table[sector])


def shade(rgb, factor):
    """Lighten (factor > 1) or darken (factor < 1) a colour."""
    if factor >= 1.0:
        return lerp(rgb, (255.0, 255.0, 255.0), min((factor - 1.0) * 1.35, 0.48))
    return tuple(max(0.0, c * factor) for c in rgb)


def sweep_colour(cx):
    """The word's left-to-right sweep: Tac runs cyan -> blue -> violet."""
    t = (cx - MARK_PAD) / (MARK_W - 2.0 * MARK_PAD)
    t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
    if t < 0.38:
        return lerp(CYAN, BLUE, t / 0.38)
    return lerp(BLUE, VIOLET, (t - 0.38) / 0.62)


def stem_colour(cx, cy):
    """The vertical plane: its own indigo->violet fall, pulled toward the sweep."""
    t = (cy - _STEM_TOP_Y) / (_STEM_BOTTOM_Y - _STEM_TOP_Y)
    return lerp(lerp(INDIGO, VIOLET, t), sweep_colour(cx), 0.42)


def _hash01(*values):
    h = 2166136261
    for value in values:
        h ^= int(round(value * 16.0)) & 0xFFFFFFFF
        h = (h * 16777619) & 0xFFFFFFFF
    return ((h >> 8) & 0xFFFFFF) / float(1 << 24)


def _normalize(vector):
    length = math.sqrt(sum(c * c for c in vector)) or 1.0
    return tuple(c / length for c in vector)


# ------------------------------------------------------------------ shading

LIGHT_DIR = _normalize((-0.50, -0.62, 0.60))
HALF_DIR = _normalize((LIGHT_DIR[0], LIGHT_DIR[1], LIGHT_DIR[2] + 1.0))

CORE_CENTRE = (
    sum(p[0] for p in OVERLAP) / len(OVERLAP),
    sum(p[1] for p in OVERLAP) / len(OVERLAP),
)


def height_field(x, y):
    """A shallow dome crossed by a slow ripple: the surface the facets sit on."""
    dx = (x - MARK_CX) / MARK_CX
    dy = (y - MARK_CY) / MARK_CY
    dome = 1.0 - 0.44 * (dx * dx + dy * dy)
    ripple = 0.19 * math.sin((x * 0.85 + y) / 54.0)
    return dome + ripple


def contact_shade(point):
    """Contact shadow where the vertical plane slides out from under the bar.

    Without it the two planes just abut; with it the mark reads as two stacked
    translucent slabs, which is the whole point of the composition. Measured
    against the crossbar's underside, which is a slanted line in an italic.
    """
    distance = _side(BAR_BOTTOM_LINE, point) * _CROSSBAR_SIGN / _CROSSBAR_NORM
    if distance <= 0.0:
        return 1.0
    return 0.72 + 0.28 * min(1.0, distance / 36.0)


def facet_lighting(cx, cy, slope=46.0, sharpness=18.0, grain=0.12):
    """Per-facet shading: smooth sheen, grainy diffuse.

    ``slope`` scales the tiny height-field gradient into a usable normal. The
    diffuse term uses a per-facet normal tilt -- real low-poly surfaces are
    irregular, and that is what makes neighbouring faces step against each other.
    The specular term deliberately uses the *smooth* normal instead, so the
    highlight reads as one coherent band of light rather than as sparkle.
    """
    eps = 2.0
    dzdx = (height_field(cx + eps, cy) - height_field(cx - eps, cy)) / (2.0 * eps) * slope
    dzdy = (height_field(cx, cy + eps) - height_field(cx, cy - eps)) / (2.0 * eps) * slope
    smooth = _normalize((-dzdx, -dzdy, 1.0))
    faceted = _normalize(
        (
            -dzdx + (_hash01(cx, cy) - 0.5) * grain,
            -dzdy + (_hash01(cy, cx, 7.0) - 0.5) * grain,
            1.0,
        )
    )
    diffuse = max(0.0, sum(faceted[i] * LIGHT_DIR[i] for i in range(3)))
    specular = max(0.0, sum(smooth[i] * HALF_DIR[i] for i in range(3))) ** sharpness
    return diffuse, specular


# --------------------------------------------------------------- tessellate

def tidy(poly, epsilon=0.05):
    """Drop duplicate/degenerate vertices left behind by clipping."""
    out = []
    for point in poly:
        if not out or abs(point[0] - out[-1][0]) > epsilon or abs(point[1] - out[-1][1]) > epsilon:
            out.append(point)
    if len(out) > 1 and abs(out[0][0] - out[-1][0]) <= epsilon and abs(out[0][1] - out[-1][1]) <= epsilon:
        out.pop()
    if len(out) < 3:
        return []
    area = 0.0
    for index, point in enumerate(out):
        nxt = out[(index + 1) % len(out)]
        area += point[0] * nxt[1] - nxt[0] * point[1]
    return out if abs(area) > 1.0 else []


def _circumcircle(a, b, c):
    ax, ay = a
    bx, by = b
    cx, cy = c
    d = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by))
    if abs(d) < 1e-9:
        return None
    a2 = ax * ax + ay * ay
    b2 = bx * bx + by * by
    c2 = cx * cx + cy * cy
    ux = (a2 * (by - cy) + b2 * (cy - ay) + c2 * (ay - by)) / d
    uy = (a2 * (cx - bx) + b2 * (ax - cx) + c2 * (bx - ax)) / d
    return (ux, uy, (ax - ux) ** 2 + (ay - uy) ** 2)


def delaunay_indices(points):
    """Bowyer-Watson Delaunay triangulation of ``points`` -> index triples."""
    pts = list(points)
    count = len(pts)
    if count < 3:
        return []
    min_x = min(p[0] for p in pts)
    max_x = max(p[0] for p in pts)
    min_y = min(p[1] for p in pts)
    max_y = max(p[1] for p in pts)
    span = max(max_x - min_x, max_y - min_y) or 1.0
    mid_x = (min_x + max_x) / 2.0
    mid_y = (min_y + max_y) / 2.0
    pts.append((mid_x - 24.0 * span, mid_y - span))
    pts.append((mid_x, mid_y + 24.0 * span))
    pts.append((mid_x + 24.0 * span, mid_y - span))

    triangles = [(count, count + 1, count + 2)]
    for index in range(count):
        px, py = pts[index]
        bad = []
        for tri in triangles:
            circle = _circumcircle(pts[tri[0]], pts[tri[1]], pts[tri[2]])
            if circle is None:
                continue
            if (px - circle[0]) ** 2 + (py - circle[1]) ** 2 <= circle[2] + 1e-6:
                bad.append(tri)
        edges = {}
        for tri in bad:
            for edge in ((tri[0], tri[1]), (tri[1], tri[2]), (tri[2], tri[0])):
                key = (edge[0], edge[1]) if edge[0] < edge[1] else (edge[1], edge[0])
                edges[key] = edges.get(key, 0) + 1
        bad_set = set(bad)
        triangles = [tri for tri in triangles if tri not in bad_set]
        for (a, b), uses in edges.items():
            if uses == 1:
                triangles.append((a, b, index))

    return [tri for tri in triangles if all(i < count for i in tri)]


def delaunay(points):
    """Delaunay triangulation of ``points`` -> coordinate triples."""
    return [tuple(points[i] for i in tri) for tri in delaunay_indices(points)]


def mesh_points(spacing=None, jitter=None, margin=None, seed=None):
    """Irregular vertex cloud: silhouette corners, edge midpoints, jittered grid."""
    spacing = MESH_SPACING if spacing is None else spacing
    jitter = MESH_JITTER if jitter is None else jitter
    margin = MESH_MARGIN if margin is None else margin
    seed = MESH_SEED if seed is None else seed
    rng = random.Random(seed)

    # Anchor the letters' outlines, thinned to roughly one point per cell, so the
    # mesh meets every curve instead of crossing it.
    raw = []
    step2 = (spacing * 0.55) ** 2
    for contour in GLYPH_CONTOURS:
        for point in contour:
            candidate = (point[0], point[1])
            if all(
                (candidate[0] - q[0]) ** 2 + (candidate[1] - q[1]) ** 2 > step2
                for q in raw
            ):
                raw.append(candidate)

    left = -margin
    right = MARK_W + margin
    top = -margin
    bottom = MARK_H + margin
    cols = max(2, int(round((right - left) / spacing)))
    rows = max(2, int(round((bottom - top) / spacing)))
    for row in range(rows + 1):
        for col in range(cols + 1):
            grid_x = left + col * (right - left) / cols
            grid_y = top + row * (bottom - top) / rows
            # Shear the lattice with the letters, so the triangulation streams
            # along the lean instead of reading as a static honeycomb.
            raw.append(
                (
                    grid_x + (grid_y - MARK_BASELINE) * MESH_FLOW + rng.uniform(-jitter, jitter),
                    grid_y + rng.uniform(-jitter, jitter),
                )
            )

    unique = []
    for point in raw:
        if all((point[0] - q[0]) ** 2 + (point[1] - q[1]) ** 2 > 64.0 for q in unique):
            unique.append(point)
    return unique


def _inside_convex(poly, point):
    """Inside test for a convex polygon wound consistently."""
    centre = (
        sum(p[0] for p in poly) / len(poly),
        sum(p[1] for p in poly) / len(poly),
    )
    for index, vertex in enumerate(poly):
        line = _line(vertex, poly[(index + 1) % len(poly)])
        if _side(line, point) * _side(line, centre) < 0.0:
            return False
    return True


def _inside_word(point):
    """Even-odd point test over every glyph contour, so counters stay hollow."""
    for _, contours in GLYPHS:
        crossings = 0
        for contour in contours:
            x, y = point
            count = len(contour)
            for index in range(count):
                ax, ay = contour[index][0], contour[index][1]
                bx, by = contour[(index + 1) % count][0], contour[(index + 1) % count][1]
                if (ay > y) != (by > y):
                    if x < ax + (y - ay) / (by - ay) * (bx - ax):
                        crossings += 1
        if crossings % 2 == 1:
            return True
    return False


_MESH_CACHE = {}


def relaxed_points(spacing=None, jitter=None, margin=None, seed=None, steps=RELAX_STEPS):
    """Jittered cloud, then Lloyd-relaxed inside the mark.

    Each free vertex moves to the average of its incident triangle circumcentres
    -- the Voronoi centroid. That is what turns a lumpy random point set into the
    evenly graded triangulation a real mesher would hand you, and it is the main
    reason the mark reads as engineering rather than noise.
    """
    key = (spacing, jitter, margin, seed, steps)
    if key in _MESH_CACHE:
        return _MESH_CACHE[key]

    points = mesh_points(spacing, jitter, margin, seed)
    movable = [index for index, point in enumerate(points) if _inside_word(point)]
    if movable and steps > 0:
        free = set(movable)
        for _ in range(steps):
            gather = {index: [] for index in movable}
            for tri in delaunay_indices(points):
                centre = _circumcircle(points[tri[0]], points[tri[1]], points[tri[2]])
                if centre is None:
                    continue
                for vertex in tri:
                    if vertex in free:
                        gather[vertex].append((centre[0], centre[1]))
            for index in movable:
                centres = gather[index]
                if centres:
                    points[index] = (
                        sum(c[0] for c in centres) / len(centres),
                        sum(c[1] for c in centres) / len(centres),
                    )

    _MESH_CACHE[key] = points
    return points


def mesh_vertices(steps=RELAX_STEPS):
    """Relaxed interior vertices -- drawn as nodes on the finished mark."""
    return [p for p in relaxed_points(steps=steps) if _inside_word(p)]


def build_facets(spacing=None, jitter=None, margin=None, seed=None, steps=RELAX_STEPS):
    """Flat-shaded facets of the mark, in paint order.

    Every mesh triangle is clipped against the exclusive colour regions, so the
    overlap can be screened once with no blend modes required in any renderer.
    """
    margin = MESH_MARGIN if margin is None else margin
    facets = []
    for triangle in delaunay(relaxed_points(spacing, jitter, margin, seed, steps)):
        cx = sum(p[0] for p in triangle) / 3.0
        cy = sum(p[1] for p in triangle) / 3.0
        if not (
            -margin <= cx <= MARK_W + margin and -margin <= cy <= MARK_H + margin
        ):
            continue
        diffuse, specular = facet_lighting(cx, cy)
        grad = 1.0 + 0.05 * (_hash01(cx, cy) - 0.5)
        for region, kind in REGIONS:
            if kind == "letter" and (
                _inside_convex(BAR, (cx, cy)) or _inside_convex(STEM, (cx, cy))
            ):
                continue
            poly = tidy(clip_convex(list(triangle), region))
            if not poly:
                continue
            if kind == "overlap":
                # The T's two translucent planes screening together. Keep the core
                # smooth and hot so it reads as a lumen, not as more facet noise.
                rgb = luminous(screen(sweep_colour(cx), stem_colour(cx, cy)))
                bloom = max(
                    0.0,
                    1.0 - math.hypot(cx - CORE_CENTRE[0], cy - CORE_CENTRE[1]) / CORE_BLOOM,
                )
                rgb = lerp(rgb, (190.0, 235.0, 255.0), 0.46 * bloom * bloom)
                local = (1.20 + 0.10 * diffuse) * grad
            else:
                rgb = stem_colour(cx, cy) if kind == "stem" else sweep_colour(cx)
                local = (0.74 + 0.48 * diffuse) * grad
                if kind == "stem":
                    local *= contact_shade((cx, cy))
                # Light from the composited core bleeds into the planes around it:
                # additive blending does not respect the parent rectangle, and
                # showing that is what sells the composite.
                bleed = max(
                    0.0,
                    1.0 - math.hypot(cx - CORE_CENTRE[0], cy - CORE_CENTRE[1]) / CORE_BLEED,
                )
                rgb = lerp(rgb, (150.0, 215.0, 255.0), 0.22 * bleed * bleed)
            rgb = shade(rgb, local)
            rgb = lerp(rgb, (255.0, 255.0, 255.0), min(0.30, specular * 0.45))
            # Edge treatment follows the facet's own exposure: faces turned into
            # the light get a bright bevel, faces turned away get a dark crease.
            # That is what makes the surface read as faceted crystal rather than
            # as a wireframe laid over a gradient.
            edge = 0.62 + 0.42 * diffuse
            facets.append(
                {
                    "poly": poly,
                    "fill": rgb,
                    "stroke": shade(rgb, edge),
                    "depth": min(1.0, max(0.0, local - 0.55)),
                    "spec": specular,
                    "centre": (cx, cy),
                }
            )
    return facets


FACETS = build_facets()
NODES = mesh_vertices()

# Two sparkles, placed automatically: the composited core, and the hardest-lit
# facet far enough away from it to read as a separate hit.
def _brightest_facet():
    core = CORE_CENTRE
    candidates = [
        f for f in FACETS
        if math.hypot(f["centre"][0] - core[0], f["centre"][1] - core[1]) > 70.0
    ] or FACETS
    return max(candidates, key=lambda f: f["spec"])["centre"]


GLINTS = [
    (CORE_CENTRE[0], CORE_CENTRE[1], 14.0),
    (_brightest_facet()[0], _brightest_facet()[1], 9.5),
]


# ------------------------------------------------------------------ SVG bits

def rgb255(rgb):
    return tuple(int(round(max(0.0, min(255.0, c)))) for c in rgb)


def hexc(rgb):
    return "#%02X%02X%02X" % rgb255(rgb)


def points_attr(poly, precision=2):
    fmt = "%%.%df,%%.%df" % (precision, precision)
    return " ".join(fmt % (x, y) for x, y in poly)


def depth_facets(facets):
    """The extruded side wall: the same tessellation, dropped into shadow.

    Tinting each facet's own colour (rather than filling one flat slab) is what
    keeps the extrusion reading as solid geometry instead of as a drop shadow.
    """
    out = []
    for facet in facets:
        wall = lerp(facet["fill"], DEPTH_TINT, DEPTH_MIX)
        out.append((facet["poly"], wall, shade(wall, 0.74)))
    return out


def mark_svg_group(mono=None, indent="  ", facets=None, suite=0):
    facets = FACETS if facets is None else facets
    clip_id = "tacui-clip-%d" % suite
    rim_id = "tacui-rim-%d" % suite

    def wall_lines(inner):
        lines = [
            '%s<g transform="translate(%g %g)" clip-path="url(#%s)">'
            % (indent, EXTRUDE[0], EXTRUDE[1], clip_id),
            '%s  <g fill-opacity="%g">' % (indent, 1.0 if mono is None else 0.38),
        ]
        for poly, fill, stroke in (
            depth_facets(facets)
            if mono is None
            else [(f["poly"], mono, mono) for f in facets]
        ):
            lines.append(
                '%s    <polygon points="%s" fill="%s" stroke="%s"/>'
                % (indent, points_attr(poly), hexc(fill), hexc(stroke))
            )
        lines.append("%s  </g>" % indent)
        lines.append("%s</g>" % indent)
        return lines

    if mono is not None:
        # Monochrome is a knocked-out mesh: solid silhouette minus the wireframe,
        # so it stays legible on any background.
        mask_id = "tacui-mono-mesh-%d" % suite
        lines = [
            "%s<defs>" % indent,
            '%s  <clipPath id="%s"><path d="%s" clip-rule="evenodd"/></clipPath>'
            % (indent, clip_id, WORD_PATH),
            '%s  <mask id="%s" maskUnits="userSpaceOnUse" x="-4" y="-4" width="%g" height="%g">'
            % (indent, mask_id, MARK_W + 8, MARK_H + 8),
            '%s    <path d="%s" fill="#FFFFFF" fill-rule="evenodd"/>' % (indent, WORD_PATH),
            '%s    <g stroke="#000000" stroke-width="%g" stroke-linejoin="round" fill="none">'
            % (indent, STROKE_WIDTH),
        ]
        for facet in facets:
            lines.append('%s      <polygon points="%s"/>' % (indent, points_attr(facet["poly"])))
        lines.append("%s    </g>" % indent)
        lines.append("%s  </mask>" % indent)
        lines.append("%s</defs>" % indent)
        lines.extend(wall_lines(False))
        lines.append(
            '%s<rect x="0" y="0" width="%g" height="%g" fill="%s" mask="url(#%s)"/>'
            % (indent, MARK_W, MARK_H, hexc(mono), mask_id)
        )
        return "\n".join(lines)

    lines = [
        "%s<defs>" % indent,
        '%s  <clipPath id="%s"><path d="%s" clip-rule="evenodd"/></clipPath>'
        % (indent, clip_id, WORD_PATH),
        '%s  <linearGradient id="%s" gradientUnits="userSpaceOnUse" '
        'x1="%g" y1="%g" x2="%g" y2="%g">'
        % (indent, rim_id, MARK_PAD * 0.6, MARK_PAD * 0.4, MARK_W * 0.62, MARK_H),
        '%s    <stop offset="0" stop-color="#FFFFFF" stop-opacity="1"/>' % indent,
        '%s    <stop offset="0.42" stop-color="#FFFFFF" stop-opacity="0.24"/>' % indent,
        '%s    <stop offset="1" stop-color="#FFFFFF" stop-opacity="0.04"/>' % indent,
        '%s  </linearGradient>' % indent,
        "%s</defs>" % indent,
    ]
    lines.extend(wall_lines(True))
    lines.append('%s<g clip-path="url(#%s)">' % (indent, clip_id))
    for facet in facets:
        lines.append(
            '%s  <polygon points="%s" fill="%s" stroke="%s"/>'
            % (indent, points_attr(facet["poly"]), hexc(facet["fill"]), hexc(facet["stroke"]))
        )
    for x, y in NODES:
        lines.append(
            '%s  <circle cx="%.2f" cy="%.2f" r="%g" fill="#FFFFFF" fill-opacity="0.30"/>'
            % (indent, x, y, NODE_RADIUS)
        )
    lines.append(
        '%s  <path d="%s" fill="none" stroke="url(#%s)" stroke-width="%g" '
        'stroke-linejoin="round"/>' % (indent, WORD_PATH, rim_id, RIM_WIDTH)
    )
    for x, y, radius in GLINTS:
        lines.append(
            '%s  <polygon points="%s" fill="#FFFFFF" fill-opacity="0.80"/>'
            % (indent, points_attr(star(x, y, radius), 2))
        )
    lines.append("%s</g>" % indent)
    return "\n".join(lines)


def star(cx, cy, radius, waist=0.20, points=4, rotation=0.0):
    """A four-point sparkle, the kind a specular hit leaves behind."""
    out = []
    for index in range(points * 2):
        angle = rotation + index * math.pi / points
        reach = radius if index % 2 == 0 else radius * waist
        out.append((cx + reach * math.cos(angle), cy + reach * math.sin(angle)))
    return out


def svg_document(width, height, body, defs=""):
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" width="%g" height="%g" '
        'viewBox="0 0 %g %g" fill="none" role="img">\n'
        "%s%s\n</svg>\n" % (width, height, width, height, defs, body)
    )


# --------------------------------------------------------------- wordmark

class Wordmark:
    """Lays out text runs from the real font metrics (shared by SVG and PNG)."""

    def __init__(self, path):
        self.path = path
        self.font = TrueTypeFont(path)
        self.cap = self.font.cap_height_units() / self.font.units_per_em

    def em_for_cap(self, cap_height):
        return cap_height / self.cap

    def scale(self, em):
        return em / self.font.units_per_em

    def advance(self, character, em, tracking=0.0):
        return self.font.advance_units(character) * self.scale(em) + tracking

    def run_width(self, text, em, tracking=0.0):
        if not text:
            return 0.0
        return sum(self.advance(c, em, tracking) for c in text) - tracking

    def place(self, text, em, x, baseline, tracking=0.0):
        """Return ``[(character, path_d, pen_x)]`` plus the ending pen position."""
        placed = []
        pen = x
        for character in text:
            contours = self.font.contours(self.font.glyph_id(character))
            if contours:
                d = contours_to_path(
                    contours,
                    lambda px, py, pen=pen, baseline=baseline, s=self.scale(em): (
                        pen + px * s,
                        baseline - py * s,
                    ),
                    2,
                )
                if d:
                    placed.append((character, d, pen))
            pen += self.advance(character, em, tracking)
        return placed, pen - tracking

    def ink_bounds(self, text, em, x, baseline, tracking=0.0):
        """Ink bounding box of a text run, in the destination coordinate space."""
        s = self.scale(em)
        pen = x
        xs, ys = [], []
        for character in text:
            contours = self.font.contours(self.font.glyph_id(character))
            for contour in contours:
                for px, py, _ in contour:
                    xs.append(pen + px * s)
                    ys.append(baseline - py * s)
            pen += self.advance(character, em, tracking)
        if not xs:
            return None
        return (min(xs), min(ys), max(xs), max(ys))


BOLD = Wordmark(FONT_BOLD)
BOLD_ITALIC = Wordmark(FONT_BOLD_ITALIC)
REGULAR = Wordmark(FONT_REGULAR)


def lighten(layout):
    """Return a copy of ``layout`` recoloured for a light background."""
    for element in layout["elements"]:
        if element["type"] != "text":
            continue
        if element.get("fill") == INK_LIGHT:
            element["fill"] = INK_DARK
        elif element.get("fill") == MUTED_LIGHT:
            element["fill"] = MUTED_DARK
        if element.get("gradient") == (CYAN, VIOLET):
            element["gradient"] = ACCENT_LIGHT
    return layout


# ------------------------------------------------------- atmosphere elements

GLASS = (168, 214, 255)
STREAK = (150, 200, 255)


def _streaks(width, height, seed, count):
    """Thin horizontal streaks -- the GPU-acceleration motif."""
    rng = random.Random(seed)
    lines = []
    for _ in range(count):
        span = rng.uniform(width * 0.05, width * 0.38)
        start = rng.uniform(-width * 0.06, width * 0.94)
        y = rng.uniform(height * 0.03, height * 0.97)
        lines.append(
            (
                start, y, start + span, y,
                rng.uniform(0.05, 0.22),
                rng.uniform(1.0, 2.4),
            )
        )
    return {"type": "streaks", "lines": lines, "colour": STREAK}


def _glow(cx, cy, rx, ry, colour, opacity):
    """A soft radial bloom: light the scene without drawing a shape."""
    return {
        "type": "glow", "cx": cx, "cy": cy, "rx": rx, "ry": ry,
        "colour": colour, "opacity": opacity,
    }


def _glass(cx, cy, rx, ry, fill=0.05, edge=0.20, sheen=0.10):
    """A translucent pane: the composited layer, made visible."""
    return {
        "type": "glass", "cx": cx, "cy": cy, "rx": rx, "ry": ry,
        "fill": fill, "edge": edge, "sheen": sheen,
    }


def radial_falloff(width, height, cx, cy, rx, ry, power=1.8):
    """Numpy radial ramp in pixels, 1 at the centre and 0 at the rim."""
    yy, xx = np.mgrid[0:height, 0:width]
    distance = np.sqrt(
        ((xx - cx) / max(rx, 1.0)) ** 2 + ((yy - cy) / max(ry, 1.0)) ** 2
    )
    return np.clip(1.0 - distance, 0.0, 1.0) ** power


# ------------------------------------------------------------------ layouts

def mark_box(cap):
    """Display size of the mark when its cap height is ``cap``."""
    scale = cap / MARK_CAP
    return MARK_TOTAL_W * scale, MARK_TOTAL_H * scale, MARK_BASELINE * scale


def _text_run(text, em, tracking, gradient=None, fill=None, face=None):
    return {
        "type": "text", "text": text, "face": face or BOLD_ITALIC, "em": em,
        "tracking": tracking, "gradient": gradient, "fill": fill,
    }


def layout_mark(cap=MARK_CAP, mono=None):
    width, height, _ = mark_box(cap)
    return {
        "width": width,
        "height": height,
        "elements": [
            {"type": "mark", "x": 0.0, "y": 0.0, "scale": cap / MARK_CAP, "mono": mono}
        ],
    }


def layout_wordmark(cap=88, tracking=None):
    """Type-only logo: the full name, for places the mark cannot go."""
    em = BOLD_ITALIC.em_for_cap(cap)
    if tracking is None:
        tracking = -0.012 * em
    bounds = BOLD_ITALIC.ink_bounds("TacUI", em, 0.0, 0.0, tracking)
    pad = em * 0.06
    x = pad - bounds[0]
    baseline = pad + cap
    return {
        "width": (bounds[2] - bounds[0]) + 2 * pad,
        "height": cap + 2 * pad,
        "elements": [
            dict(_text_run("Tac", em, tracking, fill=INK_LIGHT), x=x, baseline=baseline),
            dict(
                _text_run("UI", em, tracking, gradient=(CYAN, VIOLET)),
                x=x + BOLD_ITALIC.run_width("Tac", em, tracking), baseline=baseline,
            ),
        ],
    }


def layout_horizontal(cap=86, gap=0.10, pad=6):
    """The mark's "Tac" and the type's "UI" share a baseline and a cap height, so
    the lockup reads as one word rather than a symbol sitting next to a name."""
    em = BOLD_ITALIC.em_for_cap(cap)
    tracking = -0.012 * em
    scale = cap / MARK_CAP
    mark_w, mark_h, baseline = mark_box(cap)
    ink_right = (MARK_W - MARK_PAD + EXTRUDE[0]) * scale
    ui_bounds = BOLD_ITALIC.ink_bounds("UI", em, 0.0, 0.0, tracking)
    ui_x = ink_right + gap * cap - ui_bounds[0]
    overshoot = max(0.0, ui_bounds[3])
    return {
        "width": ui_x + ui_bounds[2] + pad,
        "height": max(mark_h, baseline + overshoot) + pad,
        "elements": [
            {"type": "mark", "x": 0.0, "y": 0.0, "scale": scale, "mono": None},
            dict(
                _text_run("UI", em, tracking, gradient=(CYAN, VIOLET)),
                x=ui_x, baseline=baseline,
            ),
        ],
    }


def layout_horizontal_tagline(cap=80, gap=0.10, pad=6):
    base = layout_horizontal(cap=cap, gap=gap, pad=pad)
    ui = base["elements"][1]
    tag_em = REGULAR.em_for_cap(cap * 0.24)
    tag_tracking = 0.14 * tag_em
    tag = "TESSELLATED ACCELERATED COMPOSITION LIBRARY"
    tag_x = ui["x"]
    tag_baseline = ui["baseline"] + cap * 0.72
    tag_width = REGULAR.run_width(tag, tag_em, tag_tracking)
    base["height"] = max(base["height"], tag_baseline + tag_em * 0.22 + pad)
    base["width"] = max(base["width"], tag_x + tag_width + pad)
    base["elements"].append(
        {
            "type": "text", "text": tag, "face": REGULAR, "em": tag_em,
            "x": tag_x, "baseline": tag_baseline, "tracking": tag_tracking, "fill": MUTED_LIGHT,
        }
    )
    return base


def layout_stacked(cap=104, gap=0.16, pad=14):
    """Mark above type, both centred, with the tagline underneath."""
    em = BOLD_ITALIC.em_for_cap(cap)
    tracking = -0.012 * em
    scale = cap / MARK_CAP
    mark_w, mark_h, baseline = mark_box(cap)
    ui_bounds = BOLD_ITALIC.ink_bounds("UI", em, 0.0, 0.0, tracking)
    ui_ink_w = ui_bounds[2] - ui_bounds[0]

    tag_em = REGULAR.em_for_cap(cap * 0.16)
    tag_tracking = 0.105 * tag_em
    tag = "TRIANGULATION  ·  GPU  ·  COMPOSITING"
    tag_width = REGULAR.run_width(tag, tag_em, tag_tracking)

    width = max(mark_w, ui_ink_w, tag_width) + 2 * pad
    ui_baseline = mark_h + gap * cap + cap
    tag_baseline = ui_baseline + cap * 0.70
    return {
        "width": width,
        "height": tag_baseline + tag_em * 0.26 + pad,
        "elements": [
            {"type": "mark", "x": (width - mark_w) / 2.0, "y": 0.0, "scale": scale, "mono": None},
            dict(
                _text_run("UI", em, tracking, gradient=(CYAN, VIOLET)),
                x=(width - ui_ink_w) / 2.0 - ui_bounds[0], baseline=ui_baseline,
            ),
            {
                "type": "text", "text": tag, "face": REGULAR, "em": tag_em,
                "x": (width - tag_width) / 2.0, "baseline": tag_baseline,
                "tracking": tag_tracking, "fill": MUTED_LIGHT,
            },
        ],
    }


def layout_hero(width=1600, height=560, cap=206.0):
    """Showcase banner: the lockup sitting inside glass, with light streaking past.

    This is the one place the atmosphere elements belong. The mark itself stays
    clean so it still survives a 60px favicon.
    """
    base = layout_horizontal(cap=cap, gap=0.12, pad=0)
    tag_em = REGULAR.em_for_cap(22.0)
    tag_tracking = 0.22 * tag_em
    tag = "TESSELLATED ACCELERATED COMPOSITION LIBRARY"
    tag_width = REGULAR.run_width(tag, tag_em, tag_tracking)

    lock_w = base["width"]
    lock_h = base["height"]
    lock_x = (width - lock_w) / 2.0
    block = lock_h + 92.0 + tag_em * 0.35
    lock_y = max(36.0, (height - block) / 2.0)
    centre_x = width / 2.0
    centre_y = lock_y + lock_h / 2.0
    tag_baseline = lock_y + lock_h + 92.0

    elements = [
        _streaks(width, height, 20260406, 26),
        _glow(centre_x, centre_y, 560.0, 300.0, (56, 140, 255), 0.46),
        _glass(centre_x - lock_w * 0.21, centre_y, 430.0, 300.0, 0.055, 0.11, 0.13),
        _glass(centre_x + lock_w * 0.24, centre_y + 14.0, 300.0, 215.0, 0.035, 0.08, 0.09),
    ]
    for element in base["elements"]:
        shifted = dict(element)
        shifted["x"] = element["x"] + lock_x
        if element["type"] == "mark":
            shifted["y"] = element["y"] + lock_y
        else:
            shifted["baseline"] = element["baseline"] + lock_y
        elements.append(shifted)

    elements.append(
        {
            "type": "text", "text": tag, "face": REGULAR, "em": tag_em,
            "x": (width - tag_width) / 2.0, "baseline": tag_baseline,
            "tracking": tag_tracking, "fill": MUTED_LIGHT,
        }
    )
    return {"width": width, "height": height, "elements": elements}


# ---------------------------------------------------------------- SVG output
def emit_svg(layout, defs="", body_attrs="", background=None):
    parts = []
    if body_attrs:
        parts.append(body_attrs)
    if background is not None:
        parts.append(
            '<rect width="%g" height="%g" fill="%s"/>'
            % (layout["width"], layout["height"], hexc(background))
        )
    for index, element in enumerate(layout["elements"]):
        if element["type"] == "mark":
            parts.append(
                '<g transform="translate(%.3f %.3f) scale(%.5f)">'
                % (element["x"], element["y"], element["scale"])
            )
            parts.append(
                mark_svg_group(
                    mono=element.get("mono"),
                    indent="    ",
                    suite=index,
                )
            )
            parts.append("</g>")
        elif element["type"] == "text":
            face = element["face"]
            tracking = element.get("tracking", 0.0)
            placed, _ = face.place(
                element["text"], element["em"], element["x"], element["baseline"], tracking
            )
            fill = element.get("fill")
            gradient = element.get("gradient")
            if gradient:
                bounds = face.ink_bounds(
                    element["text"], element["em"], element["x"], element["baseline"], tracking
                )
                gid = "tacui-text-%d" % len(parts)
                defs += (
                    '<linearGradient id="%s" gradientUnits="userSpaceOnUse" x1="%.2f" y1="%.2f" '
                    'x2="%.2f" y2="%.2f"><stop offset="0" stop-color="%s"/>'
                    '<stop offset="1" stop-color="%s"/></linearGradient>'
                    % (gid, bounds[0], bounds[1], bounds[2], bounds[3], hexc(gradient[0]), hexc(gradient[1]))
                )
                paint = 'fill="url(#%s)"' % gid
            else:
                paint = 'fill="%s"' % hexc(fill)
            parts.append('<g %s fill-rule="nonzero">' % paint)
            for _, d, _ in placed:
                parts.append('  <path d="%s"/>' % d)
            parts.append("</g>")
        elif element["type"] == "rect":
            parts.append(
                '<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" rx="%.2f" fill="%s"/>'
                % (element["x"], element["y"], element["width"], element["height"],
                   element.get("rx", 0.0), element.get("fill", "none"))
            )
        elif element["type"] == "streaks":
            parts.append('<g stroke="%s" stroke-linecap="round">' % hexc(element["colour"]))
            for x0, y0, x1, y1, alpha, thick in element["lines"]:
                parts.append(
                    '  <line x1="%.2f" y1="%.2f" x2="%.2f" y2="%.2f" '
                    'stroke-opacity="%.3f" stroke-width="%.2f"/>' % (x0, y0, x1, y1, alpha, thick)
                )
            parts.append("</g>")
        elif element["type"] == "glow":
            gid = "tacui-glow-%d" % len(parts)
            defs += (
                '<radialGradient id="%s" gradientUnits="userSpaceOnUse" cx="%.2f" cy="%.2f" '
                'r="%.2f"><stop offset="0" stop-color="%s" stop-opacity="%.3f"/>'
                '<stop offset="1" stop-color="%s" stop-opacity="0"/></radialGradient>'
                % (gid, element["cx"], element["cy"], element["rx"],
                   hexc(element["colour"]), element["opacity"], hexc(element["colour"]))
            )
            parts.append(
                '<ellipse cx="%.2f" cy="%.2f" rx="%.2f" ry="%.2f" fill="url(#%s)"/>'
                % (element["cx"], element["cy"], element["rx"], element["ry"], gid)
            )
        elif element["type"] == "glass":
            parts.append(
                '<ellipse cx="%.2f" cy="%.2f" rx="%.2f" ry="%.2f" fill="%s" '
                'fill-opacity="%.3f" stroke="%s" stroke-opacity="%.3f" stroke-width="1.4"/>'
                % (element["cx"], element["cy"], element["rx"], element["ry"],
                   hexc(GLASS), element["fill"], hexc(GLASS), element["edge"])
            )
            sheen_id = "tacui-sheen-%d" % len(parts)
            defs += (
                '<linearGradient id="%s" gradientUnits="userSpaceOnUse" x1="%.2f" y1="%.2f" '
                'x2="%.2f" y2="%.2f"><stop offset="0" stop-color="#FFFFFF" '
                'stop-opacity="%.3f"/><stop offset="0.55" stop-color="#FFFFFF" '
                'stop-opacity="0"/></linearGradient>'
                % (sheen_id, element["cx"] - element["rx"], element["cy"] - element["ry"],
                   element["cx"], element["cy"] + element["ry"], element["sheen"])
            )
            parts.append(
                '<ellipse cx="%.2f" cy="%.2f" rx="%.2f" ry="%.2f" fill="url(#%s)"/>'
                % (element["cx"], element["cy"], element["rx"], element["ry"], sheen_id)
            )
    defs_block = "<defs>%s</defs>\n" % defs if defs else ""
    return svg_document(layout["width"], layout["height"], "\n".join(parts), defs_block)


# --------------------------------------------------------------- PNG output

def diagonal_falloff(width, height):
    """Alpha ramp that is bright at the top-left and gone by the bottom-right."""
    yy, xx = np.mgrid[0:height, 0:width]
    t = (xx / max(1.0, width - 1.0) + yy / max(1.0, height - 1.0)) / 2.0
    values = np.clip(1.0 - t * 1.7, 0.0, 1.0) * 255.0
    return Image.fromarray(values.astype("uint8"), "L")


def sweep_backdrop(width, height):
    """A flat word-wide sweep, used to backfill any pixel the facets miss.

    Region clipping can drop hairline slivers along a letter's edge; without a
    backfill those would surface as opaque black once the mask is applied.
    """
    strip = Image.new("RGB", (width, 1))
    pixels = strip.load()
    for x in range(width):
        pixels[x, 0] = rgb255(sweep_colour(x * MARK_W / width))
    return strip.resize((width, height), Image.NEAREST).convert("RGBA")


def word_mask(width, height, scale):
    """Rasterised silhouette of the word, with letter counters kept hollow."""
    mask = Image.new("L", (width, height), 0)
    draw = ImageDraw.Draw(mask)
    shells, holes = [], []
    for _, contours in GLYPHS:
        if not contours:
            continue
        areas = [_signed_area([(p[0], p[1]) for p in c]) for c in contours]
        largest = max(range(len(areas)), key=lambda i: abs(areas[i]))
        shell_sign = 1.0 if areas[largest] > 0 else -1.0
        for contour, area in zip(contours, areas):
            points = [(x * scale, y * scale) for x, y in contour_polyline(contour)]
            if len(points) < 3:
                continue
            (shells if (area > 0) == (shell_sign > 0) else holes).append(points)
    for points in shells:
        draw.polygon(points, fill=255)
    for points in holes:
        draw.polygon(points, fill=0)
    return mask


def depth_backdrop(width, height):
    """Diagonal ramp for the extruded side wall: lit near the face, dark far off."""
    yy, xx = np.mgrid[0:height, 0:width]
    t = np.clip(
        xx / max(1.0, width - 1.0) * 0.35 + yy / max(1.0, height - 1.0) * 0.65, 0.0, 1.0
    )
    near = np.array(DEPTH_NEAR, dtype=float)
    far = np.array(DEPTH_FAR, dtype=float)
    ramp = near[None, None, :] + (far - near)[None, None, :] * t[:, :, None]
    return Image.fromarray(ramp.astype("uint8"), "RGB").convert("RGBA")


def glint_layer(width, height, scale):
    """The sparkles: nested four-point stars, brightest at the core."""
    layer = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    for x, y, radius in GLINTS:
        for step in range(7, 0, -1):
            t = step / 7.0
            alpha = int(150 * (1.0 - t) ** 1.5) + 8
            points = [(px * scale, py * scale) for px, py in star(x, y, radius * t)]
            draw.polygon(points, fill=(255, 255, 255, alpha))
        core = radius * 0.22 * scale
        draw.ellipse(
            [x * scale - core, y * scale - core, x * scale + core, y * scale + core],
            fill=(255, 255, 255, 230),
        )
    return layer


def mark_image(width_px, mono=None, supersample=3, facets=None):
    """Rasterise the mark to an RGBA image ``width_px`` wide, extrusion included."""
    facets = FACETS if facets is None else facets
    side = max(8, int(round(width_px * supersample)))
    scale = side / MARK_TOTAL_W
    height = int(round(MARK_TOTAL_H * scale))
    stroke = max(1, int(round(STROKE_WIDTH * scale)))

    def poly_points(poly):
        return [(x * scale, y * scale) for x, y in poly]

    mask = word_mask(side, height, scale)

    if mono is not None:
        # Extrusion: the same silhouette, pushed down-right, knocked back.
        depth_mask = Image.new("L", (side, height), 0)
        depth_mask.paste(
            mask, (int(round(EXTRUDE[0] * scale)), int(round(EXTRUDE[1] * scale)))
        )
        depth = Image.new("RGBA", (side, height), rgb255(mono) + (255,))
        depth.putalpha(ImageChops.multiply(depth_mask, Image.new("L", (side, height), 92)))
        knockout = ImageDraw.Draw(mask)
        for facet in facets:
            points = poly_points(facet["poly"])
            knockout.line(points + [points[0]], fill=0, width=stroke, joint="curve")
        face = Image.new("RGBA", (side, height), rgb255(mono) + (255,))
        face.putalpha(mask)
        image = Image.alpha_composite(depth, face)
    else:
        # Extrusion: the same tessellation, offset and dropped into shadow. The
        # facet structure has to survive out there, or the wall reads as a blur.
        offset = (EXTRUDE[0] * scale, EXTRUDE[1] * scale)
        depth = Image.new("RGBA", (side, height), (0, 0, 0, 0))
        depth_draw = ImageDraw.Draw(depth)
        wall = depth_facets(facets)
        for poly, fill, _ in wall:
            depth_draw.polygon(
                [(x * scale + offset[0], y * scale + offset[1]) for x, y in poly],
                fill=rgb255(fill) + (255,),
            )
        for poly, _, stroke_rgb in wall:
            points = [(x * scale + offset[0], y * scale + offset[1]) for x, y in poly]
            depth_draw.line(
                points + [points[0]], fill=rgb255(stroke_rgb) + (255,),
                width=stroke, joint="curve",
            )
        depth_mask = Image.new("L", (side, height), 0)
        depth_mask.paste(mask, (int(round(offset[0])), int(round(offset[1]))))
        depth.putalpha(ImageChops.multiply(depth.split()[3], depth_mask))

        face = Image.new("RGBA", (side, height), (0, 0, 0, 0))
        draw = ImageDraw.Draw(face)
        for facet in facets:
            draw.polygon(poly_points(facet["poly"]), fill=rgb255(facet["fill"]) + (255,))
        for facet in facets:
            points = poly_points(facet["poly"])
            draw.line(
                points + [points[0]], fill=rgb255(facet["stroke"]) + (255,),
                width=stroke, joint="curve",
            )
        radius = max(1.0, NODE_RADIUS * scale)
        for x, y in NODES:
            cx, cy = x * scale, y * scale
            draw.ellipse([cx - radius, cy - radius, cx + radius, cy + radius], fill=(255, 255, 255, 76))

        # Rim light: the silhouette outline, halved by the mask and faded away
        # from the key light, so the mark reads as a lit solid.
        rim = Image.new("RGBA", (side, height), (0, 0, 0, 0))
        rim_draw = ImageDraw.Draw(rim)
        for contour in GLYPH_CONTOURS:
            points = [(x * scale, y * scale) for x, y in contour_polyline(contour)]
            if len(points) >= 2:
                rim_draw.line(
                    points + [points[0]], fill=(214, 242, 255, 255),
                    width=max(1, int(round(RIM_WIDTH * scale))), joint="curve",
                )
        alpha = ImageChops.multiply(rim.split()[3], mask)
        alpha = ImageChops.multiply(alpha, diagonal_falloff(side, height))
        rim.putalpha(alpha)
        face = Image.alpha_composite(face, rim)
        face = Image.composite(face, sweep_backdrop(side, height), face.split()[3])
        face.putalpha(mask)
        image = Image.alpha_composite(depth, face)
        image = Image.alpha_composite(image, glint_layer(side, height, scale))

    if side != width_px:
        image = image.resize(
            (width_px, max(1, int(round(width_px * MARK_TOTAL_H / MARK_TOTAL_W)))), Image.LANCZOS
        )
    return image


def text_layer(layout, element, supersample):
    """Rasterise one text element as an RGBA layer over the whole layout."""
    width = int(round(layout["width"] * supersample))
    height = int(round(layout["height"] * supersample))
    face = element["face"]
    em_px = element["em"] * supersample
    font = ImageFont.truetype(face.path, int(round(em_px)))
    scale = em_px / face.font.units_per_em
    tracking = element.get("tracking", 0.0) * supersample

    mask = Image.new("L", (width, height), 0)
    draw = ImageDraw.Draw(mask)
    pen = element["x"] * supersample
    baseline = element["baseline"] * supersample
    for character in element["text"]:
        draw.text((pen, baseline), character, font=font, fill=255, anchor="ls")
        pen += face.font.advance_units(character) * scale + tracking

    layer = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    if element.get("gradient"):
        c0, c1 = element["gradient"]
        bounds = face.ink_bounds(
            element["text"], element["em"], element["x"], element["baseline"],
            element.get("tracking", 0.0),
        )
        x0, y0, x1, y1 = [v * supersample for v in bounds]
        span = max(1.0, x1 - x0)
        ramp = Image.new("RGBA", (width, height), (0, 0, 0, 0))
        ramp_draw = ImageDraw.Draw(ramp)
        steps = 256
        for step in range(steps):
            t = step / (steps - 1.0)
            colour = tuple(int(round(c)) for c in lerp(c0, c1, t))
            left = x0 + span * step / steps
            right = x0 + span * (step + 1) / steps + 1
            ramp_draw.rectangle([left, y0 - 4, right, y1 + 4], fill=colour + (255,))
        layer = Image.composite(ramp, layer, mask)
    else:
        colour = tuple(int(round(c)) for c in element["fill"])
        layer.paste(Image.new("RGBA", (width, height), colour + (255,)), (0, 0), mask)
    return layer


def render_layout_png(layout, scale=2, background=None, radius=0.0):
    width = int(round(layout["width"] * scale))
    height = int(round(layout["height"] * scale))
    canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))

    if background is not None:
        plate = Image.new("RGBA", (width, height), tuple(int(round(c)) for c in background) + (255,))
        if radius > 0:
            mask = Image.new("L", (width, height), 0)
            ImageDraw.Draw(mask).rounded_rectangle(
                [0, 0, width - 1, height - 1], radius=radius * scale, fill=255
            )
            canvas.paste(plate, (0, 0), mask)
        else:
            canvas.paste(plate, (0, 0))

    for element in layout["elements"]:
        if element["type"] == "mark":
            width = int(round(MARK_TOTAL_W * element["scale"] * scale))
            image = mark_image(width, mono=element.get("mono"), supersample=3)
            canvas.alpha_composite(
                image, (int(round(element["x"] * scale)), int(round(element["y"] * scale)))
            )
        elif element["type"] == "text":
            canvas.alpha_composite(text_layer(layout, element, scale))
        elif element["type"] == "rect":
            draw = ImageDraw.Draw(canvas)
            draw.rounded_rectangle(
                [element["x"] * scale, element["y"] * scale,
                 (element["x"] + element["width"]) * scale, (element["y"] + element["height"]) * scale],
                radius=element.get("rx", 0.0) * scale,
                fill=tuple(int(round(c)) for c in element["fill"]) + (255,),
            )
        elif element["type"] == "streaks":
            streaks = Image.new("RGBA", (width, height), (0, 0, 0, 0))
            sdraw = ImageDraw.Draw(streaks)
            for x0, y0, x1, y1, alpha, thick in element["lines"]:
                sdraw.line(
                    [x0 * scale, y0 * scale, x1 * scale, y1 * scale],
                    fill=rgb255(element["colour"]) + (int(round(alpha * 255)),),
                    width=max(1, int(round(thick * scale))),
                )
            canvas.alpha_composite(streaks)
        elif element["type"] == "glow":
            ramp = radial_falloff(
                width, height,
                element["cx"] * scale, element["cy"] * scale,
                element["rx"] * scale, element["ry"] * scale,
            )
            tint = np.zeros((height, width, 4), dtype=float)
            tint[:, :, 0], tint[:, :, 1], tint[:, :, 2] = rgb255(element["colour"])
            tint[:, :, 3] = ramp * element["opacity"] * 255.0
            canvas.alpha_composite(Image.fromarray(tint.astype("uint8"), "RGBA"))
        elif element["type"] == "glass":
            pane = Image.new("RGBA", (width, height), (0, 0, 0, 0))
            box = [
                (element["cx"] - element["rx"]) * scale, (element["cy"] - element["ry"]) * scale,
                (element["cx"] + element["rx"]) * scale, (element["cy"] + element["ry"]) * scale,
            ]
            pdraw = ImageDraw.Draw(pane)
            pdraw.ellipse(
                box,
                fill=rgb255(GLASS) + (int(round(element["fill"] * 255)),),
                outline=rgb255(GLASS) + (int(round(element["edge"] * 255)),),
                width=max(1, int(round(1.4 * scale))),
            )
            sheen = radial_falloff(
                width, height,
                element["cx"] * scale, element["cy"] * scale,
                element["rx"] * scale, element["ry"] * scale, power=0.6,
            )
            top_left = radial_falloff(
                width, height,
                (element["cx"] - element["rx"] * 0.45) * scale,
                (element["cy"] - element["ry"] * 0.55) * scale,
                element["rx"] * scale, element["ry"] * scale,
            )
            alpha = (sheen * top_left * element["sheen"] * 255.0)
            tint = np.zeros((height, width, 4), dtype=float)
            tint[:, :, :3] = 255.0
            tint[:, :, 3] = alpha
            pane.alpha_composite(Image.fromarray(tint.astype("uint8"), "RGBA"))
            canvas.alpha_composite(pane)
    return canvas


# ----------------------------------------------------------------- app icon

ICON_CAP = 0.32          # the word's cap height, as a fraction of the tile


def app_icon_image(size_px, supersample=2):
    side = int(round(size_px * supersample))
    plate = Image.new("RGBA", (side, side), (0, 0, 0, 0))

    gradient = Image.new("RGBA", (side, side))
    gdraw = ImageDraw.Draw(gradient)
    top, bottom = (13, 20, 38), (9, 13, 26)
    for y in range(side):
        gdraw.line([(0, y), (side, y)], fill=tuple(int(round(c)) for c in lerp(top, bottom, y / side)) + (255,))

    mask = Image.new("L", (side, side), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [0, 0, side - 1, side - 1], radius=side * 0.222, fill=255
    )
    plate.paste(gradient, (0, 0), mask)

    # Atmosphere, clipped to the tile: the same streaks and glass as the banner,
    # scaled down. Without them the icon looks like a sticker.
    atmosphere = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    adraw = ImageDraw.Draw(atmosphere)
    rng = random.Random(7)
    for _ in range(16):
        y = rng.uniform(0.0, side)
        start = rng.uniform(-side * 0.10, side * 0.95)
        span = rng.uniform(side * 0.10, side * 0.45)
        adraw.line(
            [start, y, start + span, y],
            fill=STREAK + (int(rng.uniform(14, 52)),),
            width=max(1, int(round(side * 0.0018))),
        )
    halo_ramp = radial_falloff(side, side, side / 2.0, side / 2.0, side * 0.52, side * 0.46)
    tint = np.zeros((side, side, 4), dtype=float)
    tint[:, :, 0], tint[:, :, 1], tint[:, :, 2] = (56, 140, 255)
    tint[:, :, 3] = halo_ramp * 210.0
    atmosphere.alpha_composite(Image.fromarray(tint.astype("uint8"), "RGBA"))
    adraw.ellipse(
        [side / 2 - side * 0.42, side / 2 - side * 0.42,
         side / 2 + side * 0.42, side / 2 + side * 0.42],
        fill=GLASS + (20,),
        outline=GLASS + (13,),
        width=max(1, int(round(side * 0.0016))),
    )
    atmosphere.putalpha(ImageChops.multiply(atmosphere.split()[3], mask))
    plate.alpha_composite(atmosphere)

    mark = mark_image(
        int(round(size_px * ICON_CAP / MARK_CAP * MARK_TOTAL_W * supersample)), supersample=2
    )
    offset = ((side - mark.width) // 2, (side - mark.height) // 2)

    glow = mark.split()[3].filter(ImageFilter.GaussianBlur(side * 0.055))
    halo = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    halo.paste(Image.new("RGBA", mark.size, (56, 140, 255, 255)), offset, glow)
    halo.putalpha(ImageChops.multiply(halo.split()[3], Image.new("L", (side, side), 215)))
    plate.alpha_composite(halo)

    plate.alpha_composite(mark, offset)

    border = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    ImageDraw.Draw(border).rounded_rectangle(
        [0, 0, side - 1, side - 1], radius=side * 0.222, outline=(255, 255, 255, 26),
        width=max(1, int(round(side * 0.0035))),
    )
    plate.alpha_composite(border)

    if side != size_px:
        plate = plate.resize((size_px, size_px), Image.LANCZOS)
    return plate


# --------------------------------------------------------------- brand sheet

def build_sheet(path):
    HERO_W = 1800
    HERO_H = int(round(HERO_W * 560.0 / 1600.0))
    W, H = 1800, 2130
    sheet = Image.new("RGBA", (W, H), SHEET_BG + (255,))
    sheet.alpha_composite(render_layout_png(layout_hero(HERO_W, HERO_H), scale=1, background=SHEET_BG), (0, 0))

    draw = ImageDraw.Draw(sheet)
    label_font = ImageFont.truetype(FONT_REGULAR, 20)
    margin = 76

    def card(x, y, w, h, fill, radius=22):
        draw.rounded_rectangle([x, y, x + w, y + h], radius=radius, fill=fill + (255,))

    def caption(text, x, y, colour=MUTED_LIGHT):
        draw.text((x, y), text, font=label_font, fill=colour + (255,))

    # ---- plates
    plate = 340
    top = HERO_H + 116
    gap = 40

    def plate_at(column):
        return margin + column * (plate + gap)

    draw.line([(margin, top - 46), (W - margin, top - 46)], fill=(255, 255, 255, 28), width=1)
    caption("PRIMARY MARK", margin, top - 24)

    card(plate_at(0), top, plate, plate, CARD_DARK)
    mark_dark = mark_image(int(plate * 0.80), supersample=3)
    sheet.alpha_composite(mark_dark, (plate_at(0) + (plate - mark_dark.width) // 2, top + (plate - mark_dark.height) // 2))
    caption("on dark", plate_at(0), top + plate + 16)

    icon = app_icon_image(plate)
    sheet.alpha_composite(icon, (plate_at(1), top))
    caption("app icon", plate_at(1), top + plate + 16)

    card(plate_at(2), top, plate, plate, CARD_LIGHT)
    mark_light = mark_image(int(plate * 0.80), supersample=3)
    sheet.alpha_composite(mark_light, (plate_at(2) + (plate - mark_light.width) // 2, top + (plate - mark_light.height) // 2))
    caption("on light", plate_at(2), top + plate + 16)

    card(plate_at(3), top, plate, plate, CARD_DARK)
    mono = mark_image(int(plate * 0.80), mono=INK_LIGHT, supersample=3)
    sheet.alpha_composite(mono, (plate_at(3) + (plate - mono.width) // 2, top + (plate - mono.height) // 2))
    caption("monochrome", plate_at(3), top + plate + 16)

    # ---- lockups on both backgrounds
    lock_top = top + plate + 86
    caption("HORIZONTAL LOCKUP", margin, lock_top)
    band_h = 230
    band_w = W - 2 * margin
    band1 = lock_top + 34
    band2 = band1 + band_h + 24

    card(margin, band1, band_w, band_h, CARD_DARK)
    dark_lock = render_layout_png(layout_horizontal(cap=66), scale=1.9)
    sheet.alpha_composite(dark_lock, (margin + 42, band1 + (band_h - dark_lock.height) // 2))

    card(margin, band2, band_w, band_h, CARD_LIGHT)
    light_lock = render_layout_png(lighten(layout_horizontal(cap=66)), scale=1.9)
    sheet.alpha_composite(light_lock, (margin + 42, band2 + (band_h - light_lock.height) // 2))

    # ---- scalability strip
    strip_top = band2 + band_h + 62
    caption("MARK WIDTH", margin, strip_top)
    baseline = strip_top + 40 + 150
    x = margin
    for width in (300, 200, 140, 96, 64):
        image = mark_image(width, supersample=4)
        sheet.alpha_composite(image, (x, baseline - image.height))
        caption(str(width), x, baseline + 14)
        x += width + 46

    caption("APP ICON", W - margin - 560, strip_top)
    icon_x = W - margin - 560
    for size in (150, 110, 76, 48, 32):
        image = app_icon_image(size)
        sheet.alpha_composite(image, (icon_x, baseline - size))
        caption(str(size), icon_x, baseline + 14)
        icon_x += size + 56

    sheet.convert("RGB").save(path, "PNG")
    return path


# --------------------------------------------------------------------- main

def write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    return path


def padded(image, pad, colour):
    out = Image.new("RGBA", (image.width + 2 * pad, image.height + 2 * pad), tuple(colour) + (255,))
    out.alpha_composite(image, (pad, pad))
    return out


def main():
    os.makedirs(PNG_DIR, exist_ok=True)
    written = []

    def out(*parts):
        return os.path.join(BRAND, *parts)

    # ---- vector: the mark
    written.append(write(out("tacui-mark.svg"), emit_svg(layout_mark())))
    written.append(write(out("tacui-mark-mono.svg"), emit_svg(layout_mark(mono=INK_DARK))))

    # ---- vector: lockups, in a light-ink and a dark-ink flavour
    lockups = (
        ("tacui-wordmark", layout_wordmark),
        ("tacui-logo-horizontal", layout_horizontal),
        ("tacui-logo-horizontal-tagline", layout_horizontal_tagline),
        ("tacui-logo-stacked", layout_stacked),
    )
    for name, factory in lockups:
        written.append(write(out("%s.svg" % name), emit_svg(lighten(factory()))))
        written.append(write(out("%s-dark.svg" % name), emit_svg(factory())))

    written.append(
        write(
            os.path.join(BRAND, "tacui-hero.svg"),
            emit_svg(layout_hero(), background=SHEET_BG),
        )
    )

    # ---- raster: the mark and the app icon
    for name, factory in (
        ("tacui-mark-512.png", lambda: mark_image(512)),
        ("tacui-mark-1024.png", lambda: mark_image(1024)),
        ("tacui-mark-2048.png", lambda: mark_image(2048)),
        ("tacui-mark-mono-1024.png", lambda: mark_image(1024, mono=INK_DARK)),
        ("tacui-app-icon-1024.png", lambda: app_icon_image(1024)),
    ):
        target = os.path.join(PNG_DIR, name)
        factory().save(target, "PNG")
        written.append(target)

    # ---- raster: lockups. The dark-ink files stay transparent so they sit on any
    # light surface; the light-ink files carry a plate so they preview correctly.
    for name, factory, plate in (
        ("tacui-logo-horizontal.png", layout_horizontal, None),
        ("tacui-logo-horizontal-dark.png", layout_horizontal, SHEET_BG),
        ("tacui-logo-stacked.png", layout_stacked, None),
        ("tacui-logo-stacked-dark.png", layout_stacked, SHEET_BG),
    ):
        layout = lighten(factory()) if plate is None else factory()
        image = render_layout_png(layout, scale=2)
        if plate is not None:
            image = padded(image, 26, plate)
        target = os.path.join(PNG_DIR, name)
        image.save(target, "PNG")
        written.append(target)

    written.append(build_sheet(os.path.join(PNG_DIR, "tacui-brand-sheet.png")))
    hero = render_layout_png(layout_hero(), scale=1, background=SHEET_BG)
    hero.convert("RGB").save(os.path.join(PNG_DIR, "tacui-hero.png"), "PNG")
    written.append(os.path.join(PNG_DIR, "tacui-hero.png"))

    for path in written:
        print("wrote", os.path.relpath(path, os.path.dirname(BRAND)))


if __name__ == "__main__":
    main()
