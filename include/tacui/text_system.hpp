#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct IDWriteFactory;
struct IDWriteFontFace;
struct IDWriteFont;
struct IDWriteFontCollection;
struct IDWriteFontFallback;

namespace tac::text {

// One positioned glyph within a shaped line.
struct Glyph {
    uint16_t index   = 0;
    uint16_t face    = 0;      // which face in the fallback chain produced it
    float    x       = 0.0f;   // pen position along the line
    float    advance = 0.0f;
};

struct ShapedLine {
    std::vector<Glyph> glyphs;
    float              width = 0.0f;
};

// A shaped *block*: one line when the text was given no width to wrap into,
// several when it was. Glyph `x` is relative to the line's own origin, so a
// line can be drawn without knowing about the ones before it — which is what
// lets the layout pass place a wrapped paragraph as one node.
//
// This is where "自动换行" lives: everything above it (intrinsic size, node
// bounds, hit testing) sees a plain `width` x `height` box.
struct ShapedBlock {
    std::vector<ShapedLine> lines;
    float width      = 0.0f;   // widest line, logical px
    float ascent     = 0.0f;   // first baseline, measured from the block top
    float descent    = 0.0f;   // below the last baseline
    float lineHeight = 0.0f;   // baseline-to-baseline distance

    // Block height: the last baseline plus its descent. `lines.size() - 1`
    // line steps sit between the first baseline and the last.
    float height() const {
        if (lines.empty()) return 0.0f;
        return ascent + descent +
               lineHeight * static_cast<float>(lines.size() - 1);
    }
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
// Fallback: a character the primary family does not cover (CJK, symbols, …) is
// resolved through DirectWrite's system font fallback, so each glyph carries
// the face it came from. Without that, Chinese came out as tofu boxes.
//
// Still deliberate limitations:
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

    // Shapes UTF-8 into a block, wrapping at `maxWidth` logical pixels.
    // `maxWidth <= 0` means "no wrapping" and yields exactly one line.
    //
    // The greedy breaker prefers the last space (or CJK boundary) that fits and
    // only splits inside a word when a single word is wider than the line, so
    // Latin text wraps between words and CJK text wraps between characters
    // without needing a dictionary. `\n` always breaks.
    bool shapeBlock(const char* utf8, float sizePx, float maxWidth, ShapedBlock& out) const;

    // Rasterises one glyph at `sizePx` into an 8-bit coverage bitmap,
    // cropped to the ink bounds and positioned relative to the baseline.
    // `face` is the id carried by the Glyph (0 = primary).
    bool rasterize(uint16_t face, uint16_t glyphIndex, float sizePx,
                   GlyphBitmap& out) const;

    float ascent(float sizePx) const;
    float descent(float sizePx) const;
    float lineHeight(float sizePx) const;

    // Bumped whenever the face changes, so caches keyed on it can invalidate.
    uint32_t faceId() const { return faceId_; }

private:
    // One resolved face in the fallback chain. Raw pointers: the public header
    // must stay free of dwrite.h, so lifetime is managed by hand in shutdown().
    struct FaceEntry {
        IDWriteFontFace* face            = nullptr;
        IDWriteFont*     font            = nullptr;   // owner, for identity/has-char
        float            designUnitsPerEm = 2048.0f;
        // Stable identity. IDWriteFont pointers handed back by MapCharacters are
        // not stable across calls, so keying by pointer would mint a fresh face
        // every rebuild and defeat the glyph cache.
        std::wstring     key;
    };

    // Resolves (and caches) the face for a fallback font. Const because the
    // face cache is filled lazily during shaping; `faces_` is mutable.
    uint16_t faceIdForFont(IDWriteFont* font) const;
    float    faceEm(uint16_t face) const;

    IDWriteFactory*  factory_ = nullptr;
    IDWriteFontFace* face_    = nullptr;
    uint32_t         faceId_  = 0;
    bool             comOwned_ = false;   // whether we own the thread's COM init

    IDWriteFontCollection* collection_ = nullptr;
    IDWriteFontFallback*   fallback_   = nullptr;
    std::wstring           primaryFamily_;
    mutable std::vector<FaceEntry> faces_;

    // Font design metrics, in design units.
    float designUnitsPerEm_ = 2048.0f;
    float ascentUnits_      = 0.0f;
    float descentUnits_     = 0.0f;
    float lineGapUnits_     = 0.0f;
};

} // namespace tac::text
