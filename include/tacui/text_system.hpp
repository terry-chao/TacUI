#pragma once

#include <cstdint>
#include <vector>

struct IDWriteFactory;
struct IDWriteFontFace;

namespace tac::text {

// One positioned glyph within a shaped line.
struct Glyph {
    uint16_t index   = 0;
    float    x       = 0.0f;   // pen position along the line
    float    advance = 0.0f;
};

struct ShapedLine {
    std::vector<Glyph> glyphs;
    float              width = 0.0f;
};

// 8-bit coverage bitmap for a single glyph, already cropped to its ink bounds.
struct GlyphBitmap {
    std::vector<uint8_t> alpha;      // width * height, one byte per pixel
    uint32_t             width     = 0;
    uint32_t             height    = 0;
    int32_t              bearingX  = 0;   // bitmap left edge relative to the pen
    int32_t              bearingY  = 0;   // bitmap top edge above the baseline
};

// Text shaping and glyph rasterisation.
//
// M0 uses DirectWrite for the whole stack so the renderer-side architecture
// (atlas + glyph cache + batch) is what gets validated. The plan's target is
// HarfBuzz for shaping and ICU for bidi/line-breaking (architecture.md §3.4);
// those are swappable behind this interface and land in M1.
//
// M0 limitations, deliberate:
//   * one font face, no fallback chain
//   * codepoint -> glyph is 1:1, so no ligatures or complex-script shaping
//   * single line: no wrapping, no bidi
class TextSystem {
public:
    // `family` is UTF-8, consistent with the rest of the public API.
    bool init(const char* family = "Segoe UI");
    void shutdown();
    bool valid() const { return face_ != nullptr; }

    // Shapes one line of UTF-8. Advances are in pixels at `sizePx`.
    bool shape(const char* utf8, float sizePx, ShapedLine& out) const;

    // Rasterises one glyph at `sizePx` into an 8-bit coverage bitmap,
    // cropped to the ink bounds and positioned relative to the baseline.
    bool rasterize(uint16_t glyphIndex, float sizePx, GlyphBitmap& out) const;

    float ascent(float sizePx) const;
    float descent(float sizePx) const;
    float lineHeight(float sizePx) const;

    // Bumped whenever the face changes, so caches keyed on it can invalidate.
    uint32_t faceId() const { return faceId_; }

private:
    IDWriteFactory*  factory_ = nullptr;
    IDWriteFontFace* face_    = nullptr;
    uint32_t         faceId_  = 0;
    bool             comOwned_ = false;   // whether we own the thread's COM init

    // Font design metrics, in design units.
    float designUnitsPerEm_ = 2048.0f;
    float ascentUnits_      = 0.0f;
    float descentUnits_     = 0.0f;
    float lineGapUnits_     = 0.0f;
};

} // namespace tac::text
