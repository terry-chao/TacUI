"""Minimal TrueType ``glyf`` outline extraction (standard library only).

Enough of the format to turn the TacUI wordmark into portable SVG paths, so the
logo does not depend on a viewer having the font installed:

* ``cmap`` formats 4 and 12 for character -> glyph mapping
* ``loca`` / ``glyf`` simple *and* composite glyphs
* ``hmtx`` advance widths, ``head`` units-per-em

Coordinates come back in font units; :func:`contours_to_path` maps them into SVG
space (y axis flipped).
"""

from __future__ import annotations

import struct

_ON_CURVE = 0x01
_X_SHORT = 0x02
_Y_SHORT = 0x04
_REPEAT = 0x08
_X_SAME = 0x10
_Y_SAME = 0x20

_ARG_1_AND_2_ARE_WORDS = 0x0001
_ARGS_ARE_XY_VALUES = 0x0002
_WE_HAVE_A_SCALE = 0x0008
_MORE_COMPONENTS = 0x0020
_WE_HAVE_AN_X_AND_Y_SCALE = 0x0040
_WE_HAVE_A_TWO_BY_TWO = 0x0080

_FONT_UNIT = 16384.0

Point = "tuple[float, float, bool]"


class TrueTypeFont:
    """A read-only view over an sfnt/TrueType file."""

    def __init__(self, path: str) -> None:
        with open(path, "rb") as handle:
            self.data = handle.read()
        raw = self.data
        if len(raw) < 12:
            raise ValueError("not a font file: %s" % path)
        version = raw[:4]
        if version == b"ttcf":
            raise ValueError("TrueType collections are unsupported: %s" % path)
        if version not in (b"\x00\x01\x00\x00", b"true", b"OTTO"):
            raise ValueError("unsupported sfnt version %r in %s" % (version, path))

        table_count = struct.unpack_from(">H", raw, 4)[0]
        self.tables: dict[str, tuple[int, int]] = {}
        for index in range(table_count):
            record = 12 + 16 * index
            name = raw[record : record + 4].decode("latin-1")
            offset, length = struct.unpack_from(">II", raw, record + 8)
            self.tables[name] = (offset, length)

        for required in ("head", "maxp", "hhea", "loca", "glyf", "cmap", "hmtx"):
            if required not in self.tables:
                raise ValueError("font is missing the %r table: %s" % (required, path))

        head = self.tables["head"][0]
        self.units_per_em = struct.unpack_from(">H", raw, head + 18)[0]
        self.loc_format = struct.unpack_from(">h", raw, head + 50)[0]
        self.x_min, self.y_min, self.x_max, self.y_max = struct.unpack_from(
            ">hhhh", raw, head + 36
        )

        self.num_glyphs = struct.unpack_from(">H", raw, self.tables["maxp"][0] + 4)[0]

        hhea = self.tables["hhea"][0]
        self.ascender, self.descender = struct.unpack_from(">hh", raw, hhea + 4)
        self.num_h_metrics = struct.unpack_from(">H", raw, hhea + 34)[0]

        self.glyph_offsets = self._read_loca()
        self.cmap = self._read_cmap()
        self.advances = self._read_hmtx()
        self._contour_cache: dict[int, list] = {}

    # ---------------------------------------------------------------- tables

    def _read_loca(self) -> list[int]:
        raw = self.data
        offset = self.tables["loca"][0]
        count = self.num_glyphs + 1
        if self.loc_format == 0:
            return [value * 2 for value in struct.unpack_from(">%dH" % count, raw, offset)]
        return list(struct.unpack_from(">%dI" % count, raw, offset))

    def _read_cmap(self) -> dict[int, int]:
        raw = self.data
        offset = self.tables["cmap"][0]
        count = struct.unpack_from(">H", raw, offset + 2)[0]
        records = []
        for index in range(count):
            platform, encoding, sub = struct.unpack_from(">HHI", raw, offset + 4 + 8 * index)
            records.append((platform, encoding, offset + sub))

        def rank(record: tuple[int, int, int]) -> int:
            platform, encoding, _ = record
            if (platform, encoding) == (3, 10):
                return 0
            if (platform, encoding) == (3, 1):
                return 1
            if platform == 0:
                return 2
            if platform == 3:
                return 3
            return 4

        for _, _, sub in sorted(records, key=rank):
            fmt = struct.unpack_from(">H", raw, sub)[0]
            if fmt == 4:
                return self._parse_cmap4(sub)
            if fmt == 12:
                return self._parse_cmap12(sub)
        raise ValueError("no usable cmap subtable")

    def _parse_cmap4(self, sub: int) -> dict[int, int]:
        raw = self.data
        seg_x2 = struct.unpack_from(">H", raw, sub + 6)[0]
        seg_count = seg_x2 // 2
        ends = struct.unpack_from(">%dH" % seg_count, raw, sub + 14)
        starts = struct.unpack_from(">%dH" % seg_count, raw, sub + 16 + seg_x2)
        deltas = struct.unpack_from(">%dh" % seg_count, raw, sub + 16 + 2 * seg_x2)
        range_base = sub + 16 + 3 * seg_x2
        ranges = struct.unpack_from(">%dH" % seg_count, raw, range_base)

        mapping: dict[int, int] = {}
        for index in range(seg_count):
            start = starts[index]
            end = ends[index]
            if start == 0xFFFF:
                continue
            for code in range(start, end + 1):
                if ranges[index] == 0:
                    glyph = (code + deltas[index]) & 0xFFFF
                else:
                    addr = range_base + 2 * index + ranges[index] + 2 * (code - start)
                    glyph = struct.unpack_from(">H", raw, addr)[0]
                    if glyph:
                        glyph = (glyph + deltas[index]) & 0xFFFF
                if glyph:
                    mapping[code] = glyph
        return mapping

    def _parse_cmap12(self, sub: int) -> dict[int, int]:
        raw = self.data
        group_count = struct.unpack_from(">I", raw, sub + 12)[0]
        mapping: dict[int, int] = {}
        for index in range(group_count):
            start, end, glyph = struct.unpack_from(">III", raw, sub + 16 + 12 * index)
            for code in range(start, end + 1):
                mapping[code] = glyph + (code - start)
        return mapping

    def _read_hmtx(self) -> list[int]:
        raw = self.data
        offset = self.tables["hmtx"][0]
        count = self.num_h_metrics
        metrics = struct.unpack_from(">%dH" % (2 * count), raw, offset)
        advances = [metrics[2 * index] for index in range(count)]
        if len(advances) < self.num_glyphs:
            advances.extend([advances[-1]] * (self.num_glyphs - len(advances)))
        return advances

    # ---------------------------------------------------------------- glyphs

    def glyph_id(self, character: str) -> int:
        return self.cmap.get(ord(character), 0)

    def advance_units(self, character: str) -> int:
        glyph = self.glyph_id(character)
        if 0 <= glyph < len(self.advances):
            return self.advances[glyph]
        return 0

    def contours(self, glyph: int) -> list[list[tuple[float, float, bool]]]:
        """Contours of ``glyph`` as ``(x, y, on_curve)`` in font units."""
        if glyph in self._contour_cache:
            return self._contour_cache[glyph]
        if glyph < 0 or glyph >= self.num_glyphs:
            return []
        start, end = self.glyph_offsets[glyph], self.glyph_offsets[glyph + 1]
        result = [] if end <= start else self._read_glyph(glyph, 0)
        self._contour_cache[glyph] = result
        return result

    def _read_glyph(self, glyph: int, depth: int) -> list[list[tuple[float, float, bool]]]:
        if depth > 6:
            return []
        start, end = self.glyph_offsets[glyph], self.glyph_offsets[glyph + 1]
        if end <= start:
            return []
        base = self.tables["glyf"][0] + start
        contour_count = struct.unpack_from(">h", self.data, base)[0]
        if contour_count < 0:
            return self._read_composite(base, depth)
        return self._read_simple(base, contour_count)

    def _read_simple(self, base: int, contour_count: int) -> list[list[tuple[float, float, bool]]]:
        raw = self.data
        if contour_count == 0:
            return []
        cursor = base + 10
        ends = struct.unpack_from(">%dH" % contour_count, raw, cursor)
        cursor += 2 * contour_count
        point_count = ends[-1] + 1
        instruction_length = struct.unpack_from(">H", raw, cursor)[0]
        cursor += 2 + instruction_length

        flags: list[int] = []
        while len(flags) < point_count:
            flag = raw[cursor]
            cursor += 1
            flags.append(flag)
            if flag & _REPEAT:
                flags.extend([flag] * raw[cursor])
                cursor += 1
        flags = flags[:point_count]

        xs: list[int] = []
        value = 0
        for flag in flags:
            if flag & _X_SHORT:
                delta = raw[cursor]
                cursor += 1
                value += delta if flag & _X_SAME else -delta
            elif not flag & _X_SAME:
                value += struct.unpack_from(">h", raw, cursor)[0]
                cursor += 2
            xs.append(value)

        ys: list[int] = []
        value = 0
        for flag in flags:
            if flag & _Y_SHORT:
                delta = raw[cursor]
                cursor += 1
                value += delta if flag & _Y_SAME else -delta
            elif not flag & _Y_SAME:
                value += struct.unpack_from(">h", raw, cursor)[0]
                cursor += 2
            ys.append(value)

        points = [(xs[i], ys[i], bool(flags[i] & _ON_CURVE)) for i in range(point_count)]
        contours = []
        previous = 0
        for end in ends:
            contours.append(points[previous : end + 1])
            previous = end + 1
        return contours

    def _read_composite(self, base: int, depth: int) -> list[list[tuple[float, float, bool]]]:
        raw = self.data
        cursor = base + 10
        result: list[list[tuple[float, float, bool]]] = []
        while True:
            flags, component = struct.unpack_from(">HH", raw, cursor)
            cursor += 4
            if flags & _ARG_1_AND_2_ARE_WORDS:
                arg1, arg2 = struct.unpack_from(">hh", raw, cursor)
                cursor += 4
            else:
                arg1, arg2 = struct.unpack_from(">bb", raw, cursor)
                cursor += 2
            dx, dy = (arg1, arg2) if flags & _ARGS_ARE_XY_VALUES else (0, 0)

            if flags & _WE_HAVE_A_SCALE:
                scale = struct.unpack_from(">h", raw, cursor)[0] / _FONT_UNIT
                cursor += 2
                xx = yy = scale
                xy = yx = 0.0
            elif flags & _WE_HAVE_AN_X_AND_Y_SCALE:
                sx, sy = struct.unpack_from(">hh", raw, cursor)
                cursor += 4
                xx, yy = sx / _FONT_UNIT, sy / _FONT_UNIT
                xy = yx = 0.0
            elif flags & _WE_HAVE_A_TWO_BY_TWO:
                xx, xy, yx, yy = (v / _FONT_UNIT for v in struct.unpack_from(">hhhh", raw, cursor))
                cursor += 8
            else:
                xx = yy = 1.0
                xy = yx = 0.0

            for contour in self.contours(component):
                result.append(
                    [
                        (x * xx + y * xy + dx, x * yx + y * yy + dy, on)
                        for x, y, on in contour
                    ]
                )
            if not flags & _MORE_COMPONENTS:
                break
        return result

    # --------------------------------------------------------------- helpers

    def cap_height_units(self) -> float:
        """Cap height measured from the ``H``/``T`` ink box (font units)."""
        for character in "HT":
            box = ink_bbox(self.contours(self.glyph_id(character)))
            if box:
                return box[3]
        return self.units_per_em * 0.70

    def x_height_units(self) -> float:
        box = ink_bbox(self.contours(self.glyph_id("x")))
        return box[3] if box else self.units_per_em * 0.50


