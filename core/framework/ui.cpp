#include "tacui/ui.hpp"

#include <algorithm>
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

Color applyOpacity(Color c, const RenderObject& ro) {
    if (!ro.overrideActive) return c;
    c.a *= ro.overrideOpacity;
    return c;
}

// Paint order is tree order, i.e. the painter's algorithm — within a node type.
//
// M0 limitation: rectangles and glyphs go to two separate batches, so all
// rects are drawn before all text regardless of tree order. That is fine when
// text sits on top of its background (the usual case) but wrong in general.
// M1 replaces this with a single ordered draw list.
void collect(const Element& e,
             std::vector<rhi::SdfRect>& rects,
             std::vector<rhi::GlyphQuad>& quads,
             uint64_t& glyphQuadCount,
             uint32_t& liveOverrides) {
    if (e.render) {
        const RenderObject& ro = *e.render;
        if (ro.overrideActive) ++liveOverrides;

        if (e.type == VType::Rect) {
            rhi::SdfRect r{};
            r.bounds       = effectiveBounds(ro);
            r.cornerRadius = ro.cornerRadius;
            r.color        = effectiveColor(ro);
            rects.push_back(r);
        } else if (e.type == VType::Text) {
            const Vec2 t     = ro.overrideActive ? ro.overrideTranslate : Vec2{};
            const Color tint = applyOpacity(ro.baseColor, ro);

            for (const PlacedGlyph& g : ro.placed) {
                rhi::GlyphQuad q{};
                q.bounds = Rect{ g.bounds.x + t.x, g.bounds.y + t.y, g.bounds.w, g.bounds.h };
                q.u0 = g.u0;
                q.v0 = g.v0;
                q.u1 = g.u1;
                q.v1 = g.v1;
                q.color = tint;
                quads.push_back(q);
                ++glyphQuadCount;
            }
        }
    }

    for (const auto& c : e.children) {
        collect(*c, rects, quads, glyphQuadCount, liveOverrides);
    }
}

// Depth-first, children before their parent and later siblings first: that is
// paint order reversed, so the first hit is the topmost node.
const Element* hitTestRecursive(const Element* e, float x, float y,
                                const std::unordered_map<Key, NodeBehavior>& behaviors) {
    for (auto it = e->children.rbegin(); it != e->children.rend(); ++it) {
        if (const Element* hit = hitTestRecursive(it->get(), x, y, behaviors)) {
            return hit;
        }
    }
    if (!e->render) return nullptr;
    // Base bounds, never the overridden ones: an override is paint-only and
    // must not move the region that responds to the pointer (§14.4).
    if (!e->render->bounds.contains(x, y)) return nullptr;

    const auto b = behaviors.find(e->key);
    if (b == behaviors.end() || !b->second.interactive) return nullptr;
    return e;
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

    // Focus survives rebuilds while the node does; clear it only if the node
    // was removed.
    if (focusedKey_ != kNoKey && !findByKey(root_, focusedKey_).valid()) {
        focusedKey_ = kNoKey;
    }
    if (pressedKey_ != kNoKey && !findByKey(root_, pressedKey_).valid()) {
        pressedKey_ = kNoKey;
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

    uint64_t quadCount     = 0;
    uint32_t liveOverrides = 0;
    if (root_) {
        collect(*root_, rects_, quads_, quadCount, liveOverrides);
    }

    // Upload only when the atlas actually gained glyphs — the steady state is
    // zero uploads per frame.
    if (atlasDirty_ && atlas_.valid()) {
        device.setGlyphAtlas(atlas_.pixels(), atlas_.size());
        atlasDirty_ = false;
        ++stats_.atlasUploads;
    }

    device.drawSdfRects(rects_.data(), static_cast<uint32_t>(rects_.size()));
    device.drawGlyphQuads(quads_.data(), static_cast<uint32_t>(quads_.size()));

    const auto& as = atlas_.stats();
    stats_.glyphsRasterized = as.rasterized;
    stats_.glyphCacheHits   = as.hits;
    stats_.glyphQuads       = quadCount;
    // Observed per frame, not per rebuild: overrides come and go without the
    // tree ever being dirty, so update() would leave this stale.
    stats_.liveOverrides    = liveOverrides;
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

Key Ui::hitTest(float x, float y) const {
    if (!root_) return kNoKey;
    const Element* hit = hitTestRecursive(root_.get(), x, y, behaviors_);
    return hit ? hit->key : kNoKey;
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

void Ui::dispatchEvent(const InputEvent& ev) {
    switch (ev.type) {
    case InputEventType::MouseMove: {
        pointerX_      = ev.x;
        pointerY_      = ev.y;
        pointerInside_ = true;

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
        break;

    case InputEventType::MouseDown: {
        const Key hit = hitTest(ev.x, ev.y);
        if (hit == kNoKey) break;

        pressedKey_ = hit;
        // Clicking a focusable node moves focus to it, the way every desktop
        // toolkit behaves.
        if (const NodeBehavior* b = behaviorOf(hit); b && b->focusable) {
            focusedKey_ = hit;
        }
        dirty_ = true;
        break;
    }

    case InputEventType::MouseUp: {
        const Key hit = hitTest(ev.x, ev.y);
        const Key was = pressedKey_;
        pressedKey_ = kNoKey;
        dirty_      = true;

        // A press that wandered off the node before release is not a click.
        // Tracking this here is why components do not each reimplement it.
        if (was != kNoKey && hit == was) {
            if (const NodeBehavior* b = behaviorOf(was); b && b->onClick) {
                b->onClick();
            }
        }
        break;
    }

    case InputEventType::KeyDown: {
        if (ev.code == KeyCode::Tab) {
            focusNext(ev.shift);
            dirty_ = true;
            break;
        }
        if (const NodeBehavior* b = behaviorOf(focusedKey_); b && b->onKey) {
            b->onKey(ev);
        }
        break;
    }
    }
}

} // namespace tac::ui
