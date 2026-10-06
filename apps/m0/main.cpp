// TacUI — M0 harness.
//
// Slice A: Win32 window + D3D12 device/swapchain + frame loop            [done]
// Slice B: rounded-rect SDF pipeline with analytic AA                    [done]
// Slice D: Element tree + reconcile + the direct-write override channel  [here]
//
// Slice D exists to prove the three M0 criteria that are specific to this
// design (plan.md §6.1):
//
//   2. a counter: setState -> rebuild -> diff -> the display updates
//   3. an animation driven purely by direct writes, with zero rebuilds
//   4. both paths live on the same node at once, and a rebuild does not
//      disturb an in-flight override
//
// Since text rendering is slice C, the counter is shown as a bar whose width
// encodes the value. Criterion 2 is about the diff, not about glyphs.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <tacui/tacui.hpp>

// Deliberately built only against the public entry point: the harness doubles
// as a test that the embedding API is sufficient on its own.

using namespace tac;

namespace {

constexpr uint32_t kInitialWidth  = 1280;
constexpr uint32_t kInitialHeight = 720;
constexpr Color    kClearColor    = Color::rgba8(24, 26, 32);

constexpr double kReportIntervalMs = 1000.0;

// Node identities. Keys are what let the direct-write path target a node
// without going through the build function.
constexpr ui::Key kButton      = 1;
constexpr ui::Key kCountBar    = 2;
constexpr ui::Key kCoexist     = 3;
constexpr ui::Key kAnimated    = 4;
constexpr ui::Key kAnimTarget  = 5;   // where the animated node is heading
constexpr ui::Key kTitle       = 6;
constexpr ui::Key kCountText   = 7;
constexpr ui::Key kHintText    = 8;

constexpr uint32_t kCountSlot = 0;

struct Options {
    bool        capture  = false;
    bool        selfTest = false;
    const char* outPath  = nullptr;
    double      seconds  = 0.0;   // 0 = run until the window closes
};

Options parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            o.capture = true;
            o.outPath = argv[++i];
        } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            o.seconds = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--selftest") == 0) {
            o.selfTest = true;
        }
    }
    return o;
}

struct App {
    ui::Ui ui;

    // Direct-write leases. Held across frames on purpose: an override lives as
    // long as its token does, independent of rebuilds.
    ui::OverrideToken animToken;
    ui::OverrideToken coexistToken;

    int32_t hoverX = -1;
    int32_t hoverY = -1;
    bool    buttonHover = false;

    // Set once the overrides are installed.
    bool leasesReady = false;

    double lastReport = 0.0;
};

// The build function. Produces a fresh VNode tree; base values only.
ui::VNode* buildUi(App& app, ui::VNodeArena& a) {
    ui::VNode* root = a.make(ui::VType::Stack);

    const int64_t count = app.ui.state(kCountSlot, 0);

    // --- counter button ---------------------------------------------------
    {
        auto* v = a.make(ui::VType::Rect, kButton);
        v->bounds       = Rect{ 60.0f, 60.0f, 190.0f, 58.0f };
        v->cornerRadius = 14.0f;
        v->color = app.buttonHover ? Color::rgba8(96, 156, 246)
                                   : Color::rgba8(58, 104, 186);
        a.addChild(root, v);
    }

    // --- the count, as text (slice C) ------------------------------------
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "count = %lld", static_cast<long long>(count));
        auto* v = a.make(ui::VType::Text, kCountText);
        v->bounds   = Rect{ 280.0f, 74.0f, 300.0f, 40.0f };
        v->fontSize = 30.0f;
        v->color    = Color::rgba8(235, 240, 250);
        v->text     = a.strdup(buf);
        a.addChild(root, v);
    }

    // --- the same count, as a bar: the two must stay in step -------------
    {
        const float w = 14.0f + static_cast<float>(count % 24) * 22.0f;
        auto* v = a.make(ui::VType::Rect, kCountBar);
        v->bounds       = Rect{ 60.0f, 140.0f, w, 22.0f };
        v->cornerRadius = 6.0f;
        v->color        = Color::rgba8(110, 220, 160);
        a.addChild(root, v);
    }

    // --- title ------------------------------------------------------------
    {
        auto* v = a.make(ui::VType::Text, kTitle);
        v->bounds   = Rect{ 60.0f, 16.0f, 900.0f, 28.0f };
        v->fontSize = 19.0f;
        v->color    = Color::rgba8(140, 148, 168);
        v->text     = a.strdup("TacUI M0 - text via DirectWrite + a glyph atlas");
        a.addChild(root, v);
    }

    // --- a line of longer text, to exercise cache reuse ------------------
    {
        auto* v = a.make(ui::VType::Text, kHintText);
        v->bounds   = Rect{ 60.0f, 320.0f, 900.0f, 30.0f };
        v->fontSize = 17.0f;
        v->color    = Color::rgba8(120, 128, 148);
        v->text = a.strdup(
            "The quick brown fox jumps over the lazy dog. 0123456789 !@#$%^&*()[]{}");
        a.addChild(root, v);
    }

    // --- criterion 4: base changes every rebuild, override must survive ---
    // The base colour is a function of `count`, so reconcile rewrites it on
    // every rebuild. A permanent opacity override sits on the same node. If
    // reconcile ever clobbered the override, this node would snap to opaque.
    {
        const uint8_t tint = static_cast<uint8_t>(60 + (count * 53) % 180);
        auto* v = a.make(ui::VType::Rect, kCoexist);
        v->bounds       = Rect{ 480.0f, 60.0f, 220.0f, 220.0f };
        v->cornerRadius = 22.0f;
        v->color        = Color::rgba8(tint, 96, 200);
        a.addChild(root, v);
    }

    // --- criterion 3: driven purely by direct writes ----------------------
    // The marker is added first so the travelling square paints over it.
    {
        auto* v = a.make(ui::VType::Rect, kAnimTarget);
        v->bounds       = Rect{ 800.0f, 270.0f, 90.0f, 6.0f };
        v->cornerRadius = 3.0f;
        v->color        = Color::rgba8(90, 96, 112);
        a.addChild(root, v);
    }
    {
        auto* v = a.make(ui::VType::Rect, kAnimated);
        v->bounds       = Rect{ 780.0f, 200.0f, 130.0f, 130.0f };
        v->cornerRadius = 30.0f;
        v->color        = Color::rgba8(240, 152, 64);
        a.addChild(root, v);
    }

    return root;
}

