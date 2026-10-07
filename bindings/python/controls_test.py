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
K_TEXT = 30
K_LIST = 40
K_LIST_ROW = 100  # + index
K_SCROLL = 50
K_SCROLLBAR = 51

BOX_BUTTON = (20.0, 70.0, 120.0, 32.0)
BOX_CHECK = (20.0, 120.0, 200.0, 24.0)
BOX_TOGGLE = (20.0, 160.0, 200.0, 24.0)
BOX_SLIDER = (20.0, 200.0, 240.0, 24.0)
BOX_DISABLED = (150.0, 70.0, 120.0, 32.0)
BOX_TEXT = (20.0, 340.0, 240.0, 32.0)
BOX_LIST = (300.0, 210.0, 260.0, 140.0)
BOX_SCROLL = (20.0, 400.0, 240.0, 110.0)

LIST_COUNT = 30
SCROLL_ROW_H = 26.0
SCROLL_ROWS = 16
SCROLL_CONTENT = SCROLL_ROW_H * SCROLL_ROWS


class App(tacui.App):
    def __init__(self) -> None:
        super().__init__()
        self.clicks = 0
        self.checked = False
        self.toggle_on = False
        self.slider = 0.0
        self.tab = 0
        self.progress = 0.25
        self.text = ""
        self.list_selected = -1
        self.list_labels = [f"Mesh_{i:02d}" for i in range(LIST_COUNT)]

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

    def on_text(self, value: str) -> None:
        self.text = value
        self._dirty()

    def on_select(self, index: int) -> None:
        self.list_selected = index
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

            ui.text_input(K_TEXT, BOX_TEXT, placeholder="type here",
                          suffix="UTF-8", on_change=self.on_text)

            ui.list_view(K_LIST, BOX_LIST, self.list_labels,
                         selected=self.list_selected, row_height=28.0,
                         row_gap=2.0, row_key_base=K_LIST_ROW,
                         on_select=self.on_select)

            # A scroll view: the library owns the offset, the host reads it to
            # place the content.
            with ui.scroll_view(K_SCROLL, BOX_SCROLL,
                                content_height=SCROLL_CONTENT):
                off = ui.scroll_offset(K_SCROLL)
                for i in range(SCROLL_ROWS):
                    y = BOX_SCROLL[1] + i * SCROLL_ROW_H - off
                    ui.label(0, (BOX_SCROLL[0] + 8.0, y + 4.0, 200.0, 18.0),
                             f"Row {i:02d}", size=12.0)
            ui.scroll_bar(K_SCROLLBAR, K_SCROLL, BOX_SCROLL,
                          content_height=SCROLL_CONTENT)


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

    # --- text input: focus, then type --------------------------------
    ui.click(K_TEXT)
    ui.update()
    ui.dispatch(tacui.EVENT_CHAR, codepoint=ord("h"))
    ui.dispatch(tacui.EVENT_CHAR, codepoint=ord("i"))
    ui.update()
    check(ui.text_get(K_TEXT) == "hi", "typing reaches the field's buffer")
    check(app.text == "hi", "the text callback reports the new value")

    ui.dispatch(tacui.EVENT_KEY_DOWN, key=tacui.KEY_BACKSPACE)
    ui.update()
    check(ui.text_get(K_TEXT) == "h", "backspace edits the field")

    ui.text_set(K_TEXT, "reset")
    ui.update()
    check(ui.text_get(K_TEXT) == "reset", "the host can set the field's text")

    # CJK goes in as codepoints and must land in the buffer as valid UTF-8 —
    # separate from whether the glyphs render (that is the atlas/fallback path).
    ui.text_set(K_TEXT, "")
    ui.update()
    ui.dispatch(tacui.EVENT_CHAR, codepoint=0x4E2D)   # 中
    ui.dispatch(tacui.EVENT_CHAR, codepoint=0x6587)   # 文
    ui.update()
    check(ui.text_get(K_TEXT) == "中文", "typing CJK stores correct UTF-8")

    # --- list view: select a row, then scroll with the wheel ---------
    ui.click(K_LIST_ROW + 2)
    ui.update()
    check(app.list_selected == 2, "clicking a list row reports its index")

    lx, ly, lw, lh = BOX_LIST
    before = ui.list_offset(K_LIST)
    ui.dispatch(tacui.EVENT_MOUSE_WHEEL, lx + lw * 0.5, ly + lh * 0.5, wheel=-3.0)
    ui.update()
    check(ui.list_offset(K_LIST) > before, "the wheel scrolls the list")

    # --- scroll view: the wheel moves its offset ---------------------
    sx2, sy2, sw2, sh2 = BOX_SCROLL
    ui.dispatch(tacui.EVENT_MOUSE_WHEEL, sx2 + sw2 * 0.5, sy2 + sh2 * 0.5,
                wheel=-3.0)
    ui.update()
    check(ui.scroll_offset(K_SCROLL) > 0.0, "the wheel scrolls the region")

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
