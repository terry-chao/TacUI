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

// Shapes `text` into a block of `wrapWidth` and resolves every glyph against
// the atlas, caching the result on the RenderObject.
//
// The cache key is (text, fontSize, rasterScale, wrapWidth): reconcile asks for
// the node's own width hint, and the layout pass then asks for the width the
// node actually landed in — which for a wrapped paragraph is a different
// answer. Keying on the width means the second ask is a hit, and a steady-state
// frame still does no text work.
void ensureText(RenderObject& ro, ReconcileCtx& ctx, ReconcileStats* stats,
                float wrapWidth) {
    if (!ctx.textSystem || !ctx.atlas || ro.text.empty()) {
        ro.block      = text::ShapedBlock{};
        ro.placed.clear();
        ro.blockReady = false;
        ro.wrapWidth  = wrapWidth;
        return;
    }
    if (ro.blockReady && ro.wrapWidth == wrapWidth) return;

    // Marked ready before shaping on purpose: a shaping failure means "this
    // node has no glyphs", not "try again next frame".
    ro.blockReady = true;
    ro.wrapWidth  = wrapWidth;

    // Layout shapes at the logical size (so advances match the logical layout),
    // but glyphs are rasterised at the *physical* size. Everything stored in
    // `placed` is therefore node-local physical pixels; paint adds the scaled
    // origin. That is what keeps text crisp on a scaled display instead of
    // magnifying a small bitmap.
    const float s      = ctx.scale > 0.0f ? ctx.scale : 1.0f;
    const float pxSize = ro.fontSize * s;
    ro.rasterScale     = s;

    ro.placed.clear();
    if (!ctx.textSystem->shapeBlock(ro.text.c_str(), ro.fontSize, wrapWidth,
                                    ro.block)) {
        ro.block = text::ShapedBlock{};
        return;
    }
    if (stats) ++stats->textShaped;

    // Glyph positions are stored *node-local*: the baseline is measured from
    // the block's own top, and paint adds the final origin. Layout can
    // therefore relocate a text node without re-shaping it (see PlacedGlyph).
    const float    baseline = ctx.textSystem->ascent(pxSize);
    const float    leading  = ctx.textSystem->lineHeight(pxSize);
    const uint16_t bucket   = static_cast<uint16_t>(pxSize + 0.5f);

    const uint32_t rasterizedBefore = ctx.atlas->stats().rasterized;

    size_t total = 0;
    for (const text::ShapedLine& line : ro.block.lines) total += line.glyphs.size();
    ro.placed.reserve(total);

    for (size_t li = 0; li < ro.block.lines.size(); ++li) {
        // Line origin: every line's baseline steps down by one line height.
        const float lineTop = leading * static_cast<float>(li);
        for (const text::Glyph& g : ro.block.lines[li].glyphs) {
            const text::GlyphSlot* slot =
                ctx.atlas->get(text::GlyphKey{ g.index, bucket, g.face },
                               *ctx.textSystem, pxSize);
            if (!slot) continue;   // atlas full; the glyph is dropped, not faked
            if (slot->width <= 0.0f || slot->height <= 0.0f) continue;   // blank glyph

            PlacedGlyph placed;
            placed.bounds = Rect{
                g.x * s + slot->bearingX,            // logical advance -> physical
                lineTop + baseline - slot->bearingY,  // bearingY goes up
                slot->width,
                slot->height,
            };
            placed.u0 = slot->u0;
            placed.v0 = slot->v0;
            placed.u1 = slot->u1;
            placed.v1 = slot->v1;
            ro.placed.push_back(placed);
        }
    }

    // Only flag the atlas for upload when something was actually rasterised.
    // Marking it unconditionally re-uploads the whole page on every rebuild,
    // even when every glyph was a cache hit.
    if (ctx.atlasDirty && ctx.atlas->stats().rasterized != rasterizedBefore) {
        *ctx.atlasDirty = true;
    }
}

