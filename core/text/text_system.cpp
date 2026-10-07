#include "tacui/text_system.hpp"

#include <windows.h>

#include <dwrite.h>
#include <dwrite_2.h>   // IDWriteFactory2 / IDWriteFontFallback (DWrite 1.1)
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

// The minimum IDWriteTextAnalysisSource IDWriteFontFallback::MapCharacters
// needs: hand it the text and a locale, nothing else.
class RunAnalysisSource final : public IDWriteTextAnalysisSource {
public:
    RunAnalysisSource(const wchar_t* text, UINT32 length, const wchar_t* locale)
        : text_(text), length_(length), locale_(locale) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteTextAnalysisSource)) {
            *ppv = static_cast<IDWriteTextAnalysisSource*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 pos, const wchar_t** text,
                                                UINT32* length) override {
        if (pos >= length_) { *text = nullptr; *length = 0; return S_OK; }
        *text   = text_ + pos;
        *length = length_ - pos;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 pos, const wchar_t** text,
                                                    UINT32* length) override {
        if (pos == 0 || pos > length_) { *text = nullptr; *length = 0; return S_OK; }
        *text   = text_ + pos - 1;
        *length = 1;
        return S_OK;
    }
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override {
        return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    }
    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32, UINT32* length,
                                            const wchar_t** locale) override {
        *locale = locale_;
        *length = static_cast<UINT32>(std::wcslen(locale_));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32, UINT32* length,
                                                    IDWriteNumberSubstitution** sub) override {
        *sub    = nullptr;
        *length = length_;
        return S_OK;
    }

private:
    ~RunAnalysisSource() = default;

    LONG                ref_ = 1;
    const wchar_t*      text_;
    UINT32              length_;
    const wchar_t*      locale_;
};

// A stable identity for a font in the collection. IDWriteFont objects handed
// back by MapCharacters are not guaranteed to be the same pointer for the same
// font, so identity has to come from the family name plus the face style.
std::wstring fontKey(IDWriteFont* font) {
    if (!font) return {};

    std::wstring out;
    IDWriteFontFamily* family = nullptr;
    if (SUCCEEDED(font->GetFontFamily(&family)) && family) {
        ComPtr<IDWriteLocalizedStrings> names;
        if (SUCCEEDED(family->GetFamilyNames(&names)) && names->GetCount() > 0) {
            UINT32 len = 0;
            if (SUCCEEDED(names->GetStringLength(0, &len))) {
                std::vector<wchar_t> buf(len + 1, L'\0');
                if (SUCCEEDED(names->GetString(0, buf.data(), len + 1))) {
                    out.assign(buf.data(), len);
                }
            }
        }
        family->Release();
    }

    out.push_back(L'|');
    out += std::to_wstring(static_cast<int>(font->GetWeight()));
    out.push_back(L'|');
    out += std::to_wstring(static_cast<int>(font->GetStyle()));
    out.push_back(L'|');
    out += std::to_wstring(static_cast<int>(font->GetStretch()));
    return out;
}

// Glyph indices + advances for one run of codepoints, using `face`. Advances
// are pixels at `sizePx`, scaled by that face's own design units.
void appendRunGlyphs(ShapedLine& out, IDWriteFontFace* face, float designUnits,
                     float sizePx, const std::vector<UINT32>& codePoints,
                     uint16_t faceId, float& pen) {
    if (!face || codePoints.empty()) return;

    const UINT32 n = static_cast<UINT32>(codePoints.size());
    std::vector<UINT16> glyphs(n);
    if (FAILED(face->GetGlyphIndices(codePoints.data(), n, glyphs.data()))) return;

    std::vector<DWRITE_GLYPH_METRICS> metrics(n);
    if (FAILED(face->GetDesignGlyphMetrics(glyphs.data(), n, metrics.data(), FALSE))) return;

    const float scale = sizePx / (designUnits > 0.0f ? designUnits : 2048.0f);
    for (UINT32 i = 0; i < n; ++i) {
        const float advance = static_cast<float>(metrics[i].advanceWidth) * scale;
        out.glyphs.push_back(Glyph{ glyphs[i], faceId, pen, advance });
        pen += advance;
    }
    out.width = pen;
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

    // Keep the collection (fallback resolves against it) and the *resolved*
    // family name — a caller-supplied family that does not exist was already
    // remapped to index 0, so read the name back from the family.
    ComPtr<IDWriteLocalizedStrings> names;
    if (SUCCEEDED(fontFamily->GetFamilyNames(&names)) && names->GetCount() > 0) {
        UINT32 nameLen = 0;
        if (SUCCEEDED(names->GetStringLength(0, &nameLen))) {
            std::vector<wchar_t> buf(nameLen + 1, L'\0');
            if (SUCCEEDED(names->GetString(0, buf.data(), nameLen + 1))) {
                primaryFamily_.assign(buf.data(), nameLen);
            }
        }
    }

    FaceEntry primaryFace;
    primaryFace.face             = face_;
    primaryFace.font             = font.Detach();
    primaryFace.designUnitsPerEm = designUnitsPerEm_;
    primaryFace.key              = fontKey(primaryFace.font);
    faces_.push_back(primaryFace);

    collection_ = collection.Detach();

    // System font fallback needs DWrite 1.1 (IDWriteFactory2). If it is missing
    // the text stack still works, just single-face as before.
    ComPtr<IDWriteFactory2> factory2;
    if (SUCCEEDED(factory_->QueryInterface(IID_PPV_ARGS(&factory2)))) {
        IDWriteFontFallback* fb = nullptr;
        if (SUCCEEDED(factory2->GetSystemFontFallback(&fb))) fallback_ = fb;
    }

    return true;
}