void printReport(const App& app, double elapsedSec, uint64_t frames) {
    const auto& s = app.ui.stats();
    std::printf(
        "[t=%6.2fs frames=%5llu] rebuilds=%4llu paints=%5llu survivedRebuild=%4llu | "
        "shaped=%3llu glyphs=%3llu (hits=%5llu) quads/frame=%3llu uploads=%llu\n",
        elapsedSec,
        static_cast<unsigned long long>(frames),
        static_cast<unsigned long long>(s.rebuilds),
        static_cast<unsigned long long>(s.paints),
        static_cast<unsigned long long>(s.reconcilesWithLiveOverride),
        static_cast<unsigned long long>(s.textShaped),
        static_cast<unsigned long long>(s.glyphsRasterized),
        static_cast<unsigned long long>(s.glyphCacheHits),
        static_cast<unsigned long long>(s.glyphQuads),
        static_cast<unsigned long long>(s.atlasUploads));
}

} // namespace

int main(int argc, char** argv) {
    const Options opts = parseArgs(argc, argv);

    App app;
    if (!app.ui.init([&app](ui::BuildContext& ctx) { return buildUi(app, ctx.arena); })) {
        std::fprintf(stderr, "fatal: failed to initialise the UI\n");
        return 1;
    }

    std::printf("M0 - criteria 2/3/4/6.\n");
    std::printf("`rebuilds` rises only on click/hover; `paints` rises every frame.\n\n");

    constexpr uint32_t kCaptureAfterFrames = 90;
    uint64_t           frames              = 0;
    bool               assertFailed        = false;

    auto onFrame = [&](host::Control& ctl, double elapsed) {
        frames = ctl.frame() + 1;   // Control::frame() counts completed frames

        // Hover changes a *base* property, so it has to go through a rebuild.
        // The animation below must not — that contrast is the whole point.
        {
            const Rect b   = app.ui.find(kButton).bounds();
            const bool hot = app.hoverX >= 0 &&
                             b.contains(static_cast<float>(app.hoverX),
                                        static_cast<float>(app.hoverY));
            if (hot != app.buttonHover) {
                app.buttonHover = hot;
                app.ui.invalidate();
            }
        }

        // Self-test clicks programmatically so criterion 4 can be verified
        // without a human at the mouse: each one forces a rebuild while the
        // animation override is live.
        if (opts.selfTest && ctl.frame() > 0 && ctl.frame() % 30 == 0) {
            app.ui.setState(kCountSlot, app.ui.state(kCountSlot, 0) + 1);
        }

        // Install the leases once the elements exist.
        if (!app.leasesReady && app.ui.root()) {
            app.animToken    = app.ui.find(kAnimated).beginOverride();
            app.coexistToken = app.ui.find(kCoexist).beginOverride();
            if (app.coexistToken) {
                app.coexistToken.setOpacity(0.45f);   // permanent, survives rebuilds
            }
            app.leasesReady = app.animToken.valid() && app.coexistToken.valid();
            if (!app.leasesReady) {
                std::fprintf(stderr, "fatal: could not acquire the override leases\n");
                ctl.requestExit();
                return;
            }
        }

        // Criterion 3: runs every frame and must never dirty the tree.
        if (app.animToken) {
            const float dy = std::sin(static_cast<float>(elapsed) * 2.2f) * 78.0f;
            app.animToken.setTranslate(0.0f, dy);
        }

        // Criterion 4: a rebuild must not have dislodged the override.
        if (app.coexistToken.valid()) {
            const ui::NodeRef n = app.ui.find(kCoexist);
            if (n.hasLiveOverride() && n.overrideOpacity() != 0.45f) {
                std::fprintf(stderr, "ASSERT FAILED: override opacity was clobbered\n");
                assertFailed = true;
                ctl.requestExit();
                return;
            }
        }

        if (opts.capture && ctl.frame() + 1 >= kCaptureAfterFrames) {
            ctl.captureNextFrame(opts.outPath);
        }

        if (elapsed - app.lastReport >= 1.0) {
            app.lastReport = elapsed;
            printReport(app, elapsed, ctl.frame());
        }
    };

    auto onEvent = [&app](const host::Event& ev) {
        switch (ev.type) {
        case host::EventType::MouseMove:
            app.hoverX = ev.x;
            app.hoverY = ev.y;
            break;

        case host::EventType::MouseLeave:
            app.hoverX = -1;
            app.hoverY = -1;
            break;

        case host::EventType::MouseDown: {
            // Hit test against *base* bounds: overrides are paint-only, so they
            // must not move the interactive region (architecture.md §14.4).
            const Rect b = app.ui.find(kButton).bounds();
            if (b.contains(static_cast<float>(ev.x), static_cast<float>(ev.y))) {
                app.ui.setState(kCountSlot, app.ui.state(kCountSlot, 0) + 1);
            }
            break;
        }

        default:
            break;
        }
    };

    host::Options hopts;
    hopts.title      = "TacUI - M0";
    hopts.width      = kInitialWidth;
    hopts.height     = kInitialHeight;
    hopts.clear      = kClearColor;
    hopts.maxSeconds = opts.seconds;

    const int rc = host::run(app.ui, hopts, onFrame, onEvent);
    if (rc != 0) return rc;
    if (assertFailed) return 2;
    if (!opts.selfTest) return 0;

    // ---- assertions ------------------------------------------------------
    // Run after the loop, outside any ctypes-style callback, so failures are
    // loud. host::run deliberately leaves the tree intact for this.
    const auto& s = app.ui.stats();

    std::printf("\n--- M0 self-test ---\n");
    std::printf("frames=%llu  rebuilds=%llu  paints=%llu  survivedRebuild=%llu\n",
                static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(s.rebuilds),
                static_cast<unsigned long long>(s.paints),
                static_cast<unsigned long long>(s.reconcilesWithLiveOverride));

    bool ok = true;
    auto check = [&ok](bool cond, const char* what) {
        std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
        if (!cond) ok = false;
    };

    // Criterion 2: state changes drive rebuilds.
    check(s.rebuilds >= 3, "criterion 2: setState caused rebuilds");
    // Criterion 3: paints happen every frame; rebuilds do not.
    check(s.paints > frames / 2, "criterion 3: painted nearly every frame");
    check(s.rebuilds * 8 < s.paints,
          "criterion 3: animation frames did not rebuild (rebuilds << paints)");
    // Criterion 4: overrides were live on both sides of repeated rebuilds.
    check(s.reconcilesWithLiveOverride >= 2,
          "criterion 4: override survived repeated rebuilds");
    check(app.coexistToken.valid() && app.ui.find(kCoexist).hasLiveOverride(),
          "criterion 4: coexist override still installed at exit");
    check(app.ui.find(kCoexist).overrideOpacity() == 0.45f,
          "criterion 4: coexist override value never clobbered");

    // Criterion 1c: text renders, and the glyph cache does its job.
    //
    // The signal is that rasterisation *plateaus*: it is bounded by the number
    // of distinct (glyph, size) pairs while frames and quads keep growing.
    check(s.glyphQuads > 50, "criterion 1c: text produced glyph quads");
    check(s.glyphsRasterized > 0 && s.glyphsRasterized < 150,
          "criterion 1c: rasterisation bounded to distinct glyph/size pairs");
    check(s.paints > s.glyphsRasterized * 5,
          "criterion 1c: one rasterisation serves many frames");
    check(s.textShaped * 8 < s.paints,
          "criterion 1c: shaped per rebuild, never per frame");
    check(s.atlasUploads <= s.glyphsRasterized,
          "criterion 1c: never more uploads than rasterised glyphs");
    check(s.atlasUploads < s.rebuilds,
          "criterion 1c: not one atlas upload per rebuild");
    check(s.atlasUploads * 8 < s.paints,
          "criterion 1c: atlas uploads do not track frames");

    // Criterion 6: the tree is inspectable.
    check(app.ui.root() != nullptr, "criterion 6: retained tree reachable");

    std::printf("--- %s ---\n", ok ? "ALL PASS" : "FAILURES PRESENT");
    return ok ? 0 : 2;
}
