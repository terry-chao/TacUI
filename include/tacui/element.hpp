#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "tacui/geometry.hpp"
#include "tacui/vnode.hpp"
#include "tacui/glyph_atlas.hpp"
#include "tacui/text_system.hpp"

namespace tac::ui {

struct Element;

// A glyph resolved against the atlas and positioned in the node's box. Cached
// on the RenderObject so paint stays a pure emit — shaping and atlas lookups
// happen at reconcile time, which is once per change rather than once per
// frame (architecture.md §3.4).
struct PlacedGlyph {
    Rect  bounds;      // pixel space, top-left origin
    float u0 = 0.0f, v0 = 0.0f;
    float u1 = 0.0f, v1 = 0.0f;
};

// Retained per-node state. Unlike a VNode this survives rebuilds, which is
// what makes the direct-write path possible: reconcile only ever touches
// `base*`, while direct writes touch only the override fields.
//
// Effective value = override while a token is alive, otherwise base.
// That single rule is the entire priority system (architecture.md §14.3).
struct RenderObject {
    // Layout result. M0 takes it straight from the VNode; M2 adds
    // Measure/Arrange with BoxConstraints.
    Rect  bounds;
    float cornerRadius = 0.0f;
    Color baseColor;

    // Text nodes. `shaped` and `placed` are derived from `text`/`fontSize` and
    // are rebuilt only when those change.
    std::string         text;
    float               fontSize = 0.0f;
    text::ShapedLine    shaped;
    std::vector<PlacedGlyph> placed;

    // Override channel — written by direct write, never by reconcile.
    Vec2  overrideTranslate{ 0.0f, 0.0f };
    float overrideOpacity = 1.0f;
    bool  overrideActive  = false;
};

// Effective geometry/colour, i.e. the one place the base/override rule lives.
Rect  effectiveBounds(const RenderObject& ro);
Color effectiveColor(const RenderObject& ro);

using ElementPtr = std::shared_ptr<Element>;

struct Element {
    VType    type = VType::Stack;
    Key      key  = kNoKey;
    Element* parent = nullptr;

    // Bumped when this element is reused for a *different* node (type or key
    // changed). Outstanding OverrideTokens compare against it, so a stale
    // token silently stops writing instead of driving the new node's
    // RenderObject. This is the M0 stand-in for the C ABI's generational
    // index (plan.md D7).
    uint32_t generation = 1;

    std::vector<ElementPtr> children;

    // Null for grouping nodes (Stack) — only render leaves own one.
    std::unique_ptr<RenderObject> render;

    // State slots. M0 addresses them by explicit index; positional binding
    // arrives with the DSL layer (slice F).
    std::vector<int64_t> states;
};

// ---------------------------------------------------------------------------
// Direct-write handle
// ---------------------------------------------------------------------------

// Holds an override alive. While any token for a node exists, the override
// wins over the base value; when the token dies the node falls back.
//
// Exactly one token per node at a time — that is what bounds this to "one
// override level" instead of WPF's six-deep precedence chain.
class OverrideToken {
public:
    OverrideToken() = default;
    ~OverrideToken();

    OverrideToken(OverrideToken&& other) noexcept;
    OverrideToken& operator=(OverrideToken&& other) noexcept;
    OverrideToken(const OverrideToken&)            = delete;
    OverrideToken& operator=(const OverrideToken&) = delete;

    explicit operator bool() const { return !element_.expired(); }
    bool valid() const { return !element_.expired(); }

    // All three are paint-only. Layout-affecting properties must go through a
    // rebuild (architecture.md §14.7 item 7).
    void setTranslate(float x, float y);
    void setOpacity(float alpha);

    void release();

private:
    friend class NodeRef;
    OverrideToken(std::weak_ptr<Element> e, uint32_t generation)
        : element_(std::move(e)), generation_(generation) {}

    // Locked accessor that also verifies the node is still the same one.
    Element* lockedTarget() const;

    std::weak_ptr<Element> element_;
    uint32_t               generation_ = 0;
};

// Stable reference to a logical node, the only handle host code ever gets.
//
// M0 backs it with a weak_ptr so a token outliving its node degrades to
// "invalid" rather than dangling. The C ABI replaces this with a generational
// index (plan.md D7) — same guarantee, no shared ownership.
class NodeRef {
public:
    NodeRef() = default;
    explicit NodeRef(std::weak_ptr<Element> e) : element_(std::move(e)) {}

    bool valid() const;

    // Fails (returns an invalid token) if the node already has one, so the
    // one-override-per-node rule is enforced rather than documented.
    OverrideToken beginOverride();

    Rect  bounds() const;   // base bounds — what hit testing uses (§14.7)
    VType type() const;

    // Read-only views of the override channel, for assertions and tooling.
    bool  hasLiveOverride() const;
    float overrideOpacity() const;
    Vec2  overrideTranslate() const;

private:
    std::weak_ptr<Element> element_;
};

// ---------------------------------------------------------------------------
// Reconcile
// ---------------------------------------------------------------------------

struct ReconcileStats {
    uint64_t created   = 0;
    uint64_t destroyed = 0;
    uint64_t updated   = 0;
    uint64_t textShaped = 0;
};

// Everything reconcile needs from outside the tree. Shaping and glyph lookup
// happen here rather than in paint, so a steady-state frame does no text work.
struct ReconcileCtx {
    text::TextSystem* textSystem = nullptr;
    text::GlyphAtlas* atlas      = nullptr;
    bool*             atlasDirty = nullptr;   // set when new glyphs were added
};

// Diffs `v` into `e`. M0 matches children positionally; key-based matching and
// list reordering land in M2 (plan.md §6).
void reconcile(Element& e, const VNode& v, ReconcileStats& stats, ReconcileCtx& ctx);

// Depth-first key lookup, used to hand out NodeRefs. M1 replaces this with an
// id -> element map maintained during reconcile.
NodeRef findByKey(const ElementPtr& root, Key key);

} // namespace tac::ui
