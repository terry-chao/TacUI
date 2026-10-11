// Verifies the text stack in isolation, before any GPU plumbing goes near it.
// Prints shaping results and an ASCII rendering of a rasterised glyph, so a
// failure here is obvious without a window or a screenshot.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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
    // `--cjk` uses a hard-coded sample: the source file is UTF-8 and compiled
    // with /utf-8, whereas a command-line argument is re-encoded to the ANSI
    // code page by the CRT and would not be UTF-8 on a Chinese system.
    const bool  cjkMode = (argc > 1) && std::strcmp(argv[1], "--cjk") == 0;
    const char* sample  = cjkMode ? "中文测试 日本語 한국어 ✓"
                                  : ((argc > 1) ? argv[1] : "Hello, TacUI!");
    const float sizePx  = (argc > 2) ? static_cast<float>(std::atof(argv[2])) : 48.0f;

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

    // Every character above is non-blank, so nothing may resolve to .notdef
    // (glyph 0) — which is what a missing font fallback produced, as tofu.
    if (cjkMode) {
        int missing = 0;
        for (const auto& g : line.glyphs) {
            if (g.index == 0) ++missing;
        }
        if (missing != 0) {
            std::fprintf(stderr,
                         "FAIL: %d of %zu CJK glyphs are .notdef (font fallback "
                         "did not resolve them)\n",
                         missing, line.glyphs.size());
            return 1;
        }
        std::printf("CJK: %zu glyphs, none .notdef\n\n", line.glyphs.size());
    }

    // ---- wrapping ---------------------------------------------------------
    //
    // The breaker works off the shaped advances, so it is verifiable with no
    // window and no GPU: every line must fit the width it was offered, no glyph
    // may be lost, and the block height must follow from the line count. CJK
    // and Latin differ here (character breaks vs. word breaks) and both run
    // through the same loop, so the same three assertions cover both.
    {
        std::string longText;
        for (int i = 0; i < 3; ++i) {
            if (i) longText += " ";
            longText += sample;
        }

        tac::text::ShapedLine raw;
        if (!text.shape(longText.c_str(), sizePx, raw)) {
            std::fprintf(stderr, "FAIL: shape(long text)\n");
            return 1;
        }

        // Narrower than the raw run, so a wrap is mandatory — and deliberately
        // not a neat fraction of it, so the greedy breaker has to make real
        // choices rather than landing on equal lines.
        const float maxWidth = raw.width * 0.43f;

        tac::text::ShapedBlock block;
        if (!text.shapeBlock(longText.c_str(), sizePx, maxWidth, block)) {
            std::fprintf(stderr, "FAIL: shapeBlock\n");
            return 1;
        }

        std::printf("\nwrapping to %.2fpx (raw %.2fpx): %zu lines\n",
                    maxWidth, raw.width, block.lines.size());

        size_t kept = 0;
        int    over = 0;
        int    bad  = 0;
        for (size_t i = 0; i < block.lines.size(); ++i) {
            const tac::text::ShapedLine& l = block.lines[i];
            std::printf("  line %zu: %zu glyphs, width=%.2f\n", i, l.glyphs.size(), l.width);
            kept += l.glyphs.size();
            // A single glyph wider than the line is the documented overflow
            // case (it is never split); anything else must fit.
            if (l.width > maxWidth + 0.01f && l.glyphs.size() > 1) ++over;
            if (!l.glyphs.empty()) {
                if (l.glyphs.front().x < -0.001f) ++bad;   // not rebased on the line
                const auto& last = l.glyphs.back();
                if (std::fabs((last.x + last.advance) - l.width) > 0.01f) ++bad;
            }
        }

        const float expectHeight =
            text.ascent(sizePx) + text.descent(sizePx) +
            text.lineHeight(sizePx) * static_cast<float>(block.lines.size() - 1);

        int failures = 0;
        auto fail = [&failures](const char* what) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        };
        if (block.lines.size() < 2) fail("text wider than the line did not wrap");
        if (over) fail("a line exceeded the wrap width");
        if (bad) fail("a line is not rebased on its own origin");
        // Breaks swallow the spaces they land on, so a few glyphs may go; a
        // line's worth per line is the most that can disappear.
        if (kept > raw.glyphs.size() ||
            kept + block.lines.size() < raw.glyphs.size()) {
            fail("wrapping lost glyphs");
        }
        if (std::fabs(block.height() - expectHeight) > 0.01f) {
            fail("block height does not follow from the line count");
        }
        if (block.width > maxWidth + 0.01f) fail("block width exceeds the wrap width");
        if (failures) return 1;

        std::printf("wrap OK: %zu/%zu glyphs kept, height=%.2f, width=%.2f\n",
                    kept, raw.glyphs.size(), block.height(), block.width);

        // A token with no break opportunity at all: the breaker has to split
        // inside it, or it would overflow the container it was measured for.
        tac::text::ShapedLine  token;
        tac::text::ShapedBlock tokenBlock;
        const char*            tokenText = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        if (!text.shape(tokenText, sizePx, token) ||
            !text.shapeBlock(tokenText, sizePx, token.width * 0.5f, tokenBlock)) {
            std::fprintf(stderr, "FAIL: shapeBlock(no-space token)\n");
            return 1;
        }
        for (const auto& l : tokenBlock.lines) {
            if (l.width > token.width * 0.5f + 0.01f && l.glyphs.size() > 1) {
                std::fprintf(stderr, "FAIL: an unbreakable token overflowed\n");
                return 1;
            }
        }
        std::printf("wrap OK: unbreakable token split into %zu lines\n",
                    tokenBlock.lines.size());
    }

    // Rasterise one glyph per distinct index and prove the ink is real.
    int rendered = 0;
    for (const auto& g : line.glyphs) {
        tac::text::GlyphBitmap bmp;
        if (!text.rasterize(g.face, g.index, sizePx, bmp)) {
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
            std::printf("glyph %-5u face=%u advance=%6.2f  bitmap=%ux%u  bearing=(%d,%d)  ink=%zu peak=%u\n",
                        g.index, g.face, g.advance, bmp.width, bmp.height,
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
                atlas.get(tac::text::GlyphKey{ g.index, bucket, g.face }, text, sizePx);
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
