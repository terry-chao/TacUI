// TacUI — the C ABI. This is the *only* boundary host languages see
// (plan.md D7, architecture.md §5).
//
// Rules this header obeys, and must keep obeying:
//   * no C++ types, no exceptions, no STL containers across the line
//   * everything is an opaque handle or a fixed-layout POD
//   * strings are UTF-8 (ptr, len) or NUL-terminated UTF-8
//   * nodes are addressed by generational handles, never raw pointers
//   * every entry point is versioned and queryable
//
// M0 note: the build plane is chatty by design (3 FFI calls per node).
// architecture.md §5.2 says to start simple and only batch once profiling
// says so; a 5-node tree rebuilding 64 times is ~1000 calls total.

#ifndef TACUI_H
#define TACUI_H

#include <stdint.h>

#if defined(_WIN32)
#  if defined(TUI_BUILD_DLL)
#    define TUI_API __declspec(dllexport)
#  else
#    define TUI_API
#  endif
#else
#  define TUI_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// 2: the input callback now hands back a `tui_event` (it carries the wheel
//    delta and a translated character), replacing the old five-parameter form.
#define TUI_ABI_VERSION 2u

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------

// Generational handle. `index` 0 is always null; a stale handle is detected by
// comparing `generation`, so a freed slot can never be mistaken for its
// successor. This is the C-side twin of ui::Element::generation.
typedef struct {
    uint32_t index;
    uint32_t generation;
} tui_handle;

TUI_API tui_handle tui_handle_null(void);
TUI_API int32_t    tui_handle_valid(tui_handle h);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

typedef struct tui_ui tui_ui;

TUI_API uint32_t tui_abi_version(void);

TUI_API tui_ui* tui_ui_create(void);
TUI_API void    tui_ui_destroy(tui_ui* ui);

// ---------------------------------------------------------------------------
// Node kinds and events (mirror the C++ enums)
// ---------------------------------------------------------------------------

enum {
    TUI_NODE_STACK = 0,
    TUI_NODE_RECT  = 1,
    TUI_NODE_TEXT  = 2,
};

enum {
    TUI_EVENT_RESIZE      = 0,
    TUI_EVENT_CLOSE       = 1,
    TUI_EVENT_MOUSE_MOVE  = 2,
    TUI_EVENT_MOUSE_LEAVE = 3,
    TUI_EVENT_MOUSE_DOWN  = 4,
    TUI_EVENT_MOUSE_UP    = 5,
    TUI_EVENT_KEY_DOWN    = 6,
    TUI_EVENT_MOUSE_WHEEL = 7,
    TUI_EVENT_CHAR        = 8,   // `codepoint` is valid
};

// Button styles, mirroring ui::ButtonStyle.
enum {
    TUI_BUTTON_PRIMARY   = 0,
    TUI_BUTTON_SECONDARY = 1,
    TUI_BUTTON_GHOST     = 2,
    TUI_BUTTON_DANGER    = 3,
};

// ---------------------------------------------------------------------------
// Host callbacks
// ---------------------------------------------------------------------------

// Invoked when the tree needs rebuilding. Must emit exactly one root node via
// tui_push / tui_pop.
typedef void (*tui_build_fn)(void* user);

// Invoked once per frame, before the build. This is where a host drives
// animations through the direct-write channel — writing an override here must
// never cause a rebuild (M0 criterion 3).
typedef void (*tui_frame_fn)(void* user, double elapsed_seconds);

// One input event. A struct rather than a widening parameter list, because the
// vocabulary now includes the wheel delta and a translated character.
typedef struct {
    int32_t  type;       // one of TUI_EVENT_*
    float    x;          // client-relative pixels
    float    y;
    uint32_t key;        // TUI_KEY_* for TUI_EVENT_KEY_DOWN
    uint32_t codepoint;  // TUI_EVENT_CHAR
    float    wheel;      // TUI_EVENT_MOUSE_WHEEL, notches
    int32_t  shift;      // modifier state
} tui_event;

typedef void (*tui_event_fn)(void* user, const tui_event* event);

// sizeof(tui_event) as the library was compiled. Hosts should compare it
// against their own mirror before trusting the layout — the same guard
// tui_stats_size provides, and the reason it exists.
TUI_API uint32_t tui_event_size(void);

typedef struct {
    tui_build_fn build;
    tui_frame_fn frame;
    tui_event_fn event;
    void*        user;
} tui_host;

