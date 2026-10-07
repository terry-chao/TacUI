// Verifies the text stack in isolation, before any GPU plumbing goes near it.
// Prints shaping results and an ASCII rendering of a rasterised glyph, so a
// failure here is obvious without a window or a screenshot.

#include <cmath>
#include <cstdio>
#include <cstring>

#include <tacui/tacui.hpp>

namespace {

char coverageChar(uint8_t a) {
    if (a > 200) return '#';
    if (a > 128) return '+';
    if (a > 60)  return '.';
    if (a > 0)   return ',';
    return ' ';
}

void printBitmap(const tac::text::GlyphBitmap& b) {
    for (uint32_t y = 0; y < b.height; ++y) {
        std::printf("  |");
        for (uint32_t x = 0; x < b.width; ++x) {
            std::putchar(coverageChar(b.alpha[static_cast<size_t>(y) * b.width + x]));
        }
        std::printf("|\n");
    }
}

} // namespace

int main(int argc, char** argv) {
    const char* sample = (argc > 1) ? argv[1] : "Hello, TacUI!";
    const float sizePx = 48.0f;

    tac::text::TextSystem text;
    if (!text.init("Segoe UI")) {
        std::fprintf(stderr, "FAIL: TextSystem::init\n");
        return 1;
    }

    std::printf("ascent=%.2f descent=%.2f lineHeight=%.2f\n",
                text.ascent(sizePx), text.descent(sizePx), text.lineHeight(sizePx));

    tac::text::ShapedLine line;
    if (!text.shape(sample, sizePx, line)) {
        std::fprintf(stderr, "FAIL: shape\n");
        return 1;
    }
    std::printf("shaped \"%s\": %zu glyphs, width=%.2fpx\n\n",
                sample, line.glyphs.size(), line.width);

    // Rasterise one glyph per distinct index and prove the ink is real.
    int rendered = 0;
    for (const auto& g : line.glyphs) {
        tac::text::GlyphBitmap bmp;
        if (!text.rasterize(g.index, sizePx, bmp)) {
            std::fprintf(stderr, "FAIL: rasterize(glyph %u)\n", g.index);
            return 1;
        }
        if (bmp.width == 0 || bmp.height == 0) {
            std::printf("glyph %-5u advance=%.2f  (no ink)\n", g.index, g.advance);
        } else {
            size_t nonzero = 0;
            uint8_t peak = 0;
            for (uint8_t a : bmp.alpha) {
                if (a) ++nonzero;
                if (a > peak) peak = a;
            }
            std::printf("glyph %-5u advance=%6.2f  bitmap=%ux%u  bearing=(%d,%d)  ink=%zu peak=%u\n",
                        g.index, g.advance, bmp.width, bmp.height,
                        bmp.bearingX, bmp.bearingY, nonzero, peak);
        }
        // Only draw a few, or the output becomes unreadable.
        if (rendered < 2 && bmp.width > 0) {
            printBitmap(bmp);
            std::printf("\n");
        }
        ++rendered;
    }

    // The atlas must hand out edge-to-edge UVs: the quad is exactly the glyph's
    // pixel size, so a UV span of w texels over w pixels is a 1:1 blit. Inset
    // ("texel-centre") UVs would squeeze w texels into w pixels and resample
    // every glyph — this catches that regression without a GPU.
    {
        tac::text::GlyphAtlas atlas;
        const uint32_t atlasSize = tac::rhi::kGlyphAtlasSize;
        if (!atlas.init(atlasSize)) {
            std::fprintf(stderr, "FAIL: GlyphAtlas::init\n");
            return 1;
        }

        const float   bucketF = sizePx + 0.5f;
        const uint16_t bucket = static_cast<uint16_t>(bucketF);
        int checked = 0;
        bool uvOk   = true;

        for (const auto& g : line.glyphs) {
            const tac::text::GlyphSlot* slot =
                atlas.get(tac::text::GlyphKey{ g.index, bucket }, text, sizePx);
            if (!slot || slot->width <= 0.0f || slot->height <= 0.0f) continue;   // blank

            const float spanU = (slot->u1 - slot->u0) * static_cast<float>(atlasSize);
            const float spanV = (slot->v1 - slot->v0) * static_cast<float>(atlasSize);
            if (std::fabs(spanU - slot->width) > 0.01f ||
                std::fabs(spanV - slot->height) > 0.01f) {
                std::fprintf(stderr,
                             "FAIL: glyph %u UV span is %.3fx%.3f texels but the "
                             "bitmap is %.0fx%.0f px (would resample)\n",
                             g.index, spanU, spanV, slot->width, slot->height);
                uvOk = false;
            }
            ++checked;
        }

        if (!uvOk) return 1;
        std::printf("\natlas UVs: %d glyphs, span == bitmap size (1:1 blit)\n", checked);
    }

    text.shutdown();
    std::printf("OK\n");
    return 0;
}
