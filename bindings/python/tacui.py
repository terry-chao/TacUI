"""ctypes binding for the TacUI C ABI (capi/tacui.h).

Hand-written on purpose. Per plan.md §10 the FFI plumbing is the mechanical
part; the idiomatic layer on top is the part every host language has to write
itself, and this file is the Python shape of it.

The split this demonstrates:
  * C++ owns the window, the GPU device and the frame loop  (tui_run)
  * Python owns the UI: the build callback, event handling and animations

Requires tacui.dll, built from the capi/ target.
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
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        os.path.join(here, "tacui.dll"),
        os.path.join(here, "..", "..", "build", "bin", "tacui.dll"),
        os.path.join(here, "..", "..", "build", "bin", "Debug", "tacui.dll"),
    ]
    for path in candidates:
        path = os.path.normpath(path)
        if os.path.isfile(path):
            return ctypes.CDLL(path)

    raise OSError(
        "tacui.dll not found. Build the capi target:\n"
        "  cmake --build build --config Debug --target tacui"
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

    def run(
        self,
        app: "App",
        title: str = "TacUI",
        width: int = 1280,
        height: int = 720,
    ) -> int:
        self._app = app
        app.ui = self
        app.user = id(app)
        _APPS[app.user] = app

        def _build(user):
            a = _APPS.get(user)
            if a:
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
