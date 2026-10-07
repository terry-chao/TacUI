"""M0 slice F — the same UI as apps/m0, but the UI logic lives in Python.

This is the evidence for M0 acceptance criterion 5: a non-C++ host can drive
the framework across the C ABI, including the direct-write channel.

  python demo.py                # interactive; click the blue button
  python demo.py --selftest     # scripted, asserts, exits non-zero on failure
  python demo.py --capture out.png
"""

from __future__ import annotations

import argparse
import math
import sys

import tacui
from tacui import (
    EVENT_KEY_DOWN,
    EVENT_MOUSE_DOWN,
    EVENT_MOUSE_LEAVE,
    EVENT_MOUSE_MOVE,
    VK_ESCAPE,
    rgba,
)

# Node keys — the stable identities the direct-write path addresses.
K_BUTTON = 1
K_COUNT_BAR = 2
K_COEXIST = 3
K_TARGET = 4
K_ANIMATED = 5
K_TITLE = 6
K_COUNT_TEXT = 7
K_HINT_TEXT = 8

SLOT_COUNT = 0

SELFTEST_CLICK_EVERY = 30   # frames between scripted "clicks"


class Demo(tacui.App):
    def __init__(self, exit_after_frames: int = 0) -> None:
        super().__init__()
        self.hover = False
        self.hx = -1
        self.hy = -1

        # Direct-write leases, acquired on the first frame and held for the
        # life of the app. They deliberately outlive every rebuild.
        self.anim = None
        self.coexist = None

        self.frames = 0
        self.exit_after_frames = exit_after_frames
        self.rebuilds_at_start = None
        self.rebuilds_with_live_override = 0

    # -- build ------------------------------------------------------------

    def build(self) -> None:
        ui = self.ui
        count = ui.state(SLOT_COUNT, 0)

        with ui.stack():
            ui.text(
                (60, 16, 900, 28),
                "TacUI M0 - driven from CPython via the C ABI",
                key=K_TITLE,
                size=19,
                color=rgba(140, 148, 168),
            )

            # The counter button. Its colour is a *base* property, so hovering
            # it forces a rebuild — the deliberate contrast with the animation.
            ui.rect(
                (60, 60, 190, 58),
                key=K_BUTTON,
                radius=14,
                color=rgba(96, 156, 246) if self.hover else rgba(58, 104, 186),
            )

            ui.text(
                (280, 74, 300, 40),
                f"count = {count}",
                key=K_COUNT_TEXT,
                size=30,
                color=rgba(235, 240, 250),
            )

            # The same count, as a bar: the two must stay in step.
            width = 14 + (count % 24) * 22
            ui.rect(
                (60, 140, width, 22),
                key=K_COUNT_BAR,
                radius=6,
                color=rgba(110, 220, 160),
            )

            ui.text(
                (60, 320, 900, 30),
                "The quick brown fox jumps over the lazy dog. 0123456789 !@#$%^&*()",
                key=K_HINT_TEXT,
                size=17,
                color=rgba(120, 128, 148),
            )

            # Criterion 4: base changes on every rebuild, override must survive.
            tint = 60 + (count * 53) % 180
            ui.rect(
                (480, 60, 220, 220),
                key=K_COEXIST,
                radius=22,
                color=rgba(tint, 96, 200),
            )

            # Criterion 3: moved purely by the override channel.
            ui.rect(
                (800, 270, 90, 6),
                key=K_TARGET,
                radius=3,
                color=rgba(90, 96, 112),
            )
            ui.rect(
                (780, 200, 130, 130),
                key=K_ANIMATED,
                radius=30,
                color=rgba(240, 152, 64),
            )

    # -- per frame --------------------------------------------------------

    def frame(self, elapsed: float) -> None:
        ui = self.ui

        # Hover is a base property, so it goes through a rebuild.
        hot = self.hx >= 0 and ui.contains(K_BUTTON, self.hx, self.hy)
        if hot != self.hover:
            self.hover = hot
            ui.invalidate()

        # Acquire the leases lazily. A node may legitimately not exist on
        # frame one (conditional content), so this retries rather than
        # assuming. Exceptions raised in a ctypes callback do not propagate
        # into the C loop, so failing hard here would be silent.
        if not (self.anim and self.coexist):
            if not self.anim:
                self.anim = ui.begin_override(K_ANIMATED)
            if not self.coexist:
                self.coexist = ui.begin_override(K_COEXIST)
                if self.coexist:
                    ui.override_opacity(self.coexist, 0.45)
            if not (self.anim and self.coexist) and self.frames > 3:
                raise RuntimeError("could not acquire the override leases")

        # Criterion 3: runs every frame, must never dirty the tree.
        if self.anim:
            ui.override_translate(self.anim, 0.0, math.sin(elapsed * 2.2) * 78.0)

        # Criterion 4 check: the lease and the override must still be there.
        if self.coexist and not ui.has_override(K_COEXIST):
            raise AssertionError("coexist override was clobbered by a rebuild")

        ui_ = ui.stats()
        if self.rebuilds_at_start is None:
            self.rebuilds_at_start = ui_.rebuilds
        self.rebuilds_with_live_override = ui_.reconciles_with_live_override

        self.frames += 1

        if self.exit_after_frames and self.frames % SELFTEST_CLICK_EVERY == 0:
            ui.set_state(SLOT_COUNT, ui.state(SLOT_COUNT, 0) + 1)

        if self.exit_after_frames and self.frames >= self.exit_after_frames:
            ui.request_exit()

    # -- input ------------------------------------------------------------

    def event(self, ev) -> None:
        if ev.kind == EVENT_MOUSE_MOVE:
            self.hx, self.hy = ev.x, ev.y
        elif ev.kind == EVENT_MOUSE_LEAVE:
            self.hx, self.hy = -1, -1
        elif ev.kind == EVENT_MOUSE_DOWN:
            if self.ui.contains(K_BUTTON, ev.x, ev.y):
                self.ui.set_state(SLOT_COUNT, self.ui.state(SLOT_COUNT, 0) + 1)
        elif ev.kind == EVENT_KEY_DOWN and ev.key == VK_ESCAPE:
            self.ui.request_exit()


