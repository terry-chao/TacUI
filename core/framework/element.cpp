#include "tacui/element.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

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

// Mirrors the layout half of a VNode onto the retained Element. The VNode is
// thrown away after reconcile, so anything layout needs has to be copied here.
void copyLayout(Element& e, const VNode& v) {
    e.layout     = v.layout;
    e.gap        = v.gap;
    e.padding    = v.padding;
    e.frameBox   = v.bounds;
    e.widthHint  = v.width;
    e.heightHint = v.height;
    e.flex       = v.flex;
    e.margin     = v.margin;
    e.alignX     = v.alignX;
    e.alignY     = v.alignY;
}

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

    // Glyph positions are stored *node-local*: the baseline is measured from
    // the node's own top, and paint adds the final origin. Layout can therefore
    // relocate a text node without re-shaping it (see PlacedGlyph).
    const float baseline = ctx.textSystem->ascent(ro.fontSize);
    const uint16_t bucket = static_cast<uint16_t>(ro.fontSize + 0.5f);

    const uint32_t rasterizedBefore = ctx.atlas->stats().rasterized;

    ro.placed.reserve(ro.shaped.glyphs.size());
    for (const text::Glyph& g : ro.shaped.glyphs) {
        const text::GlyphSlot* slot =
            ctx.atlas->get(text::GlyphKey{ g.index, bucket }, *ctx.textSystem, ro.fontSize);
        if (!slot) continue;   // atlas full; the glyph is dropped, not faked
        if (slot->width <= 0.0f || slot->height <= 0.0f) continue;   // blank glyph

        PlacedGlyph placed;
        placed.bounds = Rect{
            g.x + slot->bearingX,
            baseline - slot->bearingY,   // bearingY is positive going up
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
    e->clipsChildren = v.clipChildren;
    e->clipBounds    = v.clip;
    copyLayout(*e, v);
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

// ---------------------------------------------------------------------------
// Layout
//
// Frame-based, per plan.md D6. A relative container (row / column) is handed an
// explicit box and splits it among its children; a child that is itself
// absolute (a panel, a clip, a bare stack) is moved *as a whole* — its subtree
// is translated rather than re-authored. Absolute subtrees outside any relative
// container are never touched, so the M0 model is unchanged where it was used.
// ---------------------------------------------------------------------------

bool isRelative(const Element& e) {
    return e.type == VType::Stack &&
           (e.layout == StackLayout::Row || e.layout == StackLayout::Column);
}

float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

// The union of a subtree's painted rectangles, in the subtree's current
// (authored) coordinates. Used to size and place a group that does not carry an
// explicit box of its own — a bare `stack()`.
bool subtreeBounds(const Element& e, Rect& out) {
    bool any = false;
    auto add = [&any, &out](const Rect& r) {
        if (!any) {
            out = r;
            any = true;
            return;
        }
        const float x0 = std::min(out.x, r.x);
        const float y0 = std::min(out.y, r.y);
        const float x1 = std::max(out.right(), r.right());
        const float y1 = std::max(out.bottom(), r.bottom());
        out = Rect{ x0, y0, x1 - x0, y1 - y0 };
    };

    if (e.render) add(e.render->bounds);
    if (e.clipsChildren) add(e.clipBounds);
    for (const auto& c : e.children) {
        Rect cb;
        if (subtreeBounds(*c, cb)) add(cb);
    }
    return any;
}

// Moves an absolute subtree to a new place by shifting every rectangle in it —
// paint bounds, clip bounds and the frame — by the same delta. Because glyphs
// are stored node-local (see PlacedGlyph), text follows for free.
void translateSubtree(Element& e, float dx, float dy) {
    if (dx == 0.0f && dy == 0.0f) return;
    if (e.render) {
        e.render->bounds.x += dx;
        e.render->bounds.y += dy;
    }
    if (e.clipsChildren) {
        e.clipBounds.x += dx;
        e.clipBounds.y += dy;
    }
    e.frameBox.x += dx;
    e.frameBox.y += dy;
    for (auto& c : e.children) translateSubtree(*c, dx, dy);
}

// The size a node wants when it is a child of a relative container: an explicit
// hint wins, otherwise its intrinsic size.
Size intrinsicSize(const Element& e, ReconcileCtx& ctx) {
    switch (e.type) {
    case VType::Text: {
        float w = e.widthHint;
        float h = e.heightHint;
        if (w < 0.0f || h < 0.0f) {
            float tw = 0.0f;
            float th = 0.0f;
            if (e.render && ctx.textSystem) {
                text::ShapedLine line;
                if (ctx.textSystem->shape(e.render->text.c_str(),
                                          e.render->fontSize, line)) {
                    tw = line.width;
                }
                th = ctx.textSystem->ascent(e.render->fontSize) +
                     ctx.textSystem->descent(e.render->fontSize);
            }
            if (w < 0.0f) w = tw;
            if (h < 0.0f) h = th;
        }
        return Size{ w, h };
    }

    case VType::Rect: {
        const float w = e.widthHint  >= 0.0f ? e.widthHint
                                             : (e.render ? e.render->bounds.w : 0.0f);
        const float h = e.heightHint >= 0.0f ? e.heightHint
                                             : (e.render ? e.render->bounds.h : 0.0f);
        return Size{ w, h };
    }

    default: {   // Stack
        if (isRelative(e)) {
            const float w = e.widthHint  >= 0.0f ? e.widthHint  : e.frameBox.w;
            const float h = e.heightHint >= 0.0f ? e.heightHint : e.frameBox.h;
            return Size{ w, h };
        }
        float w = e.widthHint;
        float h = e.heightHint;
        if (w < 0.0f || h < 0.0f) {
            Rect bb;
            if (subtreeBounds(e, bb)) {
                if (w < 0.0f) w = bb.w;
                if (h < 0.0f) h = bb.h;
            }
        }
        return Size{ w < 0.0f ? 0.0f : w, h < 0.0f ? 0.0f : h };
    }
    }
}

void arrangeNode(Element& e, float x, float y, float w, float h, ReconcileCtx& ctx);

void arrangeContainer(Element& e, const Rect& frame, ReconcileCtx& ctx) {
    e.frameBox = frame;

    const bool   row    = (e.layout == StackLayout::Row);
    const EdgeInsets pad = e.padding;
    const float contentX = frame.x + pad.left;
    const float contentY = frame.y + pad.top;
    const float contentW = std::max(0.0f, frame.w - pad.horizontal());
    const float contentH = std::max(0.0f, frame.h - pad.vertical());
    const float contentCross = row ? contentH : contentW;
    const float crossBase    = row ? contentY : contentX;

    const size_t n = e.children.size();
    std::vector<Size> sizes(n);

    float fixedMain = 0.0f;
    float totalFlex = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        Element& c = *e.children[i];
        sizes[i]   = intrinsicSize(c, ctx);
        const float mainSlot =
            row ? sizes[i].w + c.margin.horizontal()
                : sizes[i].h + c.margin.vertical();
        if (c.flex > 0.0f) totalFlex += c.flex;
        else               fixedMain += mainSlot;
    }

    const float contentMain = row ? contentW : contentH;
    const float gaps        = n > 1 ? e.gap * static_cast<float>(n - 1) : 0.0f;
    float remaining = contentMain - fixedMain - gaps;
    if (remaining < 0.0f) remaining = 0.0f;

    float main = row ? contentX : contentY;
    for (size_t i = 0; i < n; ++i) {
        Element& c = *e.children[i];

        float childW = sizes[i].w;
        float childH = sizes[i].h;
        if (totalFlex > 0.0f && c.flex > 0.0f) {
            const float share = remaining * (c.flex / totalFlex);
            if (row) childW = std::max(0.0f, share - c.margin.horizontal());
            else     childH = std::max(0.0f, share - c.margin.vertical());
        }

        float cx;
        float cy;
        if (row) {
            cx = main + c.margin.left;
            const float slack = contentCross - c.margin.vertical() - childH;
            cy = crossBase + c.margin.top + (slack > 0.0f ? slack * clamp01(c.alignY) : 0.0f);
        } else {
            cy = main + c.margin.top;
            const float slack = contentCross - c.margin.horizontal() - childW;
            cx = crossBase + c.margin.left + (slack > 0.0f ? slack * clamp01(c.alignX) : 0.0f);
        }

        arrangeNode(c, cx, cy, childW, childH, ctx);

        main += row ? childW + c.margin.horizontal() : childH + c.margin.vertical();
        if (i + 1 < n) main += e.gap;
    }
}

void arrangeNode(Element& e, float x, float y, float w, float h, ReconcileCtx& ctx) {
    if (isRelative(e)) {
        arrangeContainer(e, Rect{ x, y, w, h }, ctx);
        return;
    }

    if (e.type == VType::Stack) {
        // An absolute group placed by a relative parent: move it as a whole so
        // its current top-left lands on the slot. Prefer its explicit box; fall
        // back to the bounding box of the subtree for a bare `stack()`.
        float ox = e.frameBox.x;
        float oy = e.frameBox.y;
        if (e.frameBox.w <= 0.0f && e.frameBox.h <= 0.0f) {
            Rect bb;
            if (subtreeBounds(e, bb)) {
                ox = bb.x;
                oy = bb.y;
            }
        }
        translateSubtree(e, x - ox, y - oy);
        return;
    }

    if (e.render) {
        e.render->bounds = Rect{ x, y, w, h };
    }
    e.frameBox = Rect{ x, y, w, h };
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
    e.clipsChildren = v.clipChildren;
    e.clipBounds    = v.clip;
    copyLayout(e, v);
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

void layoutTree(Element& root, ReconcileCtx& ctx) {
    // The root itself can be a relative container (the common case for a
    // full-window row/column): lay it out in its own authored frame.
    if (isRelative(root)) {
        arrangeContainer(root, root.frameBox, ctx);
        return;
    }

    // Otherwise walk down. A relative container encountered here is one no
    // relative parent placed — so it keeps its authored box, and its children
    // are arranged inside it. Anything under it is handled by arrangeNode.
    for (auto& child : root.children) {
        if (isRelative(*child)) {
            arrangeContainer(*child, child->frameBox, ctx);
        } else {
            layoutTree(*child, ctx);
        }
    }
}

} // namespace tac::ui
