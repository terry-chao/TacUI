#include "tacui/ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace tac::ui {

namespace {

uint32_t countOverrides(const Element& e) {
    uint32_t n = (e.render && e.render->overrideActive) ? 1u : 0u;
    for (const auto& c : e.children) {
        n += countOverrides(*c);
    }
    return n;
}

Rect intersectRect(const Rect& a, const Rect& b) {
    const float x0 = std::max(a.x, b.x);
    const float y0 = std::max(a.y, b.y);
    const float x1 = std::min(a.right(), b.right());
    const float y1 = std::min(a.bottom(), b.bottom());
    return Rect{ x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0) };
}

void collectFocusable(const Element& e,
                      const std::unordered_map<Key, NodeBehavior>& behaviors,
                      std::vector<Key>& out) {
    const auto b = behaviors.find(e.key);
    if (b != behaviors.end() && b->second.focusable) {
        out.push_back(e.key);
    }
    for (const auto& c : e.children) {
        collectFocusable(*c, behaviors, out);
    }
}

} // namespace

// Paint order is tree order, i.e. the painter's algorithm — across node types.
//
// Rectangles and glyphs land in separate buffers but share one ordered run
// list, so paint submits them interleaved in the order the tree was walked.
// (M0 drew two whole batches, rects first, which was wrong whenever text sat
// under a later rect.)
void Ui::collectPaint(const Element& e, Rect clip, bool clipped) {
    // A clip container narrows the scissor for everything below it. Folding it
    // in here means the primitive carries its own final clip, so the backend
    // never has to know the tree shape — and one instanced draw call still
    // covers the whole frame.
    if (e.clipsChildren) {
        clip    = clipped ? intersectRect(clip, e.clipBounds) : e.clipBounds;
        clipped = true;
    }
    const rhi::ClipRect scissor = clipped
        ? rhi::ClipRect{ clip.x, clip.y, clip.right(), clip.bottom(), true }
        : rhi::ClipRect{};

    if (e.render) {
        const RenderObject& ro = *e.render;
        if (ro.overrideActive) ++liveOverrides_;

        if (e.type == VType::Rect) {
            rhi::SdfRect r{};
            r.bounds       = effectiveBounds(ro);
            r.cornerRadius = ro.cornerRadius;
            r.color        = effectiveColor(ro);
            r.clip         = scissor;
            pushRect(r);
        } else if (e.type == VType::Text) {
            // The glyph cache holds node-local positions; the node origin is
            // applied here. Snap the *final* origin to whole pixels — drawing a
            // bitmap glyph at a fractional origin makes the linear sampler
            // resample it, which is what turns UI text to mush. The pen advance
            // inside the run stays fractional, so spacing is unaffected.
            const Rect  ob   = effectiveBounds(ro);
            const Color tint = effectiveColor(ro);

            for (const PlacedGlyph& g : ro.placed) {
                rhi::GlyphQuad q{};
                q.bounds = Rect{ std::round(ob.x + g.bounds.x),
                                 std::round(ob.y + g.bounds.y),
                                 g.bounds.w, g.bounds.h };
                q.u0 = g.u0;
                q.v0 = g.v0;
                q.u1 = g.u1;
                q.v1 = g.v1;
                q.color = tint;
                q.clip  = scissor;
                pushQuad(q);
            }
        }
    }

    for (const auto& c : e.children) {
        collectPaint(*c, clip, clipped);
    }
}

void Ui::pushRect(const rhi::SdfRect& r) {
    if (runs_.empty() || runs_.back().text) {
        runs_.push_back(DrawRun{ false, static_cast<uint32_t>(rects_.size()), 0 });
    }
    rects_.push_back(r);
    ++runs_.back().count;
}

void Ui::pushQuad(const rhi::GlyphQuad& q) {
    if (runs_.empty() || !runs_.back().text) {
        runs_.push_back(DrawRun{ true, static_cast<uint32_t>(quads_.size()), 0 });
    }
    quads_.push_back(q);
    ++runs_.back().count;
    ++glyphQuadCount_;
}

bool Ui::init(BuildFn build, const char* fontFamily) {
    build_ = std::move(build);
    dirty_ = true;
    root_.reset();

    if (fontFamily) {
        if (!textSystem_.init(fontFamily)) {
            std::fprintf(stderr, "[ui] text system unavailable; text nodes will be blank\n");
        } else if (!atlas_.init(rhi::kGlyphAtlasSize)) {
            std::fprintf(stderr, "[ui] glyph atlas allocation failed\n");
            textSystem_.shutdown();
        }
    }
    return true;
}