TUI_API void tui_set_host(tui_ui* ui, const tui_host* host);

// ---------------------------------------------------------------------------
// Build plane — valid only inside a `build` callback
// ---------------------------------------------------------------------------

TUI_API void tui_push(tui_ui* ui, uint32_t kind, uint64_t key);
TUI_API void tui_pop(tui_ui* ui);

// Properties of the node most recently pushed. `rgba8` is R in the low byte.
TUI_API void tui_rect(tui_ui* ui, float x, float y, float w, float h,
                      float corner_radius, uint32_t rgba8);

// Same, for a text node. `utf8` is copied into the arena, so the caller's
// buffer only has to outlive the call. The baseline sits at y + ascent(size).
TUI_API void tui_text(tui_ui* ui, float x, float y, float w, float h,
                      float font_size, uint32_t rgba8, const char* utf8);

// ---------------------------------------------------------------------------
// Build plane — controls
//
// The L2 control library, exported so a host language gets the same widgets the
// C++ side has rather than re-implementing hover / hit testing / focus
// (docs/components.md §4). Each is a *function*; none owns state.
//
// Like tui_push / tui_rect, every call here is only valid inside a build
// callback. Colours are the same RGBA8 layout as tui_rect; alpha 0 means
// "unset — take it from the theme".
//
// Callbacks take the host's own `user` pointer, which the binding uses to route
// back to its object (the C ABI never holds a language closure).
// ---------------------------------------------------------------------------

typedef void (*tui_click_fn)(void* user);
typedef void (*tui_bool_fn)(void* user, int32_t value);
typedef void (*tui_float_fn)(void* user, float value);
typedef void (*tui_index_fn)(void* user, int32_t index);

TUI_API void tui_label(tui_ui* ui, uint64_t key,
                       float x, float y, float w, float h,
                       const char* utf8, float size, uint32_t rgba8, int32_t strong);

TUI_API void tui_button(tui_ui* ui, uint64_t key,
                        float x, float y, float w, float h,
                        const char* utf8, uint32_t style, int32_t enabled,
                        tui_click_fn cb, void* user);

TUI_API void tui_checkbox(tui_ui* ui, uint64_t key,
                          float x, float y, float w, float h,
                          const char* utf8, int32_t checked, int32_t enabled,
                          tui_bool_fn cb, void* user);

TUI_API void tui_radio(tui_ui* ui, uint64_t key,
                       float x, float y, float w, float h,
                       const char* utf8, int32_t selected, int32_t enabled,
                       tui_click_fn cb, void* user);

TUI_API void tui_toggle(tui_ui* ui, uint64_t key,
                        float x, float y, float w, float h,
                        const char* utf8, int32_t on, int32_t enabled,
                        tui_bool_fn cb, void* user);

TUI_API void tui_slider(tui_ui* ui, uint64_t key,
                        float x, float y, float w, float h,
                        float value, float min, float max, float step,
                        int32_t enabled, tui_float_fn cb, void* user);

TUI_API void tui_progress(tui_ui* ui, uint64_t key,
                          float x, float y, float w, float h,
                          float value, uint32_t rgba8);

TUI_API void tui_divider(tui_ui* ui, uint64_t key,
                         float x, float y, float w, float h, uint32_t rgba8);

// `labels` is an array of `count` NUL-terminated UTF-8 strings, borrowed for
// the duration of the call. Segment keys are `first_key + i`.
TUI_API void tui_tabs(tui_ui* ui, uint64_t first_key,
                      float x, float y, float w, float h,
                      const char* const* labels, int32_t count, int32_t selected,
                      tui_index_fn cb, void* user);

// Opens a themed container. Children pushed after this call land inside it, so
// the host must close it with tui_pop — the same push/pop contract as
// tui_push(TUI_NODE_STACK, key).
TUI_API void tui_panel(tui_ui* ui, uint64_t key,
                       float x, float y, float w, float h,
                       uint32_t fill, float radius, int32_t border);

// --- stateful controls -----------------------------------------------------
//
// textInput / scrollView / listView keep cross-frame state (an edit buffer, a
// scroll offset). Their C++ controls hold a reference to that state inside the
// behaviour they register, so it has to outlive the build — the library owns it
// here, keyed by the control's own key, and the host reads it back through the
// accessors below rather than passing a buffer in.

