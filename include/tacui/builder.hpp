#pragma once

#include <vector>

#include "tacui/geometry.hpp"
#include "tacui/input.hpp"
#include "tacui/vnode.hpp"

namespace tac::ui {

class Builder;
class Ui;
struct BuildContext;

// Options for a relative container. Matches the Props idiom the controls use,
// so a row reads like any other control call.
struct StackProps {
    float      gap     = 0.0f;   // space between adjacent children
    EdgeInsets padding;
};

// Shorthand so call sites read as colours rather than as a factory call.
constexpr Color rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return Color::rgba8(r, g, b, a);
}

// RAII container scope. `Builder::stack()` opens one and its destructor closes
// it, which is C++'s spelling of the Python `with ui.stack():` block.
//
//     {
//         auto panel = b.stack();
//         b.text("inside the panel")...;
//     }   // panel closes here
class Scope {
public:
    Scope() = default;
    ~Scope();

    Scope(Scope&& other) noexcept;
    Scope& operator=(Scope&& other) noexcept;
    Scope(const Scope&)            = delete;
    Scope& operator=(const Scope&) = delete;

    // Container sizing hints. A container is one slot in its parent's layout,
    // so the same hints a leaf takes apply here too.
    Scope& width(float w);
    Scope& height(float h);
    Scope& flex(float f);
    Scope& margin(EdgeInsets m);
    Scope& margin(float all);
    Scope& align(float x, float y);
    Scope& padding(EdgeInsets p);

private:
    friend class Builder;
    Scope(Builder* owner, VNode* node) : owner_(owner), node_(node) {}
    Builder* owner_ = nullptr;
    VNode*   node_  = nullptr;
};

// Configures a leaf node. Setters chain:
//
//     b.rect().box(60, 60, 190, 58).radius(14).color(rgb(58, 104, 186)).key(kButton);
class Node {
public:
    Node& box(float x, float y, float w, float h);
    Node& box(Rect r);
    Node& radius(float r);
    Node& color(Color c);
    Node& key(Key k);
    Node& size(float fontSize);   // text nodes only

    // Sizing hints, used when this node is a child of a relative container
    // (row / column). Ignored in an absolute context, where `box` wins.
    Node& width(float w);
    Node& height(float h);
    Node& flex(float f);
    Node& margin(EdgeInsets m);
    Node& margin(float all);
    Node& align(float x, float y);

private:
    friend class Builder;
    explicit Node(VNode* node) : node_(node) {}
    VNode* node_ = nullptr;
};

// The C++ base layer of the DSL (plan.md D10): every host language wraps this
// shape in its own idiom. C++ has no usable macros, so this is a fluent
// builder rather than a `view! { }` block — a cost accepted in plan.md §11.4.
//
//     ui::VNode* build(ui::Builder& b) {
//         auto root = b.stack();
//         b.text("hello").box(20, 20, 300, 30).size(20).color(rgb(230, 230, 240));
//         b.rect().box(20, 60, 120, 40).radius(8).color(rgb(58, 104, 186));
//         return b.root();
//     }
class Builder {
public:
    explicit Builder(BuildContext& ctx);

    // Opens a container. The first call also establishes the tree root.
    [[nodiscard]] Scope stack(Key key = kNoKey);

    // Opens a container with an explicit box. Needed by anything that is itself
    // a child of a relative container (a panel is one slot in a row).
    [[nodiscard]] Scope stack(Rect box, Key key = kNoKey);

    // Relative containers: the box is the frame the children are arranged in.
    [[nodiscard]] Scope row(Rect box, const StackProps& props = {}, Key key = kNoKey);
    [[nodiscard]] Scope column(Rect box, const StackProps& props = {}, Key key = kNoKey);

    // Opens a container whose subtree is clipped to `box` — both when painted
    // and when hit tested. Scroll views are built on this.
    [[nodiscard]] Scope clip(Rect box, Key key = kNoKey);

    // Leaves are attached immediately and never pushed onto the open stack.
    Node rect();
    Node text(const char* utf8);

    // What a component needs beyond emitting nodes: the interaction state to
    // draw itself from, and somewhere to declare what it does.
    Ui& ui() const;

    // Declares what the node with this key does. Components call this instead
    // of hit-testing their own bounds.
    void behavior(Key key, NodeBehavior b);

    VNode* root() const { return root_; }
    bool   balanced() const { return open_.empty(); }

private:
    friend class Scope;

    void attach(VNode* node);
    void pop();

    VNodeArena&         arena_;
    Ui*                 ui_ = nullptr;
    std::vector<VNode*> open_;
    VNode*              root_ = nullptr;
};

} // namespace tac::ui