def ink_bbox(contours) -> tuple[float, float, float, float] | None:
    """Ink bounding box ``(x_min, y_min, x_max, y_max)`` of font-unit contours."""
    xs: list[float] = []
    ys: list[float] = []
    for contour in contours:
        for x, y, _ in contour:
            xs.append(x)
            ys.append(y)
    if not xs:
        return None
    return (min(xs), min(ys), max(xs), max(ys))


def _midpoint(a, b):
    return ((a[0] + b[0]) / 2.0, (a[1] + b[1]) / 2.0)


def contour_to_quadratics(contour) -> list[tuple[str, tuple]]:
    """Convert one TrueType contour into ``("M"|"L"|"Q", points)`` segments."""
    points = list(contour)
    count = len(points)
    if count == 0:
        return []

    if points[0][2]:
        start = points[0]
        first = 1
    elif points[-1][2]:
        start = points[-1]
        first = 0
    else:
        start = _midpoint(points[-1], points[0])
        first = 0

    segments: list[tuple[str, tuple]] = [("M", ((start[0], start[1]),))]
    control = None
    for offset in range(count):
        x, y, on_curve = points[(first + offset) % count]
        if on_curve:
            if control is None:
                segments.append(("L", ((x, y),)))
            else:
                segments.append(("Q", (control, (x, y))))
                control = None
        else:
            if control is None:
                control = (x, y)
            else:
                implied = _midpoint(control, (x, y))
                segments.append(("Q", (control, implied)))
                control = (x, y)
    if control is not None:
        segments.append(("Q", (control, (start[0], start[1]))))
    segments.append(("Z", ()))
    return segments


def contours_to_path(contours, mapper, precision: int = 2) -> str:
    """Render ``contours`` as SVG path data, mapping each point through ``mapper``."""
    fmt = "%%.%df" % precision
    parts: list[str] = []
    for contour in contours:
        for command, points in contour_to_quadratics(contour):
            if command == "Z":
                parts.append("Z")
                continue
            coords = []
            for x, y in points:
                mx, my = mapper(x, y)
                coords.append("%s %s" % (fmt % mx, fmt % my))
            parts.append(command + " ".join(coords))
    return "".join(parts)
