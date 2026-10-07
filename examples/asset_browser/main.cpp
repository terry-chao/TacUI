// TacUI example — a scene browser panel.
//
// Deliberately the shape of UI the project is aiming at: a game-tool panel
// with a list, a selection, a detail pane and a progress indicator.
//
// It exercises both change paths side by side, which is the point of the
// design (architecture.md §14):
//
//   * hovering or focusing a row changes a *base* property -> a rebuild
//   * the progress scan changes an *override*             -> no rebuild at all
//
// Note what is *not* here: no bounding-box tests, no hover bookkeeping, no
// "did the press wander off before release" logic, no focus tracking.
// Components declare what they are and what they do; the framework does hit
// testing, hover, press and focus. See docs/components.md §2.
//
//   asset_browser                 interactive; click a row, Tab to move focus
//   asset_browser --capture x.png render one frame to a PNG and exit

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <tacui/tacui.hpp>

using namespace tac;
using ui::rgb;

namespace {

// --- palette ---------------------------------------------------------------

constexpr Color kPanel       = rgb(34, 37, 46);
constexpr Color kRowPlain    = rgb(28, 31, 39);
constexpr Color kRowHover    = rgb(38, 42, 53);
constexpr Color kRowSelected = rgb(40, 62, 105);
constexpr Color kDivider     = rgb(48, 52, 64);
constexpr Color kTextStrong  = rgb(232, 236, 245);
constexpr Color kTextDim     = rgb(150, 158, 176);
constexpr Color kTextFaint   = rgb(104, 112, 132);
constexpr Color kAccent      = rgb(96, 156, 246);
constexpr Color kFocusRing   = rgb(130, 180, 255);
constexpr Color kDotIdle     = rgb(70, 76, 92);
constexpr Color kTrack       = rgb(38, 42, 53);
constexpr Color kScan        = rgb(110, 220, 160);

// --- identity --------------------------------------------------------------
// Keys are what the direct-write path addresses, and what behaviour binds to.

constexpr ui::Key kRowBase    = 100;   // kRowBase + index
constexpr ui::Key kBakeButton = 10;
constexpr ui::Key kScanFill   = 11;

constexpr uint32_t kSlotSelected = 0;

// --- content ---------------------------------------------------------------

struct Item {
    const char* name;
    const char* vertices;
    const char* materials;
};

constexpr Item kItems[] = {
    { "Camera Rig",  "1,204",   "3"  },
    { "Terrain",     "48,912",  "12" },
    { "Character_A", "9,530",   "7"  },
    { "Light_Main",  "0",       "1"  },
    { "Skybox",      "36",      "2"  },
};
constexpr int kItemCount = static_cast<int>(sizeof(kItems) / sizeof(kItems[0]));

// --- layout ----------------------------------------------------------------

constexpr float kListX  = 20.0f;
constexpr float kListY  = 76.0f;
constexpr float kListW  = 340.0f;
constexpr float kRowH   = 34.0f;
constexpr float kRowGap = 4.0f;

constexpr float kDetailX = 384.0f;
constexpr float kDetailY = 76.0f;
constexpr float kDetailW = 396.0f;
constexpr float kDetailH = 232.0f;

constexpr float kButtonX = kDetailX + 20.0f;
constexpr float kButtonY = kDetailY + 140.0f;
constexpr float kButtonW = 96.0f;
constexpr float kButtonH = 30.0f;

constexpr float kTrackX   = kDetailX + 20.0f;
constexpr float kTrackY   = kDetailY + 192.0f;
constexpr float kTrackW   = kDetailW - 40.0f;
constexpr float kTrackH   = 8.0f;
constexpr float kScanW    = 64.0f;
constexpr float kScanSpan = kTrackW - kScanW;

struct App {
    ui::Ui* ui     = nullptr;
    bool    baking = false;

    // Direct-write lease for the scanning indicator. Held across frames on
    // purpose: an override lives as long as its token, not as long as a frame.
    ui::OverrideToken scan;

