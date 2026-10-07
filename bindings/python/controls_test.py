"""Headless self-test: the control library driven from Python over the C ABI.

The point of TacUI is that the *same* control library runs in every host
language. This proves the Python side actually reaches it: it builds a panel of
controls, injects synthetic input through the ABI, and asserts the controls
fired — no window, no GPU, no screenshot.

    python bindings/python/controls_test.py

Set TACUI_DLL if tacui.dll is somewhere the binding does not guess.
"""

from __future__ import annotations

import sys

import tacui
from tacui import App, Ui


# --- identities -------------------------------------------------------------

K_BUTTON = 1
K_CHECK = 2
K_TOGGLE = 3
K_SLIDER = 4
K_PROGRESS = 5
K_DIVIDER = 6
K_DISABLED = 7
K_TABS = 10  # + index
K_PANEL = 20
K_PANEL_LABEL = 21

BOX_BUTTON = (20.0, 70.0, 120.0, 32.0)
BOX_CHECK = (20.0, 120.0, 200.0, 24.0)
BOX_TOGGLE = (20.0, 160.0, 200.0, 24.0)
BOX_SLIDER = (20.0, 200.0, 240.0, 24.0)
BOX_DISABLED = (150.0, 70.0, 120.0, 32.0)


class App(tacui.App):
    def __init__(self) -> None:
        super().__init__()
        self.clicks = 0
        self.checked = False
        self.toggle_on = False
        self.slider = 0.0
        self.tab = 0
        self.progress = 0.25

    # -- handlers ---------------------------------------------------------

    def _dirty(self) -> None:
        assert self.ui is not None
        self.ui.invalidate()

    def on_click(self) -> None:
        self.clicks += 1
        self._dirty()

    def on_check(self, value: bool) -> None:
        self.checked = value
        self._dirty()

    def on_toggle(self, value: bool) -> None:
        self.toggle_on = value
        self._dirty()

    def on_slider(self, value: float) -> None:
        self.slider = value
        self._dirty()

    def on_tab(self, index: int) -> None:
        self.tab = index
        self._dirty()

    # -- build ------------------------------------------------------------

    def build(self) -> None:
        ui = self.ui
        assert ui is not None

        with ui.stack():
            ui.label(0, (20.0, 20.0, 400.0, 28.0),
                     "Controls from Python", size=22.0, strong=True)

            ui.button(K_BUTTON, BOX_BUTTON, "Click",
                      style=tacui.BUTTON_PRIMARY, on_click=self.on_click)
            ui.button(K_DISABLED, BOX_DISABLED, "Disabled",
                      enabled=False, on_click=self.on_click)
            ui.checkbox(K_CHECK, BOX_CHECK, "Snap to grid",
                        checked=self.checked, on_change=self.on_check)
            ui.toggle(K_TOGGLE, BOX_TOGGLE, "Wireframe",
                      on=self.toggle_on, on_change=self.on_toggle)
            ui.slider(K_SLIDER, BOX_SLIDER, value=self.slider,
                      on_change=self.on_slider)
            ui.progress(K_PROGRESS, (20.0, 240.0, 240.0, 8.0),
                        value=self.progress)
            ui.divider(K_DIVIDER, (20.0, 270.0, 240.0, 1.0))
            ui.tabs(K_TABS, (20.0, 290.0, 240.0, 32.0), ["A", "B", "C"],
                    selected=self.tab, on_select=self.on_tab)

            # A panel is a container: `with` nests its children.
            with ui.panel(K_PANEL, (300.0, 70.0, 200.0, 120.0)):
                ui.label(K_PANEL_LABEL, (316.0, 86.0, 160.0, 20.0),
                         "inside a panel", size=13.0)


def main() -> int:
    ok = True

    def check(cond: bool, what: str) -> None:
        nonlocal ok
        print(f"  [{'PASS' if cond else 'FAIL'}] {what}")
        if not cond:
            ok = False

    app = App()
    ui = Ui()
    ui._install(app)
    assert ui.update(), "the first update should build the tree"

    print("\n--- Python control layer self-test ---")

    # --- the tree actually contains the controls -------------------------
    dump = "\n".join(ui.dump())
    check("Controls from Python" in dump, "a label reached the retained tree")
    check("inside a panel" in dump, "a panel nested its children")

    # --- bounds are where the build asked for them -----------------------
    bx, by, bw, bh = ui.bounds(K_BUTTON)
    check((bx, by, bw, bh) == BOX_BUTTON, "the button sits at its box")

    # --- click reaches Python --------------------------------------------
    ui.click(K_BUTTON)
    ui.update()
    check(app.clicks == 1, "clicking the button fires the Python callback")

    ui.click(K_BUTTON)
    ui.update()
    check(app.clicks == 2, "a second click fires again")

    # --- checkbox / toggle flip their host state -------------------------
    ui.click(K_CHECK)
    ui.update()
    check(app.checked is True, "clicking the checkbox flips the host value")

    ui.click(K_TOGGLE)
    ui.update()
    check(app.toggle_on is True, "clicking the toggle flips the host value")

    # --- slider: click to jump, then drag to the end ---------------------
    ui.click(K_SLIDER)
    ui.update()
    check(0.45 < app.slider < 0.55, "clicking the slider track jumps to ~0.5")

    sx, sy, sw, sh = ui.bounds(K_SLIDER)
    cy = sy + sh * 0.5
    ui.dispatch(tacui.EVENT_MOUSE_DOWN, sx + 8.0, cy)
    ui.dispatch(tacui.EVENT_MOUSE_MOVE, sx + sw - 8.0, cy)
    ui.dispatch(tacui.EVENT_MOUSE_UP, sx + sw - 8.0, cy)
    ui.update()
    check(app.slider > 0.9, "dragging the slider to the right reaches the end")

    # --- tabs select by segment key --------------------------------------
    ui.click(K_TABS + 1)
    ui.update()
    check(app.tab == 1, "clicking a tab segment reports its index")

    # --- a disabled control stays inert ----------------------------------
    app.clicks = 0
    ui.click(K_DISABLED)
    ui.update()
    check(app.clicks == 0, "a disabled button does not fire")

    ui.close()

    print(f"--- {'ALL PASS' if ok else 'FAILURES PRESENT'} ---")
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
