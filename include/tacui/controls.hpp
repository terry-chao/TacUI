#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "tacui/builder.hpp"
#include "tacui/geometry.hpp"
#include "tacui/input.hpp"
#include "tacui/theme.hpp"
#include "tacui/vnode.hpp"

namespace tac::ui {

// L2 composite controls (docs/components.md §3).
//
// Every one of these is a *function* that emits VNodes into the arena and
// registers its behaviour with the input system. None of them is a class and
// none of them owns state — that is what makes the same control usable from
// C++, from the C ABI, and later from a Rust or C# wrapper without the
// semantics drifting (docs/components.md §4, decision C2).
//
// Two properties fall out of the shape and are worth stating outright:
//
//   * the visual box and the hit box are the same `box` argument, so they
//     cannot drift apart;
//   * hover / press / focus are read back from Ui instead of tracked locally,
//     so the region that lights up is by construction the region that
//     responds.

class Builder;
class Scope;

using ClickFn = std::function<void()>;
using BoolFn  = std::function<void(bool)>;
using FloatFn = std::function<void(float)>;
using IndexFn = std::function<void(int)>;
using TextFn  = std::function<void(const std::string&)>;
using SelectFn = std::function<void(int)>;   // list selection

// `alpha == 0` means "unset — take it from the Theme".
constexpr Color kUnset = Color{ 0.0f, 0.0f, 0.0f, 0.0f };
inline bool isSet(const Color& c) { return c.a > 0.0f; }

// ---------------------------------------------------------------------------
// Props
// ---------------------------------------------------------------------------

enum class ButtonStyle {
    Primary,     // filled accent — the one action you want people to hit
    Secondary,   // outlined surface
    Ghost,       // text only, background appears on hover
    Danger,      // filled red
};

struct LabelProps {
    const char* text   = "";
    float       size   = 0.0f;       // 0 = Theme::fontMd
    Color       color  = kUnset;     // unset = Theme::textDim
    bool        strong = false;      // unset colour picks textStrong instead
};

struct ButtonProps {
    const char* label   = "";
    ButtonStyle style   = ButtonStyle::Secondary;
    bool        enabled = true;
    float       radius  = 0.0f;      // 0 = Theme::radiusMd
};

struct CheckboxProps {
    const char* label   = "";
    bool        checked = false;
    bool        enabled = true;
};

struct RadioProps {
    const char* label    = "";
    bool        selected = false;
    bool        enabled  = true;
};

struct SwitchProps {
    const char* label   = "";
    bool        on      = false;
    bool        enabled = true;
};

struct SliderProps {
    float value  = 0.0f;             // in [min, max]
    float min    = 0.0f;
    float max    = 1.0f;
    float step   = 0.0f;             // 0 = continuous; else arrow-key step
    bool  enabled = true;
};

struct ProgressProps {
    float value = 0.0f;              // 0..1
    Color color = kUnset;            // unset = Theme::accent
};

struct DividerProps {
    Color color = kUnset;            // unset = Theme::border
};

struct PanelProps {
    Color fill   = kUnset;           // unset = Theme::surface
    float radius = -1.0f;            // < 0 = Theme::radiusMd
    bool  border = true;
};

struct ListItemProps {
    const char* label     = "";
    const char* secondary = "";      // right-aligned, e.g. a count
    bool        selected  = false;
    bool        enabled   = true;
    float       labelSize = 0.0f;    // 0 = Theme::fontMd
};

struct TabsProps {
    // One key per tab: the control uses `firstKey + index`.
    const char* const* labels   = nullptr;
    int                count    = 0;
    int                selected = 0;
};

struct ScrollViewProps {
    // Total height of the content the caller is going to emit. The control
    // needs it to clamp the offset and size the thumb; it cannot measure the
    // children, because there is no layout engine yet.
    float contentHeight = 0.0f;
    float lineStep      = 48.0f;   // pixels per wheel notch
};

struct ScrollViewState {
    float offset = 0.0f;
};

struct ScrollBarProps {
    float contentHeight = 0.0f;
    float width         = 8.0f;
    float minThumb      = 28.0f;
};

struct ListViewProps {
    const char* const* labels    = nullptr;
    const char* const* secondary = nullptr;   // may be null
    int   count       = 0;
    int   selected    = -1;
    float rowHeight   = 30.0f;
    float rowGap      = 2.0f;
    bool  showBar     = true;