    double lastReport = 0.0;
};

float rowTop(int index) {
    return kListY + static_cast<float>(index) * (kRowH + kRowGap);
}

// The build function. Everything here writes *base* values only.
ui::VNode* buildUi(ui::Builder& b, App& app) {
    const int selected = static_cast<int>(app.ui->state(kSlotSelected, 0));

    auto root = b.stack();

    // ---- header ---------------------------------------------------------
    b.text("Scene Browser").box(20, 20, 400, 28).size(19).color(kTextStrong);
    b.text("examples/asset_browser").box(20, 46, 400, 20).size(13).color(kTextFaint);
    b.rect().box(20, 68, 760, 1).color(kDivider);

    // ---- list -----------------------------------------------------------
    for (int i = 0; i < kItemCount; ++i) {
        const ui::Key key = kRowBase + i;
        const float   y   = rowTop(i);
        const bool    sel = (i == selected);

        // Interaction state comes from the framework, so what lights up under
        // the pointer is by construction what responds to it.
        const bool hot     = b.ui().isHovered(key);
        const bool pressed = b.ui().isPressed(key);

        const Color row = (pressed || sel) ? kRowSelected : hot ? kRowHover : kRowPlain;

        // Focus ring: a slightly larger rect behind the row.
        if (b.ui().isFocused(key)) {
            b.rect().box(kListX - 2, y - 2, kListW + 4, kRowH + 4)
                .radius(10).color(kFocusRing);
        }

        b.rect().box(kListX, y, kListW, kRowH).radius(8).color(row).key(key);
        b.rect().box(kListX + 12, y + 11, 12, 12).radius(6)
            .color(sel ? kAccent : kDotIdle);
        b.text(kItems[i].name).box(kListX + 36, y + 7, kListW - 48, 24)
            .size(16).color(sel ? kTextStrong : kTextDim);

        // What it does, declared once and next to the thing it belongs to.
        b.behavior(key, ui::NodeBehavior{
            .onClick = [&app, i] { app.ui->setState(kSlotSelected, i); },
            .onKey   = [&app, i](const ui::InputEvent& ev) {
                if (ev.code == ui::KeyCode::Enter || ev.code == ui::KeyCode::Space) {
                    app.ui->setState(kSlotSelected, i);
                }
            },
            .interactive = true,
            .focusable   = true,
        });
    }

    // ---- detail pane ----------------------------------------------------
    b.rect().box(kDetailX, kDetailY, kDetailW, kDetailH).radius(12).color(kPanel);

    b.text("SELECTED").box(kDetailX + 20, kDetailY + 18, 200, 16)
        .size(11).color(kTextFaint);
    b.text(kItems[selected].name)
        .box(kDetailX + 20, kDetailY + 38, kDetailW - 40, 30)
        .size(21).color(kTextStrong);

    b.rect().box(kDetailX + 20, kDetailY + 78, kDetailW - 40, 1).color(kDivider);

    b.text("Vertices").box(kDetailX + 20, kDetailY + 94, 160, 20)
        .size(13).color(kTextFaint);
    b.text(kItems[selected].vertices).box(kDetailX + 200, kDetailY + 94, 160, 20)
        .size(13).color(kTextDim);

    b.text("Materials").box(kDetailX + 20, kDetailY + 118, 160, 20)
        .size(13).color(kTextFaint);
    b.text(kItems[selected].materials).box(kDetailX + 200, kDetailY + 118, 160, 20)
        .size(13).color(kTextDim);

    // ---- progress track + the direct-written scan -----------------------
    b.rect().box(kTrackX, kTrackY, kTrackW, kTrackH).radius(kTrackH * 0.5f).color(kTrack);
    b.rect().box(kTrackX, kTrackY, kScanW, kTrackH).radius(kTrackH * 0.5f)
        .color(kScan).key(kScanFill);

    b.text(app.baking ? "Baking..." : "Idle")
        .box(kTrackX, kTrackY + 16, 200, 20)
        .size(12).color(kTextFaint);

    // ---- button ---------------------------------------------------------
    {
        const bool hot     = b.ui().isHovered(kBakeButton);
        const bool pressed = b.ui().isPressed(kBakeButton);
        const bool focused = b.ui().isFocused(kBakeButton);

        const Rect button{ kButtonX, kButtonY, kButtonW, kButtonH };

        if (focused) {
            b.rect().box(kButtonX - 2, kButtonY - 2, kButtonW + 4, kButtonH + 4)
                .radius(10).color(kFocusRing);
        }

        b.rect().box(button).radius(8)
            .color(app.baking ? kAccent
                              : pressed ? kRowSelected
                                        : hot ? rgb(60, 68, 86) : rgb(48, 54, 68))
            .key(kBakeButton);

        b.text("Bake").box(button.x + 30, button.y + 7, 80, 20)
            .size(14).color(app.baking ? kTextStrong : kTextDim);

        b.behavior(kBakeButton, ui::NodeBehavior{
            .onClick = [&app] {
                app.baking = !app.baking;
                app.ui->invalidate();
            },
            .interactive = true,
            .focusable   = true,
        });
    }

    return b.root();
}

void report(const App& app) {
    const auto& s = app.ui->stats();
    std::printf(
        "rebuilds=%4llu  paints=%5llu  |  shaped=%3llu glyphs=%3llu quads=%3llu  "
        "|  liveOverrides=%u\n",
        static_cast<unsigned long long>(s.rebuilds),
        static_cast<unsigned long long>(s.paints),
        static_cast<unsigned long long>(s.textShaped),
        static_cast<unsigned long long>(s.glyphsRasterized),
        static_cast<unsigned long long>(s.glyphQuads),
        s.liveOverrides);
}

// ---------------------------------------------------------------------------
// Input system self-test
//
// Drives synthetic events and asserts what the framework did with them. Runs
// headless — no window, no device — so it can live in CI.
// ---------------------------------------------------------------------------

ui::InputEvent moveTo(float x, float y) {
    ui::InputEvent e;
    e.type = ui::InputEventType::MouseMove;
    e.x = x;
    e.y = y;
    return e;
}

ui::InputEvent buttonAt(ui::InputEventType type, float x, float y) {
    ui::InputEvent e;
    e.type = type;
    e.x = x;
    e.y = y;
    return e;
}

ui::InputEvent pressTab(bool shift = false) {
    ui::InputEvent e;
    e.type  = ui::InputEventType::KeyDown;
    e.code  = ui::KeyCode::Tab;
    e.shift = shift;
    return e;
}

int runInputSelfTest(ui::Ui& ui) {
    // Build once so there is a tree for events to route against.
    ui.update();

    bool ok = true;
    auto check = [&ok](bool cond, const char* what) {
        std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
        if (!cond) ok = false;
    };

    const auto rowY = [](int i) { return rowTop(i) + kRowH * 0.5f; };

    std::printf("\n--- input system self-test ---\n");

    // --- hover ------------------------------------------------------------
    ui.dispatchEvent(moveTo(kListX + 10, rowY(2)));
    check(ui.hoveredKey() == kRowBase + 2,
          "hover lands on the row under the pointer");

    // Hover is re-evaluated after a rebuild, because the tree may have moved
    // under a stationary pointer.
    const float staleX = kListX + 10;
    ui.invalidate();
    ui.update();
    check(ui.hoveredKey() == kRowBase + 2,
          "hover survives a rebuild under a stationary pointer");

    // --- click ------------------------------------------------------------
    ui.dispatchEvent(buttonAt(ui::InputEventType::MouseDown, staleX, rowY(3)));
    check(ui.pressedKey() == kRowBase + 3, "press is tracked on the hit node");

    ui.dispatchEvent(buttonAt(ui::InputEventType::MouseUp, staleX, rowY(3)));
    ui.update();
    check(ui.state(kSlotSelected, 0) == 3,
          "press + release inside the row selects it");

    // --- a press that wanders off is not a click --------------------------
    const int64_t before = ui.state(kSlotSelected, 0);
    ui.dispatchEvent(buttonAt(ui::InputEventType::MouseDown, staleX, rowY(1)));
    ui.dispatchEvent(moveTo(700.0f, 320.0f));            // off the row entirely
    ui.dispatchEvent(buttonAt(ui::InputEventType::MouseUp, 700.0f, 320.0f));
    ui.update();
    check(ui.state(kSlotSelected, 0) == before,
          "a press that drags off the node does not click it");

    // --- clicks outside anything -----------------------------------------
    const int64_t empty = ui.state(kSlotSelected, 0);
    ui.dispatchEvent(buttonAt(ui::InputEventType::MouseDown, 700.0f, 320.0f));
    ui.dispatchEvent(buttonAt(ui::InputEventType::MouseUp, 700.0f, 320.0f));
    ui.update();
    check(ui.state(kSlotSelected, 0) == empty,
          "a click on empty space changes nothing");

    // --- focus ------------------------------------------------------------
    // Start from nothing: the clicks above already moved focus, since clicking
    // a focusable node takes it.
    check(ui.focusedKey() == kRowBase + 1, "clicking moved focus to the clicked row");

    ui.clearFocus();
    ui.dispatchEvent(pressTab());
    check(ui.focusedKey() == kRowBase + 0, "Tab focuses the first focusable node");

    ui.dispatchEvent(pressTab());
    check(ui.focusedKey() == kRowBase + 1, "Tab advances in tree order");

    ui.dispatchEvent(pressTab(true));
    check(ui.focusedKey() == kRowBase + 0, "Shift+Tab goes back");

    // Clicking a focusable node takes focus, the way every desktop toolkit does.
    ui.dispatchEvent(buttonAt(ui::InputEventType::MouseDown, kButtonX + 10, kButtonY + 10));
    ui.dispatchEvent(buttonAt(ui::InputEventType::MouseUp, kButtonX + 10, kButtonY + 10));
    ui.update();
    check(ui.focusedKey() == kBakeButton, "clicking a focusable node focuses it");

    // --- key routing ------------------------------------------------------
    ui.dispatchEvent(pressTab(true));                    // back to the last row
    const ui::Key focusedRow = ui.focusedKey();
    const bool isRow = focusedRow >= kRowBase && focusedRow < kRowBase + kItemCount;
    check(isRow, "focus is on a list row");

    if (isRow) {
        ui::InputEvent enter;
        enter.type = ui::InputEventType::KeyDown;
        enter.code = ui::KeyCode::Enter;
        ui.dispatchEvent(enter);
        ui.update();
        check(ui.state(kSlotSelected, 0) ==
                  static_cast<int64_t>(focusedRow - kRowBase),
              "Enter activates the focused row");
    }

    // --- focus survives rebuilds -----------------------------------------
    const ui::Key beforeRebuild = ui.focusedKey();
    ui.invalidate();
    ui.update();
    check(ui.focusedKey() == beforeRebuild,
          "focus survives a rebuild while its node still exists");

    std::printf("--- %s ---\n", ok ? "ALL PASS" : "FAILURES PRESENT");
    return ok ? 0 : 2;
}

} // namespace

