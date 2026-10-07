#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "tacui/text_system.hpp"

namespace tac::text {

// Cache key. Size is bucketed to whole pixels: keeping a distinct entry per
// fractional size would explode the cache for no visible gain.
struct GlyphKey {
    uint16_t glyphIndex = 0;
    uint16_t sizeBucket = 0;

    bool operator==(const GlyphKey& o) const {
        return glyphIndex == o.glyphIndex && sizeBucket == o.sizeBucket;
    }
};

struct GlyphKeyHash {
    size_t operator()(const GlyphKey& k) const {
        return (static_cast<size_t>(k.glyphIndex) << 16) ^ k.sizeBucket;
    }
};

// Where a glyph lives, plus everything needed to position it.
struct GlyphSlot {
    // UV rect in the atlas, edge-to-edge over the glyph's texels. The quad is
    // exactly the glyph's pixel size, so this samples texel centres 1:1 with no
    // resampling — inset UVs would squeeze the glyph and soften it.
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;

    float width    = 0.0f;   // pixels
    float height   = 0.0f;
    float bearingX = 0.0f;   // bitmap left edge relative to the pen
    float bearingY = 0.0f;   // bitmap top edge above the baseline
    // Advances belong to the shaped run, not to the atlas, which is keyed only
    // on (glyph, size) and knows nothing about the string.
};

// A single R8 coverage atlas, shelf-packed.
//
// Glyphs are rasterised once and reused forever after — that reuse is the
// retention this layer exists to provide, and the cache stats are what make it
// observable (plan.md §6.1 criterion 1).
//
// M0 has no eviction: when the atlas fills, `get` returns null and count it in
// the stats. M1 adds LRU eviction and multiple pages.
class GlyphAtlas {
public:
    static constexpr uint32_t kPadding = 1;   // gutter against filter bleed

    struct Stats {
        uint32_t rasterized = 0;   // glyphs actually rasterised
        uint32_t hits       = 0;   // lookups served from the cache
        uint32_t misses     = 0;
        uint32_t rejected   = 0;   // atlas full
    };

    bool init(uint32_t size = 1024);
    void shutdown();

    // Rasterises on first use. Returns null only when the atlas is full.
    const GlyphSlot* get(const GlyphKey& key, const TextSystem& text, float sizePx);

    const uint8_t* pixels() const { return pixels_.data(); }
    uint32_t       size() const { return size_; }
    bool           valid() const { return size_ > 0; }

    // Reports the rows changed since the last call, then clears the range.
    // Rows are contiguous because the packer fills top-down.
    bool consumeDirty(uint32_t& top, uint32_t& bottom);

    const Stats& stats() const { return stats_; }

private:
    bool place(uint32_t w, uint32_t h, uint32_t& outX, uint32_t& outY);
    void markDirty(uint32_t top, uint32_t bottom);

    uint32_t size_ = 0;
    std::vector<uint8_t> pixels_;

    // Shelf packer state.
    uint32_t shelfY_      = 0;
    uint32_t shelfHeight_ = 0;
    uint32_t penX_        = 0;

    std::unordered_map<GlyphKey, GlyphSlot, GlyphKeyHash> cache_;

    uint32_t dirtyTop_    = 0;
    uint32_t dirtyBottom_ = 0;

    Stats stats_;
};

} // namespace tac::text