void TextSystem::shutdown() {
    for (FaceEntry& f : faces_) {
        if (f.face) f.face->Release();
        if (f.font) f.font->Release();
    }
    faces_.clear();
    face_ = nullptr;   // owned by faces_[0]; just dropped
    primaryFamily_.clear();

    if (collection_) {
        collection_->Release();
        collection_ = nullptr;
    }
    if (fallback_) {
        fallback_->Release();
        fallback_ = nullptr;
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

uint16_t TextSystem::faceIdForFont(IDWriteFont* font) const {
    if (!font) return 0;

    const std::wstring key = fontKey(font);
    for (size_t i = 0; i < faces_.size(); ++i) {
        if (!faces_[i].key.empty() && faces_[i].key == key) {
            return static_cast<uint16_t>(i);
        }
    }

    IDWriteFontFace* face = nullptr;
    if (FAILED(font->CreateFontFace(&face)) || !face) return 0;

    DWRITE_FONT_METRICS fm{};
    face->GetMetrics(&fm);

    FaceEntry e;
    e.face             = face;
    e.font             = font;
    e.designUnitsPerEm = fm.designUnitsPerEm ? static_cast<float>(fm.designUnitsPerEm)
                                             : 2048.0f;
    e.key              = key;
    e.font->AddRef();   // the caller releases its fallback reference
    faces_.push_back(e);
    return static_cast<uint16_t>(faces_.size() - 1);
}

float TextSystem::faceEm(uint16_t face) const {
    if (face < faces_.size()) return faces_[face].designUnitsPerEm;
    return designUnitsPerEm_;
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

    float pen = 0.0f;

    // No fallback object (DWrite < 1.1): everything goes through the primary
    // face, which is the pre-fallback behaviour.
    if (!fallback_ || !collection_ || primaryFamily_.empty()) {
        appendRunGlyphs(out, face_, designUnitsPerEm_, sizePx, utf16ToUtf32(wide),
                        0, pen);
        return true;
    }

    static const wchar_t* kLocale = []() -> const wchar_t* {
        static wchar_t buf[LOCALE_NAME_MAX_LENGTH] = L"en-US";
        GetUserDefaultLocaleName(buf, LOCALE_NAME_MAX_LENGTH);
        return buf;
    }();

    const UINT32 length = static_cast<UINT32>(wide.size());
    RunAnalysisSource* source = new RunAnalysisSource(wide.data(), length, kLocale);

    // Walk the string in the runs the system fallback resolves. A run the base
    // family covers comes back as the base font; CJK and symbols come back as a
    // fallback family — that is what stops Chinese from rendering as tofu.
    UINT32 pos = 0;
    while (pos < length) {
        IDWriteFont* mapped       = nullptr;
        UINT32       mappedLength = 0;
        FLOAT        fontScale    = 1.0f;

        const HRESULT hr =
            fallback_->MapCharacters(source, pos, length - pos, collection_,
                                     primaryFamily_.c_str(),
                                     DWRITE_FONT_WEIGHT_NORMAL,
                                     DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL,
                                     &mappedLength, &mapped, &fontScale);
        if (FAILED(hr) || mappedLength == 0) break;   // no progress -> stop

        const std::vector<wchar_t> slice(wide.begin() + pos,
                                         wide.begin() + pos + mappedLength);

        uint16_t faceId = 0;
        float    em     = designUnitsPerEm_;
        if (mapped) {
            faceId = faceIdForFont(mapped);
            em     = faceEm(faceId);
            mapped->Release();
        }

        const float px = sizePx * (fontScale > 0.0f ? fontScale : 1.0f);
        IDWriteFontFace* runFace =
            (faceId < faces_.size()) ? faces_[faceId].face : face_;

        appendRunGlyphs(out, runFace, em, px, utf16ToUtf32(slice), faceId, pen);
        pos += mappedLength;
    }

    source->Release();
    return true;
}

bool TextSystem::rasterize(uint16_t face, uint16_t glyphIndex, float sizePx,
                           GlyphBitmap& out) const {
    out.alpha.clear();
    out.width = out.height = 0;
    out.bearingX = out.bearingY = 0;

    IDWriteFontFace* fontFace =
        (face < faces_.size()) ? faces_[face].face : face_;
    if (!fontFace) return false;

    DWRITE_GLYPH_RUN run{};
    run.fontFace     = fontFace;
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
