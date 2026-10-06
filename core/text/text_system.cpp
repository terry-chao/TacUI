#include "tacui/text_system.hpp"

#include <windows.h>

#include <dwrite.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace tac::text {

namespace {

// Opt-in tracing: TACUI_TEXT_DEBUG=1.
bool debugEnabled() {
    static const bool enabled = [] {
        size_t len = 0;
        char   buf[8] = {};
        return getenv_s(&len, buf, sizeof(buf), "TACUI_TEXT_DEBUG") == 0 && len > 1;
    }();
    return enabled;
}

std::vector<wchar_t> utf8ToWide(const char* utf8) {
    const int need = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (need <= 1) return {};
    std::vector<wchar_t> buf(static_cast<size_t>(need));
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, buf.data(), need);
    buf.pop_back();   // drop the terminator; callers use .size()
    return buf;
}

// IDWriteFontFace::GetGlyphIndices takes UTF-32 code points, not the UTF-16
// code units Windows calls wchar_t. Passing the wide buffer straight through
// reinterprets pairs of code units as one point and yields .notdef (glyph 0)
// for most input.
std::vector<UINT32> utf16ToUtf32(const std::vector<wchar_t>& in) {
    std::vector<UINT32> out;
    out.reserve(in.size());

    for (size_t i = 0; i < in.size(); ++i) {
        const uint32_t unit = static_cast<uint16_t>(in[i]);
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < in.size()) {
            const uint32_t low = static_cast<uint16_t>(in[i + 1]);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                out.push_back(0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u));
                ++i;
                continue;
            }
        }
        out.push_back(unit);
    }
    return out;
}

} // namespace

bool TextSystem::init(const char* familyUtf8) {
    // COM must stay initialised for the whole lifetime of this object, not just
    // for init() — DirectWrite calls below happen all the way through shaping
    // and rasterisation. RPC_E_CHANGED_MODE means another component already set
    // up a different apartment here: still usable, just not ours to tear down.
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    comOwned_ = SUCCEEDED(comHr);

    IUnknown* rawFactory = nullptr;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   &rawFactory)) ||
        !rawFactory) {
        std::fprintf(stderr, "[text] DWriteCreateFactory failed\n");
        return false;
    }
    factory_ = static_cast<IDWriteFactory*>(rawFactory);

    ComPtr<IDWriteFontCollection> collection;
    if (FAILED(factory_->GetSystemFontCollection(&collection))) {
        std::fprintf(stderr, "[text] no system font collection\n");
        return false;
    }

    const std::vector<wchar_t> family = utf8ToWide(familyUtf8 ? familyUtf8 : "Segoe UI");

    UINT32 index  = 0;
    BOOL   exists = FALSE;
    if (family.empty() ||
        FAILED(collection->FindFamilyName(family.data(), &index, &exists)) || !exists) {
        std::fprintf(stderr, "[text] font family not found, falling back\n");
        // Fall back to whatever family index 0 is rather than failing outright.
        if (collection->GetFontFamilyCount() == 0) return false;
        index = 0;
    }

    ComPtr<IDWriteFontFamily> fontFamily;
    if (FAILED(collection->GetFontFamily(index, &fontFamily))) {
        std::fprintf(stderr, "[text] GetFontFamily failed\n");
        return false;
    }

    ComPtr<IDWriteFont> font;
    if (FAILED(fontFamily->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL,
                                                DWRITE_FONT_STRETCH_NORMAL,
                                                DWRITE_FONT_STYLE_NORMAL,
                                                &font))) {
        std::fprintf(stderr, "[text] no matching font\n");
        return false;
    }

    if (FAILED(font->CreateFontFace(&face_)) || !face_) {
        std::fprintf(stderr, "[text] CreateFontFace failed\n");
        return false;
    }

    DWRITE_FONT_METRICS fm{};
    face_->GetMetrics(&fm);
    designUnitsPerEm_ = static_cast<float>(fm.designUnitsPerEm);
    ascentUnits_      = static_cast<float>(fm.ascent);
    descentUnits_     = static_cast<float>(fm.descent);
    lineGapUnits_     = static_cast<float>(fm.lineGap);
    faceId_           = 1;

    return true;
}

void TextSystem::shutdown() {
    if (face_) {
        face_->Release();
        face_ = nullptr;
    }
    if (factory_) {
        factory_->Release();
        factory_ = nullptr;
    }
    if (comOwned_) {
        CoUninitialize();
        comOwned_ = false;
    }
    faceId_ = 0;
}

float TextSystem::ascent(float sizePx) const {
    return ascentUnits_ * sizePx / designUnitsPerEm_;
}

float TextSystem::descent(float sizePx) const {
    return descentUnits_ * sizePx / designUnitsPerEm_;
}

float TextSystem::lineHeight(float sizePx) const {
    return (ascentUnits_ + descentUnits_ + lineGapUnits_) * sizePx / designUnitsPerEm_;
}

bool TextSystem::shape(const char* utf8, float sizePx, ShapedLine& out) const {
    out.glyphs.clear();
    out.width = 0.0f;
    if (!face_ || !utf8 || !*utf8) return true;   // empty string is not an error

    const std::vector<wchar_t> wide = utf8ToWide(utf8);
    if (wide.empty()) return true;

    const std::vector<UINT32> codePoints = utf16ToUtf32(wide);
    const UINT32 count = static_cast<UINT32>(codePoints.size());

    std::vector<UINT16> glyphs(count);
    if (FAILED(face_->GetGlyphIndices(codePoints.data(), count, glyphs.data()))) {
        std::fprintf(stderr, "[text] GetGlyphIndices failed\n");
        return false;
    }

    std::vector<DWRITE_GLYPH_METRICS> metrics(count);
    if (FAILED(face_->GetDesignGlyphMetrics(glyphs.data(), count, metrics.data(), FALSE))) {
        std::fprintf(stderr, "[text] GetDesignGlyphMetrics failed\n");
        return false;
    }

    const float scale = sizePx / designUnitsPerEm_;
    out.glyphs.reserve(count);

    float pen = 0.0f;
    for (UINT32 i = 0; i < count; ++i) {
        const float advance = static_cast<float>(metrics[i].advanceWidth) * scale;
        out.glyphs.push_back(Glyph{ glyphs[i], pen, advance });
        pen += advance;
    }
    out.width = pen;
    return true;
}

