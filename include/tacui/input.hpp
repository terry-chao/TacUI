#pragma once

#include <cstdint>
#include <functional>

#include "tacui/vnode.hpp"   // Key, kNoKey

namespace tac::ui {

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

// TacUI key codes. Hosts translate their platform's codes into these; the core
// never sees a VK_* or a DOM key string.
//
// Printable input arrives as KeyCode::Character with the Unicode codepoint in
// InputEvent::codepoint, so this enum only has to cover editing and navigation.
enum class KeyCode : uint32_t {
    Unknown = 0,
    Character,
    Tab,
    Enter,
    Escape,
    Backspace,
    Delete,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    PageUp,
    PageDown,
    Space,
};

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

enum class InputEventType {
    MouseMove,
    MouseLeave,
    MouseDown,
    MouseUp,
    KeyDown,
};

struct InputEvent {
    InputEventType type = InputEventType::MouseMove;

    float    x = 0.0f;          // client-relative pixels; -1 for MouseLeave
    float    y = 0.0f;

    KeyCode  code      = KeyCode::Unknown;
    uint32_t codepoint = 0;     // valid when code == KeyCode::Character

    bool shift = false;
    bool ctrl  = false;
    bool alt   = false;
};

// ---------------------------------------------------------------------------
// Behaviour
// ---------------------------------------------------------------------------

// What a node *does*, as opposed to what it looks like. Registered during the
// build and keyed by the node's Key.
//
// Components never store their own hover/press state. The input system owns
// that and components read it back through Ui::isHovered / isPressed /
// isFocused. One source of truth means the hit region and the visual region
// cannot drift apart, which is the bug every hand-rolled button has.
struct NodeBehavior {
    // Fired when a press and its release both land inside the node. A press
    // that wanders off before release is not a click.
    std::function<void()> onClick;

    // Key input while this node has focus.
    std::function<void(const InputEvent&)> onKey;

    bool interactive = false;   // participates in hit testing
    bool focusable   = false;   // participates in the tab order
};

} // namespace tac::ui
