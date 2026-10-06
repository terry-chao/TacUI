#include "tacui/element.hpp"

#include <cmath>
#include <cstdio>

namespace tac::ui {

Rect effectiveBounds(const RenderObject& ro) {
    return Rect{
        ro.bounds.x + ro.overrideTranslate.x,
        ro.bounds.y + ro.overrideTranslate.y,
        ro.bounds.w,
        ro.bounds.h,
    };
}

Color effectiveColor(const RenderObject& ro) {
    if (!ro.overrideActive) return ro.baseColor;
    Color c = ro.baseColor;
    c.a *= ro.overrideOpacity;
    return c;
}

// ---------------------------------------------------------------------------
// OverrideToken
// ---------------------------------------------------------------------------

OverrideToken::~OverrideToken() {
    release();
}

OverrideToken::OverrideToken(OverrideToken&& other) noexcept
    : element_(std::move(other.element_)), generation_(other.generation_) {
    other.element_.reset();
    other.generation_ = 0;
}

OverrideToken& OverrideToken::operator=(OverrideToken&& other) noexcept {
    if (this != &other) {
        release();
        element_    = std::move(other.element_);
        generation_ = other.generation_;
        other.element_.reset();
        other.generation_ = 0;
    }
    return *this;
}

Element* OverrideToken::lockedTarget() const {
    auto e = element_.lock();
    if (!e || !e->render) return nullptr;
    if (e->generation != generation_) return nullptr;   // node was re-purposed
    return e.get();
}

void OverrideToken::setTranslate(float x, float y) {
    Element* e = lockedTarget();
    if (!e) return;
    e->render->overrideTranslate = Vec2{ x, y };
    e->render->overrideActive    = true;
}

void OverrideToken::setOpacity(float alpha) {
    Element* e = lockedTarget();
    if (!e) return;
    e->render->overrideOpacity = alpha;
    e->render->overrideActive  = true;
}

void OverrideToken::release() {
    Element* e = lockedTarget();
    if (e) {
        // Fall back to the base values. Note this does not touch `base*`.
        e->render->overrideActive    = false;
        e->render->overrideTranslate = Vec2{ 0.0f, 0.0f };
        e->render->overrideOpacity   = 1.0f;
    }
    element_.reset();
    generation_ = 0;
}

// ---------------------------------------------------------------------------
// NodeRef
// ---------------------------------------------------------------------------

bool NodeRef::valid() const {
    auto e = element_.lock();
    return e && e->render != nullptr;
}

OverrideToken NodeRef::beginOverride() {
    auto e = element_.lock();
    if (!e || !e->render) return OverrideToken{};

    if (e->render->overrideActive) {
        std::fprintf(stderr,
                     "[ui] NodeRef::beginOverride: node already has an active override; "
                     "only one level is supported (architecture.md §14.3)\n");
        return OverrideToken{};
    }

    e->render->overrideActive = true;
    return OverrideToken{ element_, e->generation };
}

Rect NodeRef::bounds() const {
    auto e = element_.lock();
    return (e && e->render) ? e->render->bounds : Rect{};
}

VType NodeRef::type() const {
    auto e = element_.lock();
    return e ? e->type : VType::Stack;
}

bool NodeRef::hasLiveOverride() const {
    auto e = element_.lock();
    return e && e->render && e->render->overrideActive;
}

float NodeRef::overrideOpacity() const {
    auto e = element_.lock();
    return (e && e->render) ? e->render->overrideOpacity : 1.0f;
}

Vec2 NodeRef::overrideTranslate() const {
    auto e = element_.lock();
    return (e && e->render) ? e->render->overrideTranslate : Vec2{};
}

// ---------------------------------------------------------------------------
// Reconcile
// ---------------------------------------------------------------------------

namespace {

// Shapes `text` and resolves every glyph against the atlas, caching the result
// on the RenderObject. Runs only when the string or its size actually changed.
void rebuildText(RenderObject& ro, ReconcileCtx& ctx, ReconcileStats& stats) {
    ro.shaped.glyphs.clear();
    ro.placed.clear();
    if (!ctx.textSystem || !ctx.atlas || ro.text.empty()) return;

    if (!ctx.textSystem->shape(ro.text.c_str(), ro.fontSize, ro.shaped)) {
        return;
    }
    ++stats.textShaped;

    const float baseline = ro.bounds.y + ctx.textSystem->ascent(ro.fontSize);
    const uint16_t bucket = static_cast<uint16_t>(ro.fontSize + 0.5f);

    const uint32_t rasterizedBefore = ctx.atlas->stats().rasterized;

    ro.placed.reserve(ro.shaped.glyphs.size());
    for (const text::Glyph& g : ro.shaped.glyphs) {
        const text::GlyphSlot* slot =
            ctx.atlas->get(text::GlyphKey{ g.index, bucket }, *ctx.textSystem, ro.fontSize);
        if (!slot) continue;   // atlas full; the glyph is dropped, not faked
        if (slot->width <= 0.0f || slot->height <= 0.0f) continue;   // blank glyph

        // Snap the glyph origin to whole pixels.
        //
        // The pen advance stays fractional — that is typographically correct
        // and keeps the spacing right — but drawing a bitmap glyph at a
        // fractional origin makes the linear sampler resample it, which is
        // exactly what turns UI text to mush. Snapping the origin while
        // leaving the advance alone keeps both properties.
        //
        // (Subpixel positioning is the other valid choice, but it needs either
        // a 3-channel atlas or distance-field text to look right. Not yet.)
        PlacedGlyph placed;
        placed.bounds = Rect{
            std::round(ro.bounds.x + g.x + slot->bearingX),
            std::round(baseline - slot->bearingY),   // bearingY is positive going up
            slot->width,
            slot->height,
        };
        placed.u0 = slot->u0;
        placed.v0 = slot->v0;
        placed.u1 = slot->u1;
        placed.v1 = slot->v1;
        ro.placed.push_back(placed);
    }

    // Only flag the atlas for upload when something was actually rasterised.
    // Marking it unconditionally re-uploads the whole page on every rebuild,
    // even when every glyph was a cache hit.
    if (ctx.atlasDirty && ctx.atlas->stats().rasterized != rasterizedBefore) {
        *ctx.atlasDirty = true;
    }
}

std::unique_ptr<RenderObject> makeRenderObject(const VNode& v, ReconcileCtx& ctx,
                                               ReconcileStats& stats) {
    auto ro = std::make_unique<RenderObject>();
    if (v.type == VType::Rect) {
        ro->bounds       = v.bounds;
        ro->cornerRadius = v.cornerRadius;
        ro->baseColor    = v.color;
    } else if (v.type == VType::Text) {
        ro->bounds    = v.bounds;
        ro->baseColor = v.color;
        ro->fontSize  = v.fontSize;
        if (v.text) ro->text = v.text;
        rebuildText(*ro, ctx, stats);
    }
    return ro;
}

// Mounts a brand new element subtree from `v`. The new element has no
// override state, by construction.
ElementPtr mount(const VNode& v, ReconcileStats& stats, ReconcileCtx& ctx) {
    auto e  = std::make_shared<Element>();
    e->type = v.type;
    e->key  = v.key;
    if (v.type == VType::Rect || v.type == VType::Text) {
        e->render = makeRenderObject(v, ctx, stats);
    }
    ++stats.created;

    e->children.reserve(v.childCount);
    for (uint32_t i = 0; i < v.childCount; ++i) {
        auto child = mount(*v.children[i], stats, ctx);
        child->parent = e.get();
        e->children.push_back(std::move(child));
    }
    return e;
}

// Tears down a subtree's retained state. Elements are shared_ptr-owned, so
// outstanding NodeRefs/OverrideTokens simply observe an expired weak_ptr.
void unmount(Element& e, ReconcileStats& stats) {
    for (auto& c : e.children) {
        unmount(*c, stats);
    }
    e.children.clear();
    e.render.reset();
    ++stats.destroyed;
}

void reconcileChildren(Element& e, const VNode& v, ReconcileStats& stats, ReconcileCtx& ctx) {
    const uint32_t existingCount = static_cast<uint32_t>(e.children.size());

    // Positional matching for M0. Shrinking drops the tail; growing mounts
    // the remainder. Key matching arrives in M2.
    for (uint32_t i = 0; i < v.childCount; ++i) {
        const VNode& cv = *v.children[i];
        if (i < existingCount) {
            reconcile(*e.children[i], cv, stats, ctx);
        } else {
            auto child = mount(cv, stats, ctx);
            child->parent = &e;
            e.children.push_back(std::move(child));
        }
    }

    while (e.children.size() > v.childCount) {
        unmount(*e.children.back(), stats);
        e.children.pop_back();
    }
}

} // namespace

void reconcile(Element& e, const VNode& v, ReconcileStats& stats, ReconcileCtx& ctx) {
    // Type or key change means this is a different node: replace wholesale.
    // Bumping the generation invalidates any outstanding OverrideToken, and
    // clearing the state slots mirrors "a different widget, so fresh state".
    if (e.type != v.type || e.key != v.key) {
        auto fresh  = mount(v, stats, ctx);
        e.type      = fresh->type;
        e.key       = fresh->key;
        e.render    = std::move(fresh->render);
        e.children  = std::move(fresh->children);
        e.states.clear();
        ++e.generation;
        for (auto& c : e.children) c->parent = &e;
        return;
    }

    // Same node: write base values only. The override channel is deliberately
    // left untouched, so an in-flight animation survives the rebuild
    // (plan.md §6.1 criterion 4).
    if (v.type == VType::Rect || v.type == VType::Text) {
        if (!e.render) {
            e.render = makeRenderObject(v, ctx, stats);
        } else {
            e.render->bounds    = v.bounds;
            e.render->baseColor = v.color;

            if (v.type == VType::Rect) {
                e.render->cornerRadius = v.cornerRadius;
            } else {
                const char* newText = v.text ? v.text : "";
                if (e.render->text != newText || e.render->fontSize != v.fontSize) {
                    e.render->text     = newText;
                    e.render->fontSize = v.fontSize;
                    // Re-shape only on change; this is the expensive part.
                    rebuildText(*e.render, ctx, stats);
                }
            }
        }
    }
    ++stats.updated;

    reconcileChildren(e, v, stats, ctx);
}

NodeRef findByKey(const ElementPtr& root, Key key) {
    if (!root || key == kNoKey) return NodeRef{};
    if (root->key == key) return NodeRef{ root };

    for (const auto& c : root->children) {
        NodeRef found = findByKey(c, key);
        if (found.valid()) return found;
    }
    return NodeRef{};
}

} // namespace tac::ui
