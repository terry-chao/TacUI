#include "tacui/glyph_atlas.hpp"

#include <cstring>

namespace tac::text {

bool GlyphAtlas::init(uint32_t size) {
    if (size == 0) return false;

    size_ = size;
    pixels_.assign(static_cast<size_t>(size_) * size_, 0);

    shelfY_      = 0;
    shelfHeight_ = 0;
    penX_        = 0;

    cache_.clear();
    stats_ = Stats{};
    dirtyTop_    = 0;
    dirtyBottom_ = 0;
    return true;
}

void GlyphAtlas::shutdown() {
    pixels_.clear();
    cache_.clear();
    size_ = 0;
}

void GlyphAtlas::markDirty(uint32_t top, uint32_t bottom) {
    if (dirtyBottom_ == dirtyTop_) {
        dirtyTop_    = top;
        dirtyBottom_ = bottom;
        return;
    }
    if (top < dirtyTop_)       dirtyTop_    = top;
    if (bottom > dirtyBottom_) dirtyBottom_ = bottom;
}

bool GlyphAtlas::consumeDirty(uint32_t& top, uint32_t& bottom) {
    if (dirtyBottom_ <= dirtyTop_) return false;
    top          = dirtyTop_;
    bottom       = dirtyBottom_;
    dirtyTop_    = 0;
    dirtyBottom_ = 0;
    return true;
}

bool GlyphAtlas::place(uint32_t w, uint32_t h, uint32_t& outX, uint32_t& outY) {
    const uint32_t paddedW = w + kPadding * 2;
    const uint32_t paddedH = h + kPadding * 2;

    if (paddedW > size_) return false;

    // Wrap to a new shelf when this one runs out of horizontal room.
    if (penX_ + paddedW > size_) {
        shelfY_     += shelfHeight_;
        shelfHeight_ = 0;
        penX_        = 0;
    }

    if (shelfY_ + paddedH > size_) return false;   // atlas full

    outX = penX_ + kPadding;
    outY = shelfY_ + kPadding;

    penX_ += paddedW;
    if (paddedH > shelfHeight_) shelfHeight_ = paddedH;
    return true;
}

const GlyphSlot* GlyphAtlas::get(const GlyphKey& key, const TextSystem& text, float sizePx) {
    const auto it = cache_.find(key);
    if (it != cache_.end()) {
        ++stats_.hits;
        return &it->second;
    }
    ++stats_.misses;

    GlyphBitmap bmp;
    if (!text.rasterize(key.glyphIndex, sizePx, bmp)) {
        return nullptr;
    }

    GlyphSlot slot;
    slot.width    = static_cast<float>(bmp.width);
    slot.height   = static_cast<float>(bmp.height);
    slot.bearingX = static_cast<float>(bmp.bearingX);
    slot.bearingY = static_cast<float>(bmp.bearingY);

    if (bmp.width == 0 || bmp.height == 0) {
        // Blank glyph (space, tab): cache the metrics, contribute no pixels.
        slot.u0 = slot.v0 = slot.u1 = slot.v1 = 0.0f;
        auto inserted = cache_.emplace(key, slot);
        return &inserted.first->second;
    }

    uint32_t x = 0;
    uint32_t y = 0;
    if (!place(bmp.width, bmp.height, x, y)) {
        ++stats_.rejected;
        return nullptr;
    }

    for (uint32_t row = 0; row < bmp.height; ++row) {
        std::memcpy(pixels_.data() + static_cast<size_t>(y + row) * size_ + x,
                    bmp.alpha.data() + static_cast<size_t>(row) * bmp.width,
                    bmp.width);
    }
    markDirty(y, y + bmp.height);

    // Edge-to-edge UVs, NOT texel-centre insets.
    //
    // The quad spans exactly `bmp.width` x `bmp.height` pixels (glyph.hlsl:
    // centre +/- halfSize, and paint snaps the origin to whole pixels), so a
    // pixel centre lands on a texel centre when the UV range covers the glyph's
    // full texel extent — a 1:1 blit with no resampling. Insetting by half a
    // texel would squeeze w texels into w pixels and blend every pixel with its
    // neighbour, which softens every glyph. The padding gutter is what keeps
    // the linear filter from reaching a neighbour anyway.
    const float inv = 1.0f / static_cast<float>(size_);
    slot.u0 = static_cast<float>(x) * inv;
    slot.v0 = static_cast<float>(y) * inv;
    slot.u1 = static_cast<float>(x + bmp.width) * inv;
    slot.v1 = static_cast<float>(y + bmp.height) * inv;

    ++stats_.rasterized;
    auto inserted = cache_.emplace(key, slot);
    return &inserted.first->second;
}

} // namespace tac::text