typedef void (*tui_text_fn)(void* user, const char* utf8);

TUI_API void tui_text_input(tui_ui* ui, uint64_t key,
                            float x, float y, float w, float h,
                            const char* placeholder, const char* suffix,
                            float size, int32_t enabled, int32_t max_length,
                            tui_text_fn cb, void* user);

// Copies the field's current text into `out` (NUL-terminated) and returns its
// byte length. Pass out = NULL to query the length. `cap` includes the NUL.
TUI_API int32_t tui_text_get(tui_ui* ui, uint64_t key, char* out, int32_t cap);

// Replaces the field's text; marks the tree dirty.
TUI_API void tui_text_set(tui_ui* ui, uint64_t key, const char* utf8);

// Opens a scrollable region (a clip container) keyed by `key`; close it with
// tui_pop, then draw the bar with tui_scroll_bar. Read tui_scroll_offset to
// place the content.
TUI_API void tui_scroll_view(tui_ui* ui, uint64_t key,
                             float x, float y, float w, float h,
                             float content_height, float line_step);

TUI_API float tui_scroll_offset(tui_ui* ui, uint64_t key);
TUI_API void  tui_scroll_set_offset(tui_ui* ui, uint64_t key, float offset);

// `bar_key` owns the bar's hit region; `scroll_key` names the scroll view whose
// offset it drives (that is where the shared state lives).
TUI_API void tui_scroll_bar(tui_ui* ui, uint64_t bar_key, uint64_t scroll_key,
                            float x, float y, float w, float h,
                            float content_height, float width, float min_thumb);

TUI_API void tui_list_view(tui_ui* ui, uint64_t key,
                           float x, float y, float w, float h,
                           const char* const* labels,
                           const char* const* secondary,
                           int32_t count, int32_t selected,
                           float row_height, float row_gap, int32_t show_bar,
                           uint64_t row_key_base,
                           tui_index_fn cb, void* user);

TUI_API float tui_list_offset(tui_ui* ui, uint64_t key);

// ---------------------------------------------------------------------------
// Input injection
// ---------------------------------------------------------------------------

// Feeds one event straight into the framework, the same path the run loop uses.
// For an embedding host that owns its window loop and forwards its own events.
//
//   type      one of the TUI_EVENT_* constants
//   x, y      client-relative pixels
//   key       virtual-key-independent code: for TUI_EVENT_KEY_DOWN it is a
//             TUI_KEY_* value; for TUI_EVENT_CHAR, `codepoint` is used instead
//   codepoint Unicode codepoint, for TUI_EVENT_CHAR
//   wheel     notches, positive away from the user, for TUI_EVENT_MOUSE_WHEEL
//   shift     modifier state for keyboard events
TUI_API void tui_dispatch_event(tui_ui* ui, int32_t type, float x, float y,
                                uint32_t key, uint32_t codepoint, float wheel,
                                int32_t shift);

// Rebuilds and reconciles if the tree is dirty. Returns 1 when a rebuild
// actually happened. tui_run drives this every frame; an embedding host that
// owns its own loop — or a headless test — calls it directly.
TUI_API int32_t tui_update(tui_ui* ui);

// Overrides the DPI-derived UI scale. 0 (the default) follows the window's
// monitor; 1.0 renders DPI-independently, which a golden-image capture wants so
// the same build produces the same pixels on any display. Call before tui_run.
TUI_API void tui_set_ui_scale(tui_ui* ui, float scale);