// The wrap width to shape a text node with when nothing else is known: its own
// width hint if it has one, otherwise unconstrained (one line). The layout pass
// overrides this with the width the node actually got.
float textWrapHint(float widthHint) {
    return widthHint >= 0.0f ? widthHint : 0.0f;
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
        ensureText(*ro, ctx, &stats, textWrapHint(v.width));
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
// Two passes, per plan.md D6 — with the layout contract expressed as a value
// type (BoxConstraints) rather than a virtual MeasureOverride.
//
//   measure: what size does this node want, given what its parent can offer?
//   arrange: the parent has decided a slot; put the node in it.
//
// A relative container (row / column) is the only thing that redistributes
// space. A child that is itself absolute (a panel, a clip, a bare stack) is
// moved *as a whole* — its subtree is translated rather than re-authored, so
// controlling a widget's interior stays the caller's business. Absolute
// subtrees outside any relative container are never touched, so the M0 model is
// unchanged where it was used.
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

// What one container resolves to: its own size and one slot per child. This is
// the single source of truth for the distribution — measure asks for it under
// whatever the parent offered, arrange asks again under the box it was handed,
// and both get the same answer for the same box.
struct ContainerPlan {
    Size              size;
    std::vector<Size> children;   // content size, margins excluded
};

Size measureNode(Element& e, const BoxConstraints& c, ReconcileStats* stats,
                 ReconcileCtx& ctx);
void arrangeNode(Element& e, float x, float y, float w, float h,
                 ReconcileStats* stats, ReconcileCtx& ctx);
void planContainer(Element& e, const BoxConstraints& c, ContainerPlan& out,
                   ReconcileStats* stats, ReconcileCtx& ctx);

// The size a node wants under `c`.
Size measureNode(Element& e, const BoxConstraints& c, ReconcileStats* stats,
                 ReconcileCtx& ctx) {
    switch (e.type) {
    case VType::Text: {
        // The width it would wrap into: its own hint if it has one, otherwise
        // whatever the parent can offer. A bounded offer is what makes a long
        // label wrap instead of running out of its column.
        const float wrap = e.widthHint >= 0.0f
                               ? e.widthHint
                               : (c.hasBoundedWidth() ? c.maxWidth : 0.0f);
        Size s{ 0.0f, 0.0f };
        if (e.render) {
            ensureText(*e.render, ctx, stats, wrap);
            s = Size{ e.render->block.width, e.render->block.height() };
        }
        if (e.widthHint  >= 0.0f) s.w = e.widthHint;
        if (e.heightHint >= 0.0f) s.h = e.heightHint;
        return c.constrain(s);
    }

    case VType::Rect: {
        const Size want{
            e.widthHint  >= 0.0f ? e.widthHint
                                 : (e.render ? e.render->bounds.w : 0.0f),
            e.heightHint >= 0.0f ? e.heightHint
                                 : (e.render ? e.render->bounds.h : 0.0f),
        };
        return c.constrain(want);
    }

    default: {   // Stack
        if (isRelative(e)) {
            // A hint pins an axis; so does a positive authored frame. `kAuto`
            // (or a non-positive frame) means "size to content" — the axis the
            // parent constrains is then the one the content has to fit in.
            const float fixedW = e.widthHint  >= 0.0f ? e.widthHint
                                  : e.frameBox.w > 0.0f ? e.frameBox.w : kAuto;
            const float fixedH = e.heightHint >= 0.0f ? e.heightHint
                                  : e.frameBox.h > 0.0f ? e.frameBox.h : kAuto;

            BoxConstraints cc = c;
            if (fixedW >= 0.0f) {
                cc.minWidth = cc.maxWidth = c.constrainWidth(fixedW);
            }
            if (fixedH >= 0.0f) {
                cc.minHeight = cc.maxHeight = c.constrainHeight(fixedH);
            }

            ContainerPlan plan;
            planContainer(e, cc, plan, stats, ctx);
            return plan.size;
        }

        // An absolute subtree: its authored box if it has one, otherwise the
        // extent of what it paints. It is never re-authored, only moved.
        if (e.widthHint >= 0.0f && e.heightHint >= 0.0f) {
            return c.constrain(Size{ e.widthHint, e.heightHint });
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
        return c.constrain(Size{ w < 0.0f ? 0.0f : w, h < 0.0f ? 0.0f : h });
    }
    }
}

// Measures every child and works out what the container itself wants.
void planContainer(Element& e, const BoxConstraints& c, ContainerPlan& out,
                   ReconcileStats* stats, ReconcileCtx& ctx) {
    const bool       row = (e.layout == StackLayout::Row);
    const EdgeInsets pad = e.padding;
    const BoxConstraints inner = c.deflate(pad);
    const float crossMax = row ? inner.maxHeight : inner.maxWidth;

    const size_t n = e.children.size();
    out.children.assign(n, Size{});

    float fixedMain = 0.0f;
    float totalFlex = 0.0f;
    float crossWant = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        Element& ch = *e.children[i];

        // The main axis is offered unbounded so a non-flex child keeps its own
        // size; the cross axis is offered what the container has, which is the
        // constraint that makes a child's measurement depend on its parent.
        const BoxConstraints cc = row
            ? BoxConstraints{ 0.0f, kInfinity, 0.0f, crossMax }
            : BoxConstraints{ 0.0f, crossMax, 0.0f, kInfinity };

        const Size cs = measureNode(ch, cc, stats, ctx);
        out.children[i] = cs;

        const float slotMain = row ? cs.w + ch.margin.horizontal()
                                   : cs.h + ch.margin.vertical();
        const float slotCross = row ? cs.h + ch.margin.vertical()
                                    : cs.w + ch.margin.horizontal();
        if (ch.flex > 0.0f) totalFlex += ch.flex;
        else                fixedMain += slotMain;
        crossWant = std::max(crossWant, slotCross);
    }

    const float gaps = n > 1 ? e.gap * static_cast<float>(n - 1) : 0.0f;

    // Main axis: fill a bounded container (that is what lets `flex` distribute
    // something), otherwise take exactly what the children need. With no
    // bounded extent a flexible child has nothing to share, so it keeps its
    // intrinsic size.
    const bool  boundedMain = row ? inner.hasBoundedWidth() : inner.hasBoundedHeight();
    const float mainMax     = row ? inner.maxWidth : inner.maxHeight;
    const float contentMain =
        boundedMain ? std::max(mainMax, fixedMain + gaps) : fixedMain + gaps;

    float remaining = contentMain - fixedMain - gaps;
    if (remaining < 0.0f) remaining = 0.0f;
    if (totalFlex > 0.0f && remaining > 0.0f) {
        for (size_t i = 0; i < n; ++i) {
            Element& ch = *e.children[i];
            if (ch.flex <= 0.0f) continue;
            const float share = remaining * (ch.flex / totalFlex);
            if (row) out.children[i].w = std::max(0.0f, share - ch.margin.horizontal());
            else     out.children[i].h = std::max(0.0f, share - ch.margin.vertical());
        }
    }

    const Size want = row ? Size{ contentMain, crossWant }
                          : Size{ crossWant, contentMain };
    out.size = c.constrain(want);
}

void arrangeContainer(Element& e, const Rect& frame, ReconcileStats* stats,
                      ReconcileCtx& ctx) {
    // Re-solve against the box that was actually handed over. For an authored
    // frame this is the same answer the measure pass produced; for a container
    // whose size the parent decided (flex, cross-axis stretch) it is the one
    // that matters — children are placed in the space the node really has.
    ContainerPlan plan;
    planContainer(e, BoxConstraints::tight(frame.w, frame.h), plan, stats, ctx);

    e.frameBox = frame;

    const bool   row     = (e.layout == StackLayout::Row);
    const EdgeInsets pad = e.padding;
    const float contentX = frame.x + pad.left;
    const float contentY = frame.y + pad.top;
    const float contentW = std::max(0.0f, frame.w - pad.horizontal());
    const float contentH = std::max(0.0f, frame.h - pad.vertical());
    const float contentCross = row ? contentH : contentW;
    const float crossBase    = row ? contentY : contentX;

    const size_t n = e.children.size();
    float main = row ? contentX : contentY;
    for (size_t i = 0; i < n; ++i) {
        Element& c = *e.children[i];
        const float childW = plan.children[i].w;
        const float childH = plan.children[i].h;

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

        arrangeNode(c, cx, cy, childW, childH, stats, ctx);

        main += row ? childW + c.margin.horizontal() : childH + c.margin.vertical();
        if (i + 1 < n) main += e.gap;
    }
}

void arrangeNode(Element& e, float x, float y, float w, float h,
                 ReconcileStats* stats, ReconcileCtx& ctx) {
    if (isRelative(e)) {
        arrangeContainer(e, Rect{ x, y, w, h }, stats, ctx);
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
                const float s = ctx.scale > 0.0f ? ctx.scale : 1.0f;
                if (e.render->text != newText || e.render->fontSize != v.fontSize ||
                    e.render->rasterScale != s) {
                    e.render->text     = newText;
                    e.render->fontSize = v.fontSize;
                    // Re-shape only on change; this is the expensive part. The
                    // shaping cache is keyed on the wrap width as well, so the
                    // layout pass can ask for a different one and still hit it
                    // on the next rebuild.
                    e.render->blockReady = false;
                    ensureText(*e.render, ctx, &stats, textWrapHint(v.width));
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

namespace {

// Lays out a relative container that no relative parent placed: its frame is
// authored, and an axis authored as `kAuto` (or non-positive) sizes to its
// content instead. That single rule is what turns a row / column from "a box
// you must measure yourself" into "a box that fits what you put in it".
void arrangeAuthored(Element& e, ReconcileStats* stats, ReconcileCtx& ctx) {
    const float fixedW = e.widthHint  >= 0.0f ? e.widthHint  : e.frameBox.w;
    const float fixedH = e.heightHint >= 0.0f ? e.heightHint : e.frameBox.h;

    BoxConstraints c = BoxConstraints::unbounded();
    if (fixedW > 0.0f) c.minWidth = c.maxWidth = fixedW;
    if (fixedH > 0.0f) c.minHeight = c.maxHeight = fixedH;

    ContainerPlan plan;
    planContainer(e, c, plan, stats, ctx);

    const Rect frame{ e.frameBox.x, e.frameBox.y, plan.size.w, plan.size.h };
    arrangeContainer(e, frame, stats, ctx);
}

} // namespace

void layoutTree(Element& root, ReconcileCtx& ctx, ReconcileStats* stats) {
    // The root itself can be a relative container (the common case for a
    // full-window row/column): lay it out in its own authored frame.
    if (isRelative(root)) {
        arrangeAuthored(root, stats, ctx);
        return;
    }

    // Otherwise walk down. A relative container encountered here is one no
    // relative parent placed — so it keeps its authored box, and its children
    // are arranged inside it. Anything under it is handled by arrangeNode.
    for (auto& child : root.children) {
        if (isRelative(*child)) {
            arrangeAuthored(*child, stats, ctx);
        } else {
            layoutTree(*child, ctx, stats);
        }
    }
}

} // namespace tac::ui
