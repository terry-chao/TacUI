"""TacUI example — the same scene browser panel as main.cpp, driven from Python.

Kept deliberately parallel to the C++ version so the two can be read side by
side: same layout, same keys, same two change paths. The C++ code owns the
window and the GPU; everything below is the UI.

    python browser.py              interactive; click rows, Esc quits
    python browser.py --capture x.png
"""

from __future__ import annotations

import argparse
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "bindings", "python"))

import tacui
from tacui import (
    EVENT_MOUSE_DOWN,
    EVENT_MOUSE_LEAVE,
    EVENT_MOUSE_MOVE,
    rgba,
)

# --- palette (matches main.cpp) --------------------------------------------

PANEL = rgba(34, 37, 46)
ROW_PLAIN = rgba(28, 31, 39)
ROW_HOVER = rgba(38, 42, 53)
ROW_SELECTED = rgba(40, 62, 105)
DIVIDER = rgba(48, 52, 64)
TEXT_STRONG = rgba(232, 236, 245)
TEXT_DIM = rgba(150, 158, 176)
TEXT_FAINT = rgba(104, 112, 132)
ACCENT = rgba(96, 156, 246)
DOT_IDLE = rgba(70, 76, 92)
TRACK = rgba(38, 42, 53)
SCAN = rgba(110, 220, 160)

# --- identity --------------------------------------------------------------

K_ROW_BASE = 100          # K_ROW_BASE + index
K_BAKE_BUTTON = 10
K_SCAN_FILL = 11

SLOT_SELECTED = 0

# --- content ---------------------------------------------------------------

ITEMS = [
    ("Camera Rig", "1,204", "3"),
    ("Terrain", "48,912", "12"),
    ("Character_A", "9,530", "7"),
    ("Light_Main", "0", "1"),
    ("Skybox", "36", "2"),
]

# --- layout (matches main.cpp) ---------------------------------------------

LIST_X, LIST_Y, LIST_W = 20.0, 76.0, 340.0
ROW_H, ROW_GAP = 34.0, 4.0

DETAIL_X, DETAIL_Y, DETAIL_W, DETAIL_H = 384.0, 76.0, 396.0, 232.0

BUTTON_X, BUTTON_Y, BUTTON_W, BUTTON_H = DETAIL_X + 20, DETAIL_Y + 140, 96.0, 30.0

TRACK_X, TRACK_Y = DETAIL_X + 20, DETAIL_Y + 192
TRACK_W, TRACK_H = DETAIL_W - 40, 8.0
SCAN_W = 64.0
SCAN_SPAN = TRACK_W - SCAN_W


def row_top(index: int) -> float:
    return LIST_Y + index * (ROW_H + ROW_GAP)


