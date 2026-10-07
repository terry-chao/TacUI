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
#include "tacui/theme.hpp"

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

    // ---- theme -----------------------------------------------------------
    // Controls read their colours, radii and type sizes from here instead of
    // hardcoding them (components.md §5).
    const Theme& theme() const { return theme_; }
    void         setTheme(const Theme& t) { theme_ = t; invalidate(); }

    // ---- text metrics ----------------------------------------------------
    // Measured through the same shaper that draws, so a centred label is
    // centred against the pixels that actually appear. Returns 0 when there
    // is no text system (init() with fontFamily == nullptr).
    float measureText(const char* utf8, float sizePx) const;
    float textAscent(float sizePx) const;
    float textDescent(float sizePx) const;

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
    // True while this node owns the drag capture — a slider thumb, say, that
    // the pointer may have left while the button is still down.
    bool isCaptured(Key key) const { return key != kNoKey && capturedKey_ == key; }

    Key hoveredKey() const { return hoverKey_; }
    Key pressedKey() const { return pressedKey_; }
    Key focusedKey() const { return focusedKey_; }
    Key capturedKey() const { return capturedKey_; }

    // Moves focus to the next (or previous) focusable node in tree order.
    void focusNext(bool backwards = false);
    void clearFocus() { focusedKey_ = kNoKey; }

    // The topmost interactive node containing the point, or kNoKey. Uses *base*
    // bounds: direct-write overrides are paint-only and must not move the
    // interactive region (architecture.md §14.4).
    Key hitTest(float x, float y) const;

    // The hit chain, deepest first: the node under the pointer followed by the
    // interactive ancestors it can bubble to. Exposed so tooling and tests can
    // assert on propagation without duplicating the walk.
    void hitChain(float x, float y, std::vector<Key>& out) const;

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
    Key capturedKey_ = kNoKey;

    // Last pointer position, kept so the hit test can be redone after a
    // rebuild that moved things under a stationary pointer.
    float pointerX_      = 0.0f;
    float pointerY_      = 0.0f;
    bool  pointerInside_ = false;

    const NodeBehavior* behaviorOf(Key key) const;

    // Deepest-first walk used by both hitTest() and bubbling. `clip` carries
    // the accumulated clip rectangle; `clipped` says whether it is active.
    void collectHitChain(const Element* e, float x, float y,
                         Rect clip, bool clipped,
                         std::vector<const Element*>& out) const;

    Element* findElement(Key key) const;

    void mouseDown(float x, float y);
    void mouseUp(float x, float y);
    void mouseWheel(float x, float y, float lines);
    void keyDown(const InputEvent& ev);

    // Scratch for the hit walk, kept across events so dispatching does not
    // allocate on every mouse move.
    mutable std::vector<const Element*> chain_;

    Theme theme_ = darkTheme();

    text::TextSystem textSystem_;
    text::GlyphAtlas atlas_;
    bool             atlasDirty_ = false;

    // Paint submission is an ordered list of runs: a maximal stretch of
    // same-type primitives. Rect and glyph runs are interleaved in tree order
    // and paint issues one draw call per run, which is what makes "a rect under
    // a later text node" draw under it — the old two-batch split drew every
    // rect before every glyph.
    struct DrawRun {
        bool     text  = false;
        uint32_t begin = 0;   // index into quads_ (text) or rects_
        uint32_t count = 0;
    };

    void collectPaint(const Element& e, Rect clip, bool clipped);
    void pushRect(const rhi::SdfRect& r);
    void pushQuad(const rhi::GlyphQuad& q);

    std::vector<rhi::SdfRect>   rects_;
    std::vector<rhi::GlyphQuad> quads_;
    std::vector<DrawRun>        runs_;
    uint64_t                    glyphQuadCount_ = 0;
    uint32_t                    liveOverrides_  = 0;
    Stats                       stats_;
};

} // namespace tac::ui