void Ui::shutdown() {
    atlas_.shutdown();
    textSystem_.shutdown();
    root_.reset();
}

bool Ui::update() {
    if (!dirty_ || !build_) return false;
    dirty_ = false;

    // Snapshot override liveness before the rebuild so we can tell whether an
    // in-flight direct write survived it (criterion 4).
    const uint32_t overridesBefore = root_ ? countOverrides(*root_) : 0;

    // Hover is re-evaluated before building rather than only on mouse move:
    // the pointer has not moved, but the tree may have, so what sits under it
    // can have changed. Doing it here means the build below already sees the
    // right answer instead of needing a second pass.
    if (pointerInside_) {
        hoverKey_ = hitTest(pointerX_, pointerY_);
    }

    arena_.reset();

    // Behaviours are declared by the build, so they are rebuilt along with it.
    // A component that stops being emitted simply stops responding.
    behaviors_.clear();

    BuildContext buildCtx{ arena_, *this };
    VNode* v = build_(buildCtx);
    if (!v) {
        std::fprintf(stderr, "[ui] build function returned null\n");
        return false;
    }

    if (!root_) {
        root_ = std::make_shared<Element>();
        root_->type = VType::Stack;
    }

    ReconcileCtx ctx;
    ctx.textSystem = textSystem_.valid() ? &textSystem_ : nullptr;
    ctx.atlas      = atlas_.valid() ? &atlas_ : nullptr;
    ctx.atlasDirty = &atlasDirty_;

    ReconcileStats rs;
    reconcile(*root_, *v, rs, ctx);

    // Layout runs after reconcile because it needs shaped text to know an
    // intrinsic size, and reconcile is what shapes. Absolute subtrees are left
    // untouched, so a tree with no relative container is unchanged.
    layoutTree(*root_, ctx);

    // Focus survives rebuilds while the node does; clear it only if the node
    // was removed.
    if (focusedKey_ != kNoKey && !findByKey(root_, focusedKey_).valid()) {
        focusedKey_ = kNoKey;
    }
    if (pressedKey_ != kNoKey && !findByKey(root_, pressedKey_).valid()) {
        pressedKey_ = kNoKey;
    }
    if (capturedKey_ != kNoKey && !findByKey(root_, capturedKey_).valid()) {
        capturedKey_ = kNoKey;
    }

    const uint32_t overridesAfter = countOverrides(*root_);
    if (overridesBefore > 0 && overridesAfter > 0) {
        ++stats_.reconcilesWithLiveOverride;
    }

    ++stats_.rebuilds;
    stats_.elementsCreated   += rs.created;
    stats_.elementsDestroyed += rs.destroyed;
    stats_.elementsUpdated   += rs.updated;
    stats_.textShaped        += rs.textShaped;
    stats_.liveOverrides      = overridesAfter;
    return true;
}

void Ui::paint(rhi::Device& device) {
    rects_.clear();
    quads_.clear();
    runs_.clear();
    glyphQuadCount_ = 0;
    liveOverrides_  = 0;

    if (root_) {
        collectPaint(*root_, Rect{}, false);
    }

    // Upload only when the atlas actually gained glyphs — the steady state is
    // zero uploads per frame.
    if (atlasDirty_ && atlas_.valid()) {
        device.setGlyphAtlas(atlas_.pixels(), atlas_.size());
        atlasDirty_ = false;
        ++stats_.atlasUploads;
    }

    // One draw call per run, submitted in tree order, so a rect run and a glyph
    // run interleave exactly as the tree was walked.
    for (const DrawRun& run : runs_) {
        if (run.count == 0) continue;
        if (run.text) {
            device.drawGlyphQuads(quads_.data() + run.begin, run.count);
        } else {
            device.drawSdfRects(rects_.data() + run.begin, run.count);
        }
    }

    const auto& as = atlas_.stats();
    stats_.glyphsRasterized = as.rasterized;
    stats_.glyphCacheHits   = as.hits;
    stats_.glyphQuads       = glyphQuadCount_;
    // Observed per frame, not per rebuild: overrides come and go without the
    // tree ever being dirty, so update() would leave this stale.
    stats_.liveOverrides    = liveOverrides_;
    ++stats_.paints;
}

int64_t Ui::state(uint32_t slot, int64_t initial) const {
    if (slot >= kMaxStateSlots) return initial;
    return stateInit_[slot] ? stateValues_[slot] : initial;
}

void Ui::setState(uint32_t slot, int64_t value) {
    if (slot >= kMaxStateSlots) return;
    if (stateInit_[slot] && stateValues_[slot] == value) return;   // no-op write
    stateInit_[slot]   = true;
    stateValues_[slot] = value;
    dirty_             = true;   // the only thing that triggers a rebuild
}