def run_selftest(seconds: float) -> int:
    print(f"TacUI ABI v{tacui.TUI_ABI_VERSION} - Python host self-test")
    print(f"selftest: pressing a scripted click every {SELFTEST_CLICK_EVERY} frames\n")

    app = Demo()
    with tacui.Ui() as ui:
        # Roughly 60Hz; exit a little after the requested duration.
        app.exit_after_frames = max(SELFTEST_CLICK_EVERY * 3, int(seconds * 60))
        rc = ui.run(app, title="TacUI — Python self-test", width=900, height=420)
        if rc != 0:
            print(f"FAIL: tui_run returned {rc}", file=sys.stderr)
            return 1

        s = ui.stats()
        print(f"\nframes={app.frames}  rebuilds={s.rebuilds}  paints={s.paints}")
        print(f"created={s.elements_created} destroyed={s.elements_destroyed}")
        print(f"reconciles_with_live_override={s.reconciles_with_live_override}")

        print("\nretained tree:")
        for line in ui.dump():
            print("  " + line)

        print("\n--- M0 slice F self-test (host = CPython) ---")
        ok = True

        def check(cond: bool, what: str) -> None:
            nonlocal ok
            print(f"  [{'PASS' if cond else 'FAIL'}] {what}")
            if not cond:
                ok = False

        check(s.paints > app.frames // 2, "criterion 3: painted nearly every frame")
        check(s.rebuilds >= 3, "criterion 2: host-driven setState caused rebuilds")
        check(s.rebuilds * 8 < s.paints,
              "criterion 3: animation frames did not rebuild (rebuilds << paints)")
        check(s.reconciles_with_live_override >= 2,
              "criterion 4: override survived repeated rebuilds")
        check(ui.has_override(K_COEXIST),
              "criterion 4: coexist override still installed at exit")
        check(s.elements_created > 0 and s.elements_destroyed == 0,
              "retention: elements were reused across rebuilds, none destroyed")
        check(len(ui.dump()) >= 5, "criterion 6: tree dump produced nodes")

        print(f"--- {'ALL PASS' if ok else 'FAILURES PRESENT'} ---")
        return 0 if ok else 2


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--seconds", type=float, default=6.0)
    ap.add_argument("--capture", metavar="PATH")
    args = ap.parse_args()

    if args.selftest:
        return run_selftest(args.seconds)

    app = Demo()
    with tacui.Ui() as ui:
        if args.capture:
            app.exit_after_frames = 90
            # Fire the capture on the last frame before exiting.
            original = app.frame

            def frame(elapsed: float) -> None:
                original(elapsed)
                if app.frames == app.exit_after_frames - 1:
                    ui.capture_next_frame(args.capture)

            app.frame = frame  # type: ignore[method-assign]

        print("Click the blue button. Esc quits.")
        return ui.run(app, title="TacUI — Python", width=1280, height=720)


if __name__ == "__main__":
    sys.exit(main())