int main(int argc, char** argv) {
    const char* capturePath = nullptr;
    double      maxSeconds  = 0.0;
    bool        inputTest   = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            capturePath = argv[++i];
        } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            maxSeconds = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--input-test") == 0) {
            inputTest = true;
        }
    }

    App    app;
    ui::Ui ui;
    app.ui = &ui;

    if (!ui.init([&app](ui::BuildContext& ctx) {
            ui::Builder b(ctx);
            ui::VNode*  root = buildUi(b, app);
            if (!b.balanced()) {
                std::fprintf(stderr, "warning: unbalanced stack scopes\n");
            }
            return root;
        })) {
        std::fprintf(stderr, "fatal: failed to initialise the UI\n");
        return 1;
    }

    if (inputTest) {
        return runInputSelfTest(ui);
    }

    // No event callback at all: every interaction in this panel is declared by
    // the build. The host still routes raw input into the framework, which is
    // what makes that possible.
    auto onFrame = [&app, capturePath](host::Control& ctl, double t) {
        if (!app.scan) {
            app.scan = app.ui->find(kScanFill).beginOverride();
        }

        // Runs every frame and never marks the tree dirty, so `rebuilds` stays
        // put while the scan keeps moving. Translate is paint-only.
        if (app.scan) {
            const float phase = static_cast<float>(std::sin(t * 1.3) * 0.5 + 0.5);
            app.scan.setTranslate(phase * kScanSpan, 0.0f);
        }

        if (t - app.lastReport >= 2.0) {
            app.lastReport = t;
            report(app);
        }

        if (capturePath && ctl.frame() + 1 >= 60) {
            ctl.captureNextFrame(capturePath);
        }
    };

    host::Options opts;
    opts.title      = "TacUI - Scene Browser";
    opts.width      = 800;
    opts.height     = 340;
    opts.clear      = rgb(22, 24, 30);
    opts.maxSeconds = maxSeconds;
    // Golden captures pin the scale so the output is DPI-independent.
    opts.uiScale    = capturePath ? 1.0f : 0.0f;

    return host::run(ui, opts, onFrame);
}