// Key codes accepted by tui_dispatch_event for TUI_EVENT_KEY_DOWN. Printable
// input goes through TUI_EVENT_CHAR instead. Mirrors ui::KeyCode.
enum {
    TUI_KEY_UNKNOWN   = 0,
    TUI_KEY_TAB       = 2,
    TUI_KEY_ENTER     = 3,
    TUI_KEY_ESCAPE    = 4,
    TUI_KEY_BACKSPACE = 5,
    TUI_KEY_DELETE    = 6,
    TUI_KEY_LEFT      = 7,
    TUI_KEY_RIGHT     = 8,
    TUI_KEY_UP        = 9,
    TUI_KEY_DOWN      = 10,
    TUI_KEY_HOME      = 11,
    TUI_KEY_END       = 12,
    TUI_KEY_PAGE_UP   = 13,
    TUI_KEY_PAGE_DOWN = 14,
    TUI_KEY_SPACE     = 15,
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

TUI_API int64_t tui_state_get(const tui_ui* ui, uint32_t slot, int64_t fallback);

// Marks the tree dirty; the rebuild happens on the next frame.
TUI_API void tui_state_set(tui_ui* ui, uint32_t slot, int64_t value);

// Marks the tree dirty without touching state — for base properties that live
// outside the state slots (hover, focus, a value read from the host's own
// model). Anything that changes an *override* must not call this.
TUI_API void tui_invalidate(tui_ui* ui);

// ---------------------------------------------------------------------------
// Direct write (the override channel)
// ---------------------------------------------------------------------------

// Resolves a key to a handle without taking a lease. Returns null if absent.
TUI_API tui_handle tui_node_find(tui_ui* ui, uint64_t key);

// Takes a lease on a node's override channel. Fails (returns null) if the node
// already has a live override — only one level exists by design (§14.3).
// The lease ends at tui_override_release, or when the node stops existing.
TUI_API tui_handle tui_override_begin(tui_ui* ui, uint64_t key);

TUI_API void tui_override_translate(tui_ui* ui, tui_handle lease, float x, float y);
TUI_API void tui_override_opacity(tui_ui* ui, tui_handle lease, float opacity);
TUI_API void tui_override_release(tui_ui* ui, tui_handle lease);

// ---------------------------------------------------------------------------
// Node queries
// ---------------------------------------------------------------------------

// 1 if the point is inside the node's *base* bounds. Overrides are paint-only
// and deliberately do not move the interactive region (§14.4).
TUI_API int32_t tui_node_contains(tui_ui* ui, uint64_t key, float x, float y);

// Writes {x, y, w, h} into out_xywh (must have room for 4 floats).
TUI_API void tui_node_bounds(tui_ui* ui, uint64_t key, float* out_xywh);

TUI_API int32_t tui_node_has_override(tui_ui* ui, uint64_t key);

// ---------------------------------------------------------------------------
// Diagnostics — M0 criterion 6
// ---------------------------------------------------------------------------

typedef struct {
    uint64_t rebuilds;
    uint64_t elements_created;
    uint64_t elements_destroyed;
    uint64_t elements_updated;
    uint64_t paints;
    uint64_t reconciles_with_live_override;
    uint32_t live_overrides;

    uint64_t text_shaped;
    uint64_t glyphs_rasterized;
    uint64_t glyph_cache_hits;
    uint64_t glyph_quads;
    uint64_t atlas_uploads;
} tui_stats;

// sizeof(tui_stats) as the library was compiled.
//
// Hosts MUST check this before calling tui_get_stats. A binding that mirrors
// the struct by hand can drift out of sync with it, and ctypes will not
// notice — the counters then write straight past the end of the host's
// buffer. Comparing this against the binding's own sizeof turns a silent
// memory corruption into a loud failure at load time.
TUI_API uint32_t tui_stats_size(void);

TUI_API void tui_get_stats(const tui_ui* ui, tui_stats* out);

// One call per node, depth-first. `text` is NUL-terminated UTF-8 and is only
// valid for the duration of the call. `depth` is the tree depth.
typedef void (*tui_dump_fn)(void* user, int32_t depth, const char* text);
TUI_API void tui_dump_tree(tui_ui* ui, tui_dump_fn fn, void* user);

// ---------------------------------------------------------------------------
// Run loop
// ---------------------------------------------------------------------------

// Creates the window and the renderer, builds the tree once, then loops until
// the window closes or tui_request_exit is called. Returns 0 on a clean exit.
//
// `title_utf8` is UTF-8 like every other string in this header. wchar_t is
// deliberately not used: its width differs between Windows and elsewhere, so
// it cannot appear in an ABI meant to be portable.
//
// The initial build happens before the first `frame` callback, so a host can
// resolve handles as soon as its first frame runs. Nodes that appear
// conditionally may still be absent — a host must handle a null handle.
TUI_API int32_t tui_run(tui_ui* ui, const char* title_utf8,
                        uint32_t width, uint32_t height);

// Asks the run loop to stop after the current frame.
TUI_API void tui_request_exit(tui_ui* ui);

// Captures the next frame to a PNG (UTF-8 path) and then stops the loop.
// Used by the golden-image harness.
TUI_API void tui_capture_next_frame(tui_ui* ui, const char* path_utf8);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // TACUI_H
