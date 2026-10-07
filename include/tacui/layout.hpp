#pragma once

// Layout value types (plan.md D6): BoxConstraints, EdgeInsets, Size.
//
// The architecture commits to a *value-type* layout contract rather than
// Flutter's "subclass RenderObject and override MeasureOverride". A value type
// crosses the C ABI; a virtual override cannot. That is the whole reason this
// header exists separately from the tree.
//
// Nothing here is retained. Constraints are computed on demand during the
// layout pass and thrown away.

namespace tac::ui {

// A float sentinel meaning "no explicit size — take the intrinsic one".
// Distinct from 0 (a zero-sized box) on purpose.
constexpr float kAuto = -1.0f;

// Large enough to dwarf any real window, small enough that arithmetic on it
// (deflate, subtraction) stays finite and never produces a NaN the way a true
// infinity would.
constexpr float kInfinity = 1.0e30f;

struct Size {
    float w = 0.0f;
    float h = 0.0f;

    constexpr bool operator==(const Size& o) const { return w == o.w && h == o.h; }
};

// Per-edge insets. `left/right` are horizontal, `top/bottom` vertical.
struct EdgeInsets {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    static constexpr EdgeInsets all(float v) { return EdgeInsets{ v, v, v, v }; }

    static constexpr EdgeInsets symmetric(float horizontal, float vertical) {
        return EdgeInsets{ horizontal, vertical, horizontal, vertical };
    }

    constexpr float horizontal() const { return left + right; }
    constexpr float vertical() const { return top + bottom; }
};

// The constraint a parent hands a child during layout. A child is free to
// choose any size inside [min, max] (tight when min == max).
//
// M2 note: layout in this release is *frame-based*. A relative container
// (row / column) is given an explicit box and distributes it among its
// children; BoxConstraints is what that distribution speaks in, and what the
// leaf/child sizing below is expressed against.
struct BoxConstraints {
    float minWidth  = 0.0f;
    float maxWidth  = kInfinity;
    float minHeight = 0.0f;
    float maxHeight = kInfinity;

    static constexpr BoxConstraints tight(float w, float h) {
        return BoxConstraints{ w, w, h, h };
    }

    static constexpr BoxConstraints tight(Size s) {
        return BoxConstraints{ s.w, s.w, s.h, s.h };
    }

    static constexpr BoxConstraints loose(float w, float h) {
        return BoxConstraints{ 0.0f, w, 0.0f, h };
    }

    static constexpr BoxConstraints loose(Size s) { return loose(s.w, s.h); }

    static constexpr BoxConstraints unbounded() { return BoxConstraints{}; }

    constexpr bool hasBoundedWidth() const { return maxWidth < kInfinity; }
    constexpr bool hasBoundedHeight() const { return maxHeight < kInfinity; }

    float constrainWidth(float w) const {
        if (w < minWidth) return minWidth;
        if (w > maxWidth) return maxWidth;
        return w;
    }

    float constrainHeight(float h) const {
        if (h < minHeight) return minHeight;
        if (h > maxHeight) return maxHeight;
        return h;
    }

    Size constrain(Size s) const {
        return Size{ constrainWidth(s.w), constrainHeight(s.h) };
    }

    // Shrinks the box by `e` on every edge, clamping at zero. Used to turn a
    // container's outer box into the content box its children are laid out in.
    BoxConstraints deflate(const EdgeInsets& e) const {
        const float h = e.horizontal();
        const float v = e.vertical();
        BoxConstraints out;
        out.minWidth  = minWidth  > h ? minWidth  - h : 0.0f;
        out.maxWidth  = maxWidth  > h ? maxWidth  - h : 0.0f;
        out.minHeight = minHeight > v ? minHeight - v : 0.0f;
        out.maxHeight = maxHeight > v ? maxHeight - v : 0.0f;
        return out;
    }

    // Drops the minimums, keeping the maximums — the constraints a container
    // measures a child with when the child is allowed to be smaller than the
    // slot it will finally occupy.
    BoxConstraints loosen() const {
        return BoxConstraints{ 0.0f, maxWidth, 0.0f, maxHeight };
    }
};

} // namespace tac::ui
