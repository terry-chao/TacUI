"""ctypes binding for the TacUI C ABI (capi/tacui.h).

Hand-written on purpose. Per plan.md §10 the FFI plumbing is the mechanical
part; the idiomatic layer on top is the part every host language has to write
itself, and this file is the Python shape of it.

The split this demonstrates:
  * C++ owns the window, the GPU device and the frame loop  (tui_run)
  * Python owns the UI: the build callback, event handling and animations
  * the control library is shared: ui.button(...) reaches the same widget the
    C++ side uses, and tui_dispatch_event / tui_update let a windowless test
    drive it (see controls_test.py)

Requires tacui.dll — either built from the capi/ target, or installed by
vcpkg. Set TACUI_DLL to point at it when it lives somewhere neither of those
guesses.
"""

from __future__ import annotations

import ctypes
import os
import sys
from ctypes import (
    CFUNCTYPE,
    POINTER,
    Structure,
    c_char_p,
    c_double,
    c_float,
    c_int32,
    c_int64,
    c_uint32,
    c_uint64,
    c_void_p,
)

# --- ABI constants (must match capi/tacui.h) --------------------------------

TUI_ABI_VERSION = 1

NODE_STACK = 0
NODE_RECT = 1
NODE_TEXT = 2

EVENT_RESIZE = 0
EVENT_CLOSE = 1
EVENT_MOUSE_MOVE = 2
EVENT_MOUSE_LEAVE = 3
EVENT_MOUSE_DOWN = 4
EVENT_MOUSE_UP = 5
EVENT_KEY_DOWN = 6
EVENT_MOUSE_WHEEL = 7
EVENT_CHAR = 8

BUTTON_PRIMARY = 0
BUTTON_SECONDARY = 1
BUTTON_GHOST = 2
BUTTON_DANGER = 3

# Key codes for EVENT_KEY_DOWN; printable input goes through EVENT_CHAR.
KEY_UNKNOWN = 0
KEY_TAB = 2
KEY_ENTER = 3
KEY_ESCAPE = 4
KEY_BACKSPACE = 5
KEY_DELETE = 6
KEY_LEFT = 7
KEY_RIGHT = 8
KEY_UP = 9
KEY_DOWN = 10
KEY_HOME = 11
KEY_END = 12
KEY_PAGE_UP = 13
KEY_PAGE_DOWN = 14
KEY_SPACE = 15

VK_ESCAPE = 0x1B


# --- ABI structs ------------------------------------------------------------


class Handle(Structure):
    """Generational handle. A stale one resolves to null rather than to a
    different node — the whole point of the index+generation pair."""

    _fields_ = [("index", c_uint32), ("generation", c_uint32)]

    def __bool__(self) -> bool:
        return self.index != 0 and self.generation != 0

    def __repr__(self) -> str:
        return f"Handle(index={self.index}, generation={self.generation})"


class Stats(Structure):
    """Must mirror tui_stats in capi/tacui.h field for field.

    `_check_stats_layout` below fails loudly at import if it drifts — ctypes
    will not notice on its own, and the counters would then be written past
    the end of this structure.
    """

    _fields_ = [
        ("rebuilds", c_uint64),
        ("elements_created", c_uint64),
        ("elements_destroyed", c_uint64),
        ("elements_updated", c_uint64),
        ("paints", c_uint64),
        ("reconciles_with_live_override", c_uint64),
        ("live_overrides", c_uint32),
        ("text_shaped", c_uint64),
        ("glyphs_rasterized", c_uint64),
        ("glyph_cache_hits", c_uint64),
        ("glyph_quads", c_uint64),
        ("atlas_uploads", c_uint64),
    ]


BUILD_FN = CFUNCTYPE(None, c_void_p)
FRAME_FN = CFUNCTYPE(None, c_void_p, c_double)
EVENT_FN = CFUNCTYPE(None, c_void_p, c_int32, c_int32, c_int32, c_uint32)
DUMP_FN = CFUNCTYPE(None, c_void_p, c_int32, c_char_p)