NodeRef Ui::find(Key key) const {
    return findByKey(root_, key);
}

uint32_t Ui::liveOverrideCount() const {
    return root_ ? countOverrides(*root_) : 0;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

const NodeBehavior* Ui::behaviorOf(Key key) const {
    if (key == kNoKey) return nullptr;
    const auto it = behaviors_.find(key);
    return it == behaviors_.end() ? nullptr : &it->second;
}

void Ui::registerBehavior(Key key, NodeBehavior behavior) {
    if (key == kNoKey) {
        std::fprintf(stderr,
                     "[ui] registerBehavior: a node needs a key to be interactive\n");
        return;
    }
    behaviors_[key] = std::move(behavior);
}

// ---------------------------------------------------------------------------
// Text metrics
// ---------------------------------------------------------------------------

float Ui::measureText(const char* utf8, float sizePx) const {
    if (!textSystem_.valid() || !utf8 || !*utf8) return 0.0f;
    text::ShapedLine line;
    if (!textSystem_.shape(utf8, sizePx, line)) return 0.0f;
    return line.width;
}

float Ui::textAscent(float sizePx) const {
    return textSystem_.valid() ? textSystem_.ascent(sizePx) : 0.0f;
}

float Ui::textDescent(float sizePx) const {
    return textSystem_.valid() ? textSystem_.descent(sizePx) : 0.0f;
}

// Depth-first, children before their parent and later siblings first: that is
// paint order reversed, so the first entry is the topmost node. Everything
// after it is the chain an event can bubble through (components.md §2).
//
// A node with no render object (a clip container) has no bounds of its own, so
// it is bounded by the clip it introduces — which is exactly the region it
// should respond in.
void Ui::collectHitChain(const Element* e, float x, float y,
                         Rect clip, bool clipped,
                         std::vector<const Element*>& out) const {
    if (e->clipsChildren) {
        clip    = clipped ? intersectRect(clip, e->clipBounds) : e->clipBounds;
        clipped = true;
    }
    // Outside the clip nothing in this subtree can be hit — not the content,
    // and not the container itself (components.md §2, "必须尊重 clip").
    if (clipped && !clip.contains(x, y)) return;

    for (auto it = e->children.rbegin(); it != e->children.rend(); ++it) {
        collectHitChain(it->get(), x, y, clip, clipped, out);
    }

    const NodeBehavior* b = behaviorOf(e->key);
    if (!b || !b->interactive) return;
    // Base bounds, never the overridden ones: an override is paint-only and
    // must not move the region that responds to the pointer (§14.4).
    if (e->render && !e->render->bounds.contains(x, y)) return;
    out.push_back(e);
}

Key Ui::hitTest(float x, float y) const {
    chain_.clear();
    if (!root_) return kNoKey;
    collectHitChain(root_.get(), x, y, Rect{}, false, chain_);
    return chain_.empty() ? kNoKey : chain_.front()->key;
}

void Ui::hitChain(float x, float y, std::vector<Key>& out) const {
    out.clear();
    chain_.clear();
    if (!root_) return;
    collectHitChain(root_.get(), x, y, Rect{}, false, chain_);
    out.reserve(chain_.size());
    for (const Element* e : chain_) out.push_back(e->key);
}

Element* Ui::findElement(Key key) const {
    if (!root_ || key == kNoKey) return nullptr;
    std::vector<Element*> pending;
    pending.push_back(root_.get());
    while (!pending.empty()) {
        Element* e = pending.back();
        pending.pop_back();
        if (e->key == key) return e;
        for (auto& c : e->children) pending.push_back(c.get());
    }
    return nullptr;
}

void Ui::focusNext(bool backwards) {
    std::vector<Key> order;
    if (root_) {
        collectFocusable(*root_, behaviors_, order);
    }
    if (order.empty()) {
        focusedKey_ = kNoKey;
        return;
    }

    const auto it = std::find(order.begin(), order.end(), focusedKey_);
    if (it == order.end()) {
        focusedKey_ = backwards ? order.back() : order.front();
        return;
    }

    const auto count = static_cast<ptrdiff_t>(order.size());
    auto       index = std::distance(order.begin(), it);
    index = backwards ? (index - 1 + count) % count : (index + 1) % count;
    focusedKey_ = order[static_cast<size_t>(index)];
}

// A press walks the chain and stops at the first node that handles a press.
// That is what makes "a button inside a list row" behave: the button takes the
// press, the row never sees it, and the row is not also selected
// (components.md §2).
void Ui::mouseDown(float x, float y) {
    if (!root_) {
        pressedKey_ = kNoKey;
        return;
    }
    chain_.clear();
    collectHitChain(root_.get(), x, y, Rect{}, false, chain_);

    // Focus goes to the deepest focusable node, independent of who handles the
    // press — a label inside a focusable card should still focus the card.
    for (const Element* e : chain_) {
        const NodeBehavior* b = behaviorOf(e->key);
        if (b && b->focusable) {
            focusedKey_ = e->key;
            break;
        }
    }

    pressedKey_ = kNoKey;
    for (const Element* e : chain_) {
        const NodeBehavior* b = behaviorOf(e->key);
        if (!b) continue;
        // A drag widget takes the capture, and gets one immediate onDrag so
        // clicking a slider track jumps the thumb there.
        if (b->onDrag) {
            capturedKey_ = e->key;
            b->onDrag(x, y);
            break;
        }
        if (b->onClick) {
            pressedKey_ = e->key;
            break;
        }
    }
    dirty_ = true;
}

void Ui::mouseUp(float x, float y) {
    chain_.clear();
    if (root_) {
        collectHitChain(root_.get(), x, y, Rect{}, false, chain_);
    }

    if (capturedKey_ != kNoKey) {
        if (const NodeBehavior* b = behaviorOf(capturedKey_); b && b->onDragEnd) {
            b->onDragEnd();
        }
        capturedKey_ = kNoKey;
    }

    // A press that wandered off the node before release is not a click. Being
    // *anywhere* in the node's subtree still counts, which is why this checks
    // the chain rather than comparing keys for equality.
    if (pressedKey_ != kNoKey) {
        bool stillInside = false;
        for (const Element* e : chain_) {
            if (e->key == pressedKey_) {
                stillInside = true;
                break;
            }
        }
        if (stillInside) {
            if (const NodeBehavior* b = behaviorOf(pressedKey_); b && b->onClick) {
                b->onClick();
            }
        }
        pressedKey_ = kNoKey;
    }
    dirty_ = true;
}

// The wheel bubbles until something consumes it — the innermost node that has
// a use for it wins, and a plain row simply declines.
void Ui::mouseWheel(float x, float y, float lines) {
    if (!root_) return;
    chain_.clear();
    collectHitChain(root_.get(), x, y, Rect{}, false, chain_);
    for (const Element* e : chain_) {
        const NodeBehavior* b = behaviorOf(e->key);
        if (b && b->onWheel && b->onWheel(lines)) break;
    }
}

void Ui::keyDown(const InputEvent& ev) {
    if (ev.code == KeyCode::Tab) {
        focusNext(ev.shift);
        dirty_ = true;
        return;
    }
    // Key input bubbles from the focused node up, so an editor inside a
    // container still gets first refusal on what it does not consume.
    for (Element* e = findElement(focusedKey_); e; e = e->parent) {
        const NodeBehavior* b = behaviorOf(e->key);
        if (b && b->onKey) {
            b->onKey(ev);
            break;
        }
    }
}

void Ui::dispatchEvent(const InputEvent& ev) {
    switch (ev.type) {
    case InputEventType::MouseMove: {
        pointerX_      = ev.x;
        pointerY_      = ev.y;
        pointerInside_ = true;

        // A captured node keeps receiving moves even after the pointer has
        // left its box, which is the whole point of capture.
        if (capturedKey_ != kNoKey) {
            if (const NodeBehavior* b = behaviorOf(capturedKey_); b && b->onDrag) {
                b->onDrag(ev.x, ev.y);
            }
        }

        const Key hit = hitTest(ev.x, ev.y);
        if (hit != hoverKey_) {
            hoverKey_ = hit;
            dirty_    = true;   // hover drives base properties -> a rebuild
        }
        break;
    }

    case InputEventType::MouseLeave:
        pointerInside_ = false;
        if (hoverKey_ != kNoKey) {
            hoverKey_ = kNoKey;
            dirty_    = true;
        }
        if (pressedKey_ != kNoKey) {
            pressedKey_ = kNoKey;
            dirty_      = true;
        }
        // The drag capture deliberately survives leaving the client area: the
        // platform still delivers the release to us.
        break;

    case InputEventType::MouseDown:
        mouseDown(ev.x, ev.y);
        break;

    case InputEventType::MouseUp:
        mouseUp(ev.x, ev.y);
        break;

    case InputEventType::MouseWheel:
        mouseWheel(ev.x, ev.y, ev.wheel);
        break;

    case InputEventType::KeyDown:
        keyDown(ev);
        break;
    }
}

} // namespace tac::ui
