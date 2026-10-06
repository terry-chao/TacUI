#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "tacui/element.hpp"
#include "tacui/input.hpp"
#include "tacui/vnode.hpp"
#include "tacui/glyph_atlas.hpp"
#include "tacui/text_system.hpp"
#include "tacui/rhi.hpp"

namespace tac::ui {

class Ui;

struct Stats {
    uint64_t rebuilds          = 0;
    uint64_t elementsCreated   = 0;
    uint64_t elementsDestroyed = 0;
    uint64_t elementsUpdated   = 0;
    uint64_t paints            = 0;

    // Text: how many times a string was shaped. Stays flat across frames when
    // nothing changed, which is the point of shaping at reconcile time.
    uint64_t textShaped = 0;
    uint64_t glyphsRasterized = 0;
    uint64_t glyphCacheHits   = 0;
    uint64_t glyphQuads       = 0;
    uint64_t atlasUploads     = 0;

    // Rebuilds that ran while overrides were live on both sides of the
    // reconcile. This is the evidence for M0 criterion 4: an in-flight
    // animation is not disturbed by a rebuild (plan.md §6.1).
    uint64_t reconcilesWithLiveOverride = 0;

    // Nodes holding a live override right now.
    uint32_t liveOverrides = 0;
};

// What a build function is handed. Components need the Ui as well as the
// arena: the input system owns hover/press/focus, and a component has to read
// that state back to decide how to draw itself.
struct BuildContext {
    VNodeArena& arena;
    Ui&         ui;
};

// Builds a fresh VNode tree in the arena. Called only when the tree is dirty.
using BuildFn = std::function<VNode*(BuildContext&)>;

// Owns the retained tree and drives the rebuild -> reconcile -> paint cycle.
//
// M0's state model is explicit indexed slots owned here. Positional,
// @State-style binding needs the DSL layer's closure structure and arrives
// with slice F / M1 — none of the criteria this slice tests depend on it.
class Ui {
public:
    static constexpr uint32_t kMaxStateSlots = 32;

    // `fontFamily` is UTF-8, like every other string in the public API. Pass
    // nullptr to skip text entirely (text nodes then render as nothing).
    bool init(BuildFn build, const char* fontFamily = "Segoe UI");

    // Idempotent; also runs from the destructor so a host that forgets to call
    // it still releases the text system's COM apartment.
    void shutdown();

    Ui() = default;
    ~Ui() { shutdown(); }

    Ui(const Ui&)            = delete;
    Ui& operator=(const Ui&) = delete;

    // Rebuilds + reconciles if something invalidated the tree. Returns true
    // when a rebuild actually happened — the direct-write path asserts this
    // stays false across animation-only frames (criterion 3).
    bool update();

    // Walks the retained tree and submits draw primitives.
    void paint(rhi::Device& device);

    // ---- state ---------------------------------------------------------
    int64_t state(uint32_t slot, int64_t initial) const;
    void    setState(uint32_t slot, int64_t value);   // marks the tree dirty

    // Marks the tree dirty without touching state (e.g. hover moved). Anything
    // that changes a *base* property has to go through here; anything that
    // only changes an override must not.
    void invalidate() { dirty_ = true; }

    // ---- input -----------------------------------------------------------
    //
    // The host feeds raw events in; the framework does hit testing, hover and
    // press tracking, focus and tab order. Applications never write their own
    // bounding-box tests.

    void dispatchEvent(const InputEvent& ev);

    // Interaction state, owned by the input system and read by components
    // during the build. Single source of truth, so the region that lights up
    // under the pointer is by construction the region that responds to it.
    bool isHovered(Key key) const { return key != kNoKey && hoverKey_   == key; }
    bool isPressed(Key key) const { return key != kNoKey && pressedKey_ == key; }
    bool isFocused(Key key) const { return key != kNoKey && focusedKey_ == key; }

    Key hoveredKey() const { return hoverKey_; }
    Key pressedKey() const { return pressedKey_; }
    Key focusedKey() const { return focusedKey_; }

    // Moves focus to the next (or previous) focusable node in tree order.
    void focusNext(bool backwards = false);
    void clearFocus() { focusedKey_ = kNoKey; }

    // The topmost interactive node containing the point, or kNoKey. Uses *base*
    // bounds: direct-write overrides are paint-only and must not move the
    // interactive region (architecture.md §14.4).
    Key hitTest(float x, float y) const;

    // Registers a node's behaviour. Called by components during the build.
    void registerBehavior(Key key, NodeBehavior behavior);

    // ---- direct write ----------------------------------------------------
    NodeRef  find(Key key) const;
    uint32_t liveOverrideCount() const;

    const Stats&      stats() const { return stats_; }
    void              resetStats() { stats_ = Stats{}; }
    const ElementPtr& root() const { return root_; }

private:
    BuildFn    build_;
    VNodeArena arena_;
    ElementPtr root_;
    bool       dirty_ = true;

    bool    stateInit_[kMaxStateSlots]   = {};
    int64_t stateValues_[kMaxStateSlots] = {};

    // Interaction state. The behaviour table is rebuilt on every rebuild —
    // behaviours are declared by the build, not retained. The three keys below
    // survive rebuilds as long as their node does.
    std::unordered_map<Key, NodeBehavior> behaviors_;
    Key hoverKey_   = kNoKey;
    Key pressedKey_ = kNoKey;
    Key focusedKey_ = kNoKey;

    // Last pointer position, kept so the hit test can be redone after a
    // rebuild that moved things under a stationary pointer.
    float pointerX_      = 0.0f;
    float pointerY_      = 0.0f;
    bool  pointerInside_ = false;

    const NodeBehavior* behaviorOf(Key key) const;

    text::TextSystem textSystem_;
    text::GlyphAtlas atlas_;
    bool             atlasDirty_ = false;

    std::vector<rhi::SdfRect>    rects_;
    std::vector<rhi::GlyphQuad>  quads_;
    Stats                        stats_;
};

} // namespace tac::ui