bool TextSystem::rasterize(uint16_t glyphIndex, float sizePx, GlyphBitmap& out) const {
    out.alpha.clear();
    out.width = out.height = 0;
    out.bearingX = out.bearingY = 0;
    if (!face_) return false;

    DWRITE_GLYPH_RUN run{};
    run.fontFace     = face_;
    run.fontEmSize   = sizePx;
    run.glyphCount   = 1;
    run.bidiLevel    = 0;
    run.isSideways   = FALSE;

    FLOAT                advance = 0.0f;
    DWRITE_GLYPH_OFFSET  offset{ 0.0f, 0.0f };
    run.glyphIndices  = &glyphIndex;
    run.glyphAdvances = &advance;
    run.glyphOffsets  = &offset;

    // Baseline origin at (0,0): the returned bounds are then already relative
    // to the pen position, which is exactly what the atlas wants to store.
    const DWRITE_MATRIX transform{ 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };

    // GetAlphaTextureBounds returns an *empty rect* — not an error — when the
    // requested texture type does not match the mode the analysis was created
    // with, so a wrong guess fails silently rather than loudly.
    //
    // Measured on Segoe UI: every ALIASED_1x1 combination yields empty bounds;
    // only CLEARTYPE_3x1 produces ink. That is counter-intuitive, since 1x1 is
    // the format the docs describe as grayscale, but the analysis object only
    // exposes subpixel coverage. So the working combination goes first and the
    // rest stay as a fallback for fonts that behave differently.
    //
    // Consequence: grayscale is recovered by averaging the RGB coverage, which
    // is not identical to native grayscale AA (ClearType applies its own
    // contrast/gamma tuning). Acceptable for M0; M1 swaps in a rasteriser that
    // produces true coverage.
    struct Attempt {
        DWRITE_RENDERING_MODE mode;
        DWRITE_TEXTURE_TYPE   texture;
        uint32_t              bytesPerPixel;
    };
    static const Attempt kAttempts[] = {
        { DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, DWRITE_TEXTURE_CLEARTYPE_3x1, 3 },
        { DWRITE_RENDERING_MODE_NATURAL,           DWRITE_TEXTURE_CLEARTYPE_3x1, 3 },
        { DWRITE_RENDERING_MODE_DEFAULT,           DWRITE_TEXTURE_CLEARTYPE_3x1, 3 },
        { DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, DWRITE_TEXTURE_ALIASED_1x1,   1 },
        { DWRITE_RENDERING_MODE_DEFAULT,           DWRITE_TEXTURE_ALIASED_1x1,   1 },
    };

    for (const Attempt& attempt : kAttempts) {
        ComPtr<IDWriteGlyphRunAnalysis> analysis;
        if (FAILED(factory_->CreateGlyphRunAnalysis(&run, 1.0f, &transform, attempt.mode,
                                                    DWRITE_MEASURING_MODE_NATURAL,
                                                    0.0f, 0.0f, &analysis))) {
            continue;
        }

        RECT bounds{};
        if (FAILED(analysis->GetAlphaTextureBounds(attempt.texture, &bounds))) continue;

        const int32_t w = bounds.right - bounds.left;
        const int32_t h = bounds.bottom - bounds.top;
        if (w <= 0 || h <= 0) continue;

        const size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
        std::vector<uint8_t> raw(pixelCount * attempt.bytesPerPixel);
        if (FAILED(analysis->CreateAlphaTexture(attempt.texture, &bounds, raw.data(),
                                                static_cast<UINT32>(raw.size())))) {
            continue;
        }

        out.width    = static_cast<uint32_t>(w);
        out.height   = static_cast<uint32_t>(h);
        out.bearingX = bounds.left;
        out.bearingY = -bounds.top;   // bitmap top is above the baseline
        out.alpha.resize(pixelCount);

        if (attempt.bytesPerPixel == 1) {
            std::memcpy(out.alpha.data(), raw.data(), pixelCount);
        } else {
            // ClearType is RGB subpixel coverage; flatten it to one channel.
            for (size_t i = 0; i < pixelCount; ++i) {
                const uint32_t sum = raw[i * 3] + raw[i * 3 + 1] + raw[i * 3 + 2];
                out.alpha[i] = static_cast<uint8_t>(sum / 3);
            }
        }

        if (debugEnabled()) {
            std::fprintf(stderr,
                         "[text] glyph %u: mode=%d texture=%d %dx%d bearing=(%d,%d)\n",
                         glyphIndex, static_cast<int>(attempt.mode),
                         static_cast<int>(attempt.texture),
                         w, h, out.bearingX, out.bearingY);
        }
        return true;
    }

    // No combination produced ink. For a blank glyph (space, tab) that is the
    // correct answer, not a failure.
    if (debugEnabled()) {
        std::fprintf(stderr, "[text] glyph %u size=%.1f: no ink from any attempt\n",
                     glyphIndex, sizePx);
    }
    return true;
}

} // namespace tac::text