# Control callbacks. The C ABI hands back the `user` pointer it was given, which
# the binding always passes as null — the closure in Python is what carries the
# state, so the pointer is unused.
CLICK_FN = CFUNCTYPE(None, c_void_p)
BOOL_FN = CFUNCTYPE(None, c_void_p, c_int32)
FLOAT_FN = CFUNCTYPE(None, c_void_p, c_float)
INDEX_FN = CFUNCTYPE(None, c_void_p, c_int32)


class Host(Structure):
    _fields_ = [
        ("build", BUILD_FN),
        ("frame", FRAME_FN),
        ("event", EVENT_FN),
        ("user", c_void_p),
    ]


# --- helpers ----------------------------------------------------------------


def rgba(r: int, g: int, b: int, a: int = 255) -> int:
    """Pack a colour the way the ABI expects: R in the low byte."""
    return (a << 24) | (b << 16) | (g << 8) | r


def _load_library() -> ctypes.CDLL:
    """Find tacui.dll.

    Order: TACUI_DLL, then next to this file (a wheel, or a manual copy), then
    the in-tree build. A vcpkg install puts the DLL under
    <vcpkg>/installed/<triplet>/bin, which is deliberately not guessed at —
    point TACUI_DLL there instead.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        os.environ.get("TACUI_DLL"),
        os.path.join(here, "tacui.dll"),
        os.path.join(here, "..", "..", "build", "bin", "tacui.dll"),
        os.path.join(here, "..", "..", "build", "bin", "Debug", "tacui.dll"),
    ]
    for path in candidates:
        if path and os.path.isfile(path):
            return ctypes.CDLL(os.path.normpath(path))

    raise OSError(
        "tacui.dll not found. Build the capi target:\n"
        "  cmake --build build --config Debug --target tacui\n"
        "or point TACUI_DLL at an installed one — vcpkg puts it in\n"
        "  <vcpkg>/installed/<triplet>/bin"
    )


_lib = _load_library()

_lib.tui_abi_version.restype = c_uint32
_lib.tui_ui_create.restype = c_void_p
_lib.tui_ui_create.argtypes = []
_lib.tui_ui_destroy.argtypes = [c_void_p]
_lib.tui_set_host.argtypes = [c_void_p, POINTER(Host)]

_lib.tui_push.argtypes = [c_void_p, c_uint32, c_uint64]
_lib.tui_pop.argtypes = [c_void_p]
_lib.tui_rect.argtypes = [
    c_void_p, c_float, c_float, c_float, c_float, c_float, c_uint32
]
_lib.tui_text.argtypes = [
    c_void_p, c_float, c_float, c_float, c_float, c_float, c_uint32, c_char_p
]

_lib.tui_label.argtypes = [
    c_void_p, c_uint64, c_float, c_float, c_float, c_float,
    c_char_p, c_float, c_uint32, c_int32,
]
_lib.tui_button.argtypes = [
    c_void_p, c_uint64, c_float, c_float, c_float, c_float,
    c_char_p, c_uint32, c_int32, CLICK_FN, c_void_p,
]
_lib.tui_checkbox.argtypes = [
    c_void_p, c_uint64, c_float, c_float, c_float, c_float,
    c_char_p, c_int32, c_int32, BOOL_FN, c_void_p,
]
_lib.tui_radio.argtypes = [
    c_void_p, c_uint64, c_float, c_float, c_float, c_float,
    c_char_p, c_int32, c_int32, CLICK_FN, c_void_p,
]
_lib.tui_toggle.argtypes = [
    c_void_p, c_uint64, c_float, c_float, c_float, c_float,
    c_char_p, c_int32, c_int32, BOOL_FN, c_void_p,
]
_lib.tui_slider.argtypes = [
    c_void_p, c_uint64, c_float, c_float, c_float, c_float,
    c_float, c_float, c_float, c_float, c_int32, FLOAT_FN, c_void_p,
]
_lib.tui_progress.argtypes = [
    c_void_p, c_uint64, c_float, c_float, c_float, c_float, c_float, c_uint32,
]
_lib.tui_divider.argtypes = [
    c_void_p, c_uint64, c_float, c_float, c_float, c_float, c_uint32,
]
_lib.tui_tabs.argtypes = [
    c_void_p, c_uint64, c_float, c_float, c_float, c_float,
    POINTER(c_char_p), c_int32, c_int32, INDEX_FN, c_void_p,
]
_lib.tui_panel.argtypes = [
    c_void_p, c_uint64, c_float, c_float, c_float, c_float,
    c_uint32, c_float, c_int32,
]

_lib.tui_dispatch_event.argtypes = [
    c_void_p, c_int32, c_float, c_float, c_uint32, c_uint32, c_float, c_int32,
]
_lib.tui_update.restype = c_int32
_lib.tui_update.argtypes = [c_void_p]

_lib.tui_state_get.restype = c_int64
_lib.tui_state_get.argtypes = [c_void_p, c_uint32, c_int64]
_lib.tui_state_set.argtypes = [c_void_p, c_uint32, c_int64]
_lib.tui_invalidate.argtypes = [c_void_p]

_lib.tui_handle_null.restype = Handle
_lib.tui_handle_valid.restype = c_int32
_lib.tui_handle_valid.argtypes = [Handle]

_lib.tui_node_find.restype = Handle
_lib.tui_node_find.argtypes = [c_void_p, c_uint64]
_lib.tui_override_begin.restype = Handle
_lib.tui_override_begin.argtypes = [c_void_p, c_uint64]
_lib.tui_override_translate.argtypes = [c_void_p, Handle, c_float, c_float]
_lib.tui_override_opacity.argtypes = [c_void_p, Handle, c_float]
_lib.tui_override_release.argtypes = [c_void_p, Handle]

_lib.tui_node_contains.restype = c_int32
_lib.tui_node_contains.argtypes = [c_void_p, c_uint64, c_float, c_float]
_lib.tui_node_bounds.argtypes = [c_void_p, c_uint64, POINTER(c_float)]
_lib.tui_node_has_override.restype = c_int32
_lib.tui_node_has_override.argtypes = [c_void_p, c_uint64]

_lib.tui_stats_size.restype = c_uint32
_lib.tui_get_stats.argtypes = [c_void_p, POINTER(Stats)]


def _check_stats_layout() -> None:
    """Fail at import rather than corrupt memory later.

    A hand-written mirror of a C struct is exactly the thing that drifts, and
    ctypes gives no warning when it does: tui_get_stats would fill more bytes
    than this structure holds.
    """
    expected = _lib.tui_stats_size()
    actual = ctypes.sizeof(Stats)
    if expected != actual:
        raise RuntimeError(
            f"tacui.dll and this binding disagree on tui_stats: "
            f"library says {expected} bytes, binding has {actual}. "
            f"Update Stats in bindings/python/tacui.py."
        )


_check_stats_layout()

_lib.tui_dump_tree.argtypes = [c_void_p, DUMP_FN, c_void_p]

_lib.tui_run.restype = c_int32
_lib.tui_run.argtypes = [c_void_p, c_char_p, c_uint32, c_uint32]
_lib.tui_request_exit.argtypes = [c_void_p]
_lib.tui_capture_next_frame.argtypes = [c_void_p, c_char_p]

# The C side hands the host context back through `user`, so the registry is
# what keeps each App alive for the duration of its run.
_APPS: dict[int, "App"] = {}


class App:
    """Subclass and override the hooks. `ui` is injected during run()."""

    def __init__(self) -> None:
        self.ui: "Ui | None" = None
        self.user = 0

    def build(self) -> None:
        """Emit exactly one root node. Called only when the tree is dirty."""

    def frame(self, elapsed: float) -> None:
        """Called every frame. Animations belong here, via overrides."""

    def event(self, kind: int, x: int, y: int, key: int) -> None:
        """Input. Check `kind` against the EVENT_* constants."""


class Ui:
    """The build plane plus the direct-write channel."""

    def __init__(self) -> None:
        self._ui = _lib.tui_ui_create()
        if not self._ui:
            raise MemoryError("tui_ui_create failed")
        self._app: App | None = None
        self._build_cb = None
        self._frame_cb = None
        self._event_cb = None
        # ctypes callback objects are only valid while a reference exists, and
        # the C side keeps calling them between builds — so the binding holds
        # every control callback until the next build replaces it.
        self._callbacks: list = []

    # -- build plane ------------------------------------------------------

    def stack(self, key: int = 0) -> "_StackCtx":
        return _StackCtx(self, key)

    def rect(
        self,
        box: tuple[float, float, float, float],
        *,
        key: int = 0,
        radius: float = 0.0,
        color: int = 0xFFFFFFFF,
    ) -> None:
        """A leaf node: pushed, given its properties and popped again."""
        x, y, w, h = box
        _lib.tui_push(self._ui, NODE_RECT, key)
        _lib.tui_rect(self._ui, x, y, w, h, radius, color)
        _lib.tui_pop(self._ui)

    def text(
        self,
        box: tuple[float, float, float, float],
        content: str,
        *,
        key: int = 0,
        size: float = 16.0,
        color: int = 0xFFFFFFFF,
    ) -> None:
        """A single line of text. Its baseline sits at y + ascent(size)."""
        x, y, w, h = box
        _lib.tui_push(self._ui, NODE_TEXT, key)
        _lib.tui_text(self._ui, x, y, w, h, size, color, content.encode("utf-8"))
        _lib.tui_pop(self._ui)

    # -- controls (L2) ----------------------------------------------------
    #
    # The same control library the C++ side uses, over the ABI (docs/controls.md).
    # A control is a function; the framework owns hover / press / focus, so the
    # binding only says what it looks like and what to do when it fires.

    def _click(self, fn) -> "CLICK_FN":
        if fn is None:
            return CLICK_FN()
        cb = CLICK_FN(lambda _user: fn())
        self._callbacks.append(cb)
        return cb

    def _bool(self, fn) -> "BOOL_FN":
        if fn is None:
            return BOOL_FN()
        cb = BOOL_FN(lambda _user, v: fn(bool(v)))
        self._callbacks.append(cb)
        return cb

    def _float(self, fn) -> "FLOAT_FN":
        if fn is None:
            return FLOAT_FN()
        cb = FLOAT_FN(lambda _user, v: fn(v))
        self._callbacks.append(cb)
        return cb

    def _index(self, fn) -> "INDEX_FN":
        if fn is None:
            return INDEX_FN()
        cb = INDEX_FN(lambda _user, i: fn(i))
        self._callbacks.append(cb)
        return cb

    def label(
        self,
        key: int,
        box: tuple[float, float, float, float],
        content: str,
        *,
        size: float = 0.0,
        color: int = 0,
        strong: bool = False,
    ) -> None:
        x, y, w, h = box
        _lib.tui_label(
            self._ui, key, x, y, w, h,
            content.encode("utf-8"), size, color, 1 if strong else 0,
        )

    def button(
        self,
        key: int,
        box: tuple[float, float, float, float],
        content: str,
        *,
        style: int = BUTTON_SECONDARY,
        enabled: bool = True,
        on_click=None,
    ) -> None:
        x, y, w, h = box
        _lib.tui_button(
            self._ui, key, x, y, w, h, content.encode("utf-8"),
            style, 1 if enabled else 0, self._click(on_click), None,
        )

    def checkbox(
        self,
        key: int,
        box: tuple[float, float, float, float],
        content: str,
        *,
        checked: bool = False,
        enabled: bool = True,
        on_change=None,
    ) -> None:
        x, y, w, h = box
        _lib.tui_checkbox(
            self._ui, key, x, y, w, h, content.encode("utf-8"),
            1 if checked else 0, 1 if enabled else 0, self._bool(on_change), None,
        )

    def radio(
        self,
        key: int,
        box: tuple[float, float, float, float],
        content: str,
        *,
        selected: bool = False,
        enabled: bool = True,
        on_select=None,
    ) -> None:
        x, y, w, h = box
        _lib.tui_radio(
            self._ui, key, x, y, w, h, content.encode("utf-8"),
            1 if selected else 0, 1 if enabled else 0, self._click(on_select), None,
        )

    def toggle(
        self,
        key: int,
        box: tuple[float, float, float, float],
        content: str = "",
        *,
        on: bool = False,
        enabled: bool = True,
        on_change=None,
    ) -> None:
        x, y, w, h = box
        _lib.tui_toggle(
            self._ui, key, x, y, w, h, content.encode("utf-8"),
            1 if on else 0, 1 if enabled else 0, self._bool(on_change), None,
        )

    def slider(
        self,
        key: int,
        box: tuple[float, float, float, float],
        *,
        value: float = 0.0,
        min: float = 0.0,
        max: float = 1.0,
        step: float = 0.0,
        enabled: bool = True,
        on_change=None,
    ) -> None:
        x, y, w, h = box
        _lib.tui_slider(
            self._ui, key, x, y, w, h, value, min, max, step,
            1 if enabled else 0, self._float(on_change), None,
        )

    def progress(
        self,
        key: int,
        box: tuple[float, float, float, float],
        *,
        value: float,
        color: int = 0,
    ) -> None:
        x, y, w, h = box
        _lib.tui_progress(self._ui, key, x, y, w, h, value, color)

    def divider(
        self,
        key: int,
        box: tuple[float, float, float, float],
        *,
        color: int = 0,
    ) -> None:
        x, y, w, h = box
        _lib.tui_divider(self._ui, key, x, y, w, h, color)

    def tabs(
        self,
        first_key: int,
        box: tuple[float, float, float, float],
        labels: list[str],
        *,
        selected: int = 0,
        on_select=None,
    ) -> None:
        x, y, w, h = box
        encoded = [s.encode("utf-8") for s in labels]
        arr = (c_char_p * len(encoded))(*encoded)
        _lib.tui_tabs(
            self._ui, first_key, x, y, w, h, arr, len(encoded), selected,
            self._index(on_select), None,
        )

    def panel(
        self,
        key: int,
        box: tuple[float, float, float, float],
        *,
        fill: int = 0,
        radius: float = -1.0,
        border: bool = True,
    ) -> "_PanelCtx":
        """A container. `with ui.panel(...): ...` — closes on block exit."""
        x, y, w, h = box
        _lib.tui_panel(self._ui, key, x, y, w, h, fill, radius,
                       1 if border else 0)
        return _PanelCtx(self)

    # -- input injection --------------------------------------------------

    def dispatch(self, kind: int, x: float = 0.0, y: float = 0.0, *,
                 key: int = 0, codepoint: int = 0, wheel: float = 0.0,
                 shift: bool = False) -> None:
        _lib.tui_dispatch_event(self._ui, kind, x, y, key, codepoint, wheel,
                                1 if shift else 0)

    def click(self, key: int) -> None:
        """Synthetic click at the centre of a node, by key."""
        x, y, w, h = self.bounds(key)
        cx, cy = x + w * 0.5, y + h * 0.5
        self.dispatch(EVENT_MOUSE_DOWN, cx, cy)
        self.dispatch(EVENT_MOUSE_UP, cx, cy)

    def update(self) -> bool:
        """Rebuild + reconcile if the tree is dirty. True when it rebuilt."""
        return _lib.tui_update(self._ui) != 0

    # -- state ------------------------------------------------------------

    def state(self, slot: int, initial: int = 0) -> int:
        return _lib.tui_state_get(self._ui, slot, initial)

    def set_state(self, slot: int, value: int) -> None:
        _lib.tui_state_set(self._ui, slot, value)

    def invalidate(self) -> None:
        """Force a rebuild. For base properties that live outside the state
        slots — hover, focus, a value pulled from the host's own model."""
        _lib.tui_invalidate(self._ui)

    # -- direct write -----------------------------------------------------

    def find(self, key: int) -> Handle:
        return _lib.tui_node_find(self._ui, key)

    def begin_override(self, key: int) -> Handle:
        """Take a lease on a node's override channel. Fails if one is live."""
        return _lib.tui_override_begin(self._ui, key)

    def override_translate(self, lease: Handle, x: float, y: float) -> None:
        _lib.tui_override_translate(self._ui, lease, x, y)

    def override_opacity(self, lease: Handle, opacity: float) -> None:
        _lib.tui_override_opacity(self._ui, lease, opacity)

    def release_override(self, lease: Handle) -> None:
        _lib.tui_override_release(self._ui, lease)

    # -- queries ----------------------------------------------------------

    def contains(self, key: int, x: float, y: float) -> bool:
        return _lib.tui_node_contains(self._ui, key, x, y) != 0

    def bounds(self, key: int) -> tuple[float, float, float, float]:
        out = (c_float * 4)()
        _lib.tui_node_bounds(self._ui, key, out)
        return (out[0], out[1], out[2], out[3])

    def has_override(self, key: int) -> bool:
        return _lib.tui_node_has_override(self._ui, key) != 0

    def stats(self) -> Stats:
        s = Stats()
        _lib.tui_get_stats(self._ui, ctypes.byref(s))
        return s

    def dump(self) -> list[str]:
        lines: list[str] = []

        @DUMP_FN
        def cb(_user, depth, text):
            lines.append("  " * depth + text.decode("utf-8"))

        _lib.tui_dump_tree(self._ui, cb, None)
        return lines

    # -- run loop ---------------------------------------------------------

    def _install(self, app: "App") -> None:
        """Wire the callbacks and register the host. Shared by run() and the
        headless path (update()/click()), so both drive the same Python UI."""
        self._app = app
        app.ui = self
        app.user = id(app)
        _APPS[app.user] = app

        def _build(user):
            a = _APPS.get(user)
            if a:
                # The framework clears its behaviour table before every build,
                # so the callbacks from the previous build are unreachable now.
                self._callbacks.clear()
                a.build()

        def _frame(user, elapsed):
            a = _APPS.get(user)
            if a:
                a.frame(elapsed)

        def _event(user, kind, x, y, key):
            a = _APPS.get(user)
            if a:
                a.event(kind, x, y, key)

        # ctypes needs these alive for as long as the C side holds pointers.
        self._build_cb = BUILD_FN(_build)
        self._frame_cb = FRAME_FN(_frame)
        self._event_cb = EVENT_FN(_event)

        host = Host(self._build_cb, self._frame_cb, self._event_cb, app.user)
        _lib.tui_set_host(self._ui, ctypes.byref(host))

    def run(
        self,
        app: "App",
        title: str = "TacUI",
        width: int = 1280,
        height: int = 720,
    ) -> int:
        self._install(app)
        try:
            return _lib.tui_run(self._ui, title.encode("utf-8"), width, height)
        finally:
            _APPS.pop(app.user, None)

    def request_exit(self) -> None:
        _lib.tui_request_exit(self._ui)

    def capture_next_frame(self, path: str) -> None:
        _lib.tui_capture_next_frame(self._ui, path.encode("utf-8"))

    def close(self) -> None:
        if self._ui:
            _lib.tui_ui_destroy(self._ui)
            self._ui = None

    def __enter__(self) -> "Ui":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()


class _StackCtx:
    """`with ui.stack(): ...` — the Python spelling of push/pop."""

    def __init__(self, ui: Ui, key: int) -> None:
        self._ui = ui
        self._key = key

    def __enter__(self) -> "_StackCtx":
        _lib.tui_push(self._ui._ui, NODE_STACK, self._key)
        return self

    def __exit__(self, *_exc) -> None:
        _lib.tui_pop(self._ui._ui)


class _PanelCtx:
    """`with ui.panel(...): ...` — tui_panel opens a container, tui_pop closes
    it, so a panel nests its children without a manual push/pop pair."""

    def __init__(self, ui: Ui) -> None:
        self._ui = ui

    def __enter__(self) -> "_PanelCtx":
        return self

    def __exit__(self, *_exc) -> None:
        _lib.tui_pop(self._ui._ui)