    // Rows use `rowKeyBase + i`; 0 means `key + 2`. See the key-range note on
    // listView() below.
    Key rowKeyBase = 0;
};

struct ListViewState {
    float offset = 0.0f;
};

struct TextInputProps {
    const char* placeholder = "";
    const char* suffix      = "";    // drawn right-aligned, dimmed
    float       size        = 0.0f;  // 0 = Theme::fontMd
    bool        enabled     = true;
    int         maxLength   = 0;     // 0 = unlimited
};

// TextInput edits a buffer the *host* owns.
//
// docs/components.md files TextInput under L1 ("needs cross-frame core
// state"). This first cut keeps the buffer host-side instead, because that is
// what makes it usable from every host language immediately, including the
// C ABI, where a std::string cannot cross. Moving cursor and selection onto
// the Element is the L1 step and does not change this API.
struct TextInputState {
    std::string text;
    int cursor = 0;   // byte offset into `text`, always on a codepoint edge
    int anchor = 0;   // selection anchor; == cursor when there is no selection

    bool hasSelection() const { return anchor != cursor; }
    int  selectionMin() const { return anchor < cursor ? anchor : cursor; }
    int  selectionMax() const { return anchor < cursor ? cursor : anchor; }
    void selectAll() { anchor = 0; cursor = static_cast<int>(text.size()); }
    void clearSelection() { anchor = cursor; }
    void moveTo(int offset) { cursor = anchor = offset; }
};

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

void label(Builder& b, Key key, Rect box, const LabelProps& props);

void button(Builder& b, Key key, Rect box, const ButtonProps& props,
            ClickFn onClick = {});

void checkbox(Builder& b, Key key, Rect box, const CheckboxProps& props,
              BoolFn onChange = {});

void radio(Builder& b, Key key, Rect box, const RadioProps& props,
           ClickFn onSelect = {});

void toggle(Builder& b, Key key, Rect box, const SwitchProps& props,
            BoolFn onChange = {});

// value in [min, max]; click-to-jump and drag both work, via input capture.
void slider(Builder& b, Key key, Rect box, const SliderProps& props,
            FloatFn onChange = {});

void progressBar(Builder& b, Key key, Rect box, const ProgressProps& props);

void divider(Builder& b, Key key, Rect box, const DividerProps& props = {});

// Container: returns the Scope that owns the children, so
//
//     auto card = panel(b, kCard, box);
//     label(b, kTitle, ...);      // lands inside the card
//
// The caller still positions children absolutely — real layout is M2 and this
// control does not pretend otherwise.
[[nodiscard]] Scope panel(Builder& b, Key key, Rect box,
                          const PanelProps& props = {});

void listItem(Builder& b, Key key, Rect box, const ListItemProps& props,
              ClickFn onClick = {});

void tabs(Builder& b, Key firstKey, Rect box, const TabsProps& props,
          IndexFn onSelect = {});

// Scrollable region. Returns the clip scope that owns the content, so:
//
//     ui::ScrollViewState sv;
//     {
//         auto view = ui::scrollView(b, kScroll, box, sv, { .contentHeight = h });
//         // emit children at box.y - sv.offset
//     }
//     ui::scrollBar(b, kScrollBar, box, sv, { .contentHeight = h });
//
// The wheel bubbles: if this view is already at the end it declines, and the
// gesture goes to an ancestor scroll view instead.
[[nodiscard]] Scope scrollView(Builder& b, Key key, Rect box,
                               ScrollViewState& state,
                               const ScrollViewProps& props = {});

// The thumb, drawn outside the clip so the content cannot cover it. Takes the
// offset by reference rather than a ScrollViewState, so a list view can share
// it without a second state type.
void scrollBar(Builder& b, Key key, Rect box, float& offset,
               const ScrollBarProps& props = {});

// Virtualised list: only the rows intersecting the viewport are emitted, so a
// list of ten thousand costs the same as a list of ten.
//
// Composite controls own a contiguous key range (same convention as tabs):
// `key` is the container (wheel), `key + 1` the scrollbar thumb, and
// `key + 2 + i` row i — or `rowKeyBase + i` when that is set.
void listView(Builder& b, Key key, Rect box, const ListViewProps& props,
              ListViewState& state, SelectFn onSelect = {});

void textInput(Builder& b, Key key, Rect box, const TextInputProps& props,
               TextInputState& state, TextFn onChange = {});

} // namespace tac::ui