class SceneBrowser(tacui.App):
    def __init__(self, exit_after_frames: int = 0) -> None:
        super().__init__()
        self.hover_row = -1
        self.baking = False

        # Direct-write lease, acquired on the first frame and held for the
        # life of the app. It outlives every rebuild on purpose.
        self.scan = None

        self.frames = 0
        self.exit_after_frames = exit_after_frames
        self.last_report = 0.0

    # -- build ------------------------------------------------------------

    def build(self) -> None:
        ui = self.ui
        selected = int(ui.state(SLOT_SELECTED, 0))

        with ui.stack():
            # header
            ui.text((20, 20, 400, 28), "Scene Browser", size=19, color=TEXT_STRONG)
            ui.text((20, 46, 400, 20), "examples/asset_browser/browser.py",
                    size=13, color=TEXT_FAINT)
            ui.rect((20, 68, 760, 1), color=DIVIDER)

            # list
            for i, (name, _, _) in enumerate(ITEMS):
                y = row_top(i)
                selected_row = (i == selected)
                hot = (i == self.hover_row)

                row = ROW_SELECTED if selected_row else (ROW_HOVER if hot else ROW_PLAIN)
                ui.rect((LIST_X, y, LIST_W, ROW_H), key=K_ROW_BASE + i,
                        radius=8, color=row)
                ui.rect((LIST_X + 12, y + 11, 12, 12), radius=6,
                        color=ACCENT if selected_row else DOT_IDLE)
                ui.text((LIST_X + 36, y + 7, LIST_W - 48, 24), name,
                        size=16, color=TEXT_STRONG if selected_row else TEXT_DIM)

            # detail pane
            ui.rect((DETAIL_X, DETAIL_Y, DETAIL_W, DETAIL_H), radius=12, color=PANEL)
            ui.text((DETAIL_X + 20, DETAIL_Y + 18, 200, 16), "SELECTED",
                    size=11, color=TEXT_FAINT)
            ui.text((DETAIL_X + 20, DETAIL_Y + 38, DETAIL_W - 40, 30),
                    ITEMS[selected][0], size=21, color=TEXT_STRONG)
            ui.rect((DETAIL_X + 20, DETAIL_Y + 78, DETAIL_W - 40, 1), color=DIVIDER)

            ui.text((DETAIL_X + 20, DETAIL_Y + 94, 160, 20), "Vertices",
                    size=13, color=TEXT_FAINT)
            ui.text((DETAIL_X + 200, DETAIL_Y + 94, 160, 20), ITEMS[selected][1],
                    size=13, color=TEXT_DIM)
            ui.text((DETAIL_X + 20, DETAIL_Y + 118, 160, 20), "Materials",
                    size=13, color=TEXT_FAINT)
            ui.text((DETAIL_X + 200, DETAIL_Y + 118, 160, 20), ITEMS[selected][2],
                    size=13, color=TEXT_DIM)

            # button
            ui.rect((BUTTON_X, BUTTON_Y, BUTTON_W, BUTTON_H), key=K_BAKE_BUTTON,
                    radius=8, color=ACCENT if self.baking else rgba(48, 54, 68))
            ui.text((BUTTON_X + 30, BUTTON_Y + 7, 80, 20), "Bake",
                    size=14, color=TEXT_STRONG if self.baking else TEXT_DIM)

            # progress track, and the direct-written scan
            ui.rect((TRACK_X, TRACK_Y, TRACK_W, TRACK_H), radius=TRACK_H / 2, color=TRACK)
            ui.rect((TRACK_X, TRACK_Y, SCAN_W, TRACK_H), key=K_SCAN_FILL,
                    radius=TRACK_H / 2, color=SCAN)

            ui.text((TRACK_X, TRACK_Y + 16, 200, 20),
                    "Baking..." if self.baking else "Idle",
                    size=12, color=TEXT_FAINT)

    # -- per frame --------------------------------------------------------

    def frame(self, elapsed: float) -> None:
        ui = self.ui

        if not self.scan:
            self.scan = ui.begin_override(K_SCAN_FILL)

        # The whole point: runs every frame, never dirties the tree, so the
        # rebuild count stays put while the scan keeps moving.
        if self.scan:
            phase = math.sin(elapsed * 1.3) * 0.5 + 0.5
            ui.override_translate(self.scan, phase * SCAN_SPAN, 0.0)

        self.frames += 1
        if self.exit_after_frames and self.frames >= self.exit_after_frames:
            ui.request_exit()

    # -- input ------------------------------------------------------------

    def event(self, kind: int, x: int, y: int, key: int) -> None:
        if kind == EVENT_MOUSE_MOVE:
            hit = -1
            for i in range(len(ITEMS)):
                if LIST_X <= x < LIST_X + LIST_W and row_top(i) <= y < row_top(i) + ROW_H:
                    hit = i
                    break
            if hit != self.hover_row:
                self.hover_row = hit     # a base property changed...
                self.ui.invalidate()     # ...so this rebuilds

        elif kind == EVENT_MOUSE_LEAVE:
            if self.hover_row != -1:
                self.hover_row = -1
                self.ui.invalidate()

        elif kind == EVENT_MOUSE_DOWN:
            for i in range(len(ITEMS)):
                if LIST_X <= x < LIST_X + LIST_W and row_top(i) <= y < row_top(i) + ROW_H:
                    self.ui.set_state(SLOT_SELECTED, i)
                    return
            if (BUTTON_X <= x < BUTTON_X + BUTTON_W
                    and BUTTON_Y <= y < BUTTON_Y + BUTTON_H):
                self.baking = not self.baking
                self.ui.invalidate()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture", metavar="PATH")
    ap.add_argument("--seconds", type=float, default=0.0,
                    help="stop after N seconds; 0 runs until the window closes")
    args = ap.parse_args()

    app = SceneBrowser()
    if args.seconds > 0.0:
        app.exit_after_frames = max(1, int(args.seconds * 60))
    with tacui.Ui() as ui:
        if args.capture:
            app.exit_after_frames = 60
            original = app.frame

            def frame(elapsed: float) -> None:
                original(elapsed)
                if app.frames == app.exit_after_frames - 1:
                    ui.capture_next_frame(args.capture)

            app.frame = frame  # type: ignore[method-assign]

        print("Click a row to select it, Bake to toggle. Esc quits.")
        rc = ui.run(app, title="TacUI - Scene Browser", width=800, height=340)

        # Reported here rather than from inside the frame callback: an
        # exception raised in a ctypes callback does not propagate into the C
        # loop, it is printed and swallowed, which hides real failures.
        s = ui.stats()
        print(f"frames={app.frames}  rebuilds={s.rebuilds}  paints={s.paints}  "
              f"|  shaped={s.text_shaped} glyphs={s.glyphs_rasterized} "
              f"quads/frame={s.glyph_quads}  |  liveOverrides={s.live_overrides}")
        return rc


if __name__ == "__main__":
    sys.exit(main())
