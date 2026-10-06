// TacUI example — the control gallery.
//
// Two jobs:
//
//   1. Show every L2 control in one window, so the set is reviewable at a
//      glance and the screenshot is a usable regression reference.
//   2. Be the acceptance test for the control layer. `--input-test` drives
//      synthetic events and asserts what the controls did — headless, no
//      window, no GPU, so it can run in CI.
//
// Note what is absent from the build function: no bounding-box tests, no hover
// bookkeeping, no press/capture state, no focus tracking. Every control is a
// function call; the input system owns the interaction (docs/components.md).
//
//   controls_gallery               interactive
//   controls_gallery --input-test  scripted assertions, non-zero on failure
//   controls_gallery --capture x.png

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <tacui/tacui.hpp>

using namespace tac;

namespace {

// --- identity --------------------------------------------------------------

constexpr ui::Key kBtnPrimary   = 1;
constexpr ui::Key kBtnSecondary = 2;
constexpr ui::Key kBtnGhost     = 3;
constexpr ui::Key kBtnDanger    = 4;
constexpr ui::Key kBtnDisabled  = 5;

constexpr ui::Key kCheckA = 10;
constexpr ui::Key kCheckB = 11;

constexpr ui::Key kRadioBase = 20;   // + index

constexpr ui::Key kSwitchA = 30;
constexpr ui::Key kSwitchB = 31;

constexpr ui::Key kSlider     = 40;
constexpr ui::Key kProgress   = 41;
constexpr ui::Key kTabsBase   = 50;  // + index
constexpr ui::Key kInput      = 60;
constexpr ui::Key kPanelLeft  = 70;
constexpr ui::Key kPanelMid   = 71;
constexpr ui::Key kPanelRight = 72;
constexpr ui::Key kPanelText  = 73;
constexpr ui::Key kPanelTabs  = 74;

// --- layout ----------------------------------------------------------------
// Absolute, because M2's Measure/Arrange does not exist yet. Controls take a
// box; they do not invent one.

constexpr Rect kBoxPrimary  { 40.0f, 136.0f, 116.0f, 32.0f };
constexpr Rect kBoxSecondary{ 166.0f, 136.0f, 112.0f, 32.0f };
constexpr Rect kBoxGhost    { 40.0f, 178.0f, 116.0f, 32.0f };
constexpr Rect kBoxDanger   { 166.0f, 178.0f, 112.0f, 32.0f };
constexpr Rect kBoxDisabled { 40.0f, 220.0f, 238.0f, 32.0f };

constexpr Rect kBoxCheckA { 326.0f, 138.0f, 238.0f, 24.0f };
constexpr Rect kBoxCheckB { 326.0f, 170.0f, 238.0f, 24.0f };
constexpr Rect kBoxRadio  { 326.0f, 206.0f, 238.0f, 24.0f };
constexpr float kRadioStep = 28.0f;

constexpr Rect kBoxSwitchA { 612.0f, 138.0f, 238.0f, 24.0f };
constexpr Rect kBoxSwitchB { 612.0f, 174.0f, 238.0f, 24.0f };
constexpr Rect kBoxSlider  { 612.0f, 214.0f, 238.0f, 24.0f };

constexpr Rect kBoxTabs  { 612.0f, 374.0f, 238.0f, 32.0f };
constexpr Rect kBoxInput { 40.0f, 386.0f, 420.0f, 36.0f };

constexpr int kTabCount   = 3;
constexpr int kRadioCount = 3;

const char* const kTabs[kTabCount]     = { "Diff", "Scene", "Render" };
const char* const kRadios[kRadioCount] = { "Vertex", "Edge", "Face" };

// --- app -------------------------------------------------------------------

struct App {
    ui::Ui* ui = nullptr;

    int   clicks    = 0;
    bool  checkA    = true;
    bool  checkB    = false;
    int   radio     = 0;
    bool  switchA   = false;
    bool  switchB   = true;     // disabled, must not react
    float slider    = 0.25f;
    float progress  = 0.62f;
    int   tab       = 0;

    ui::TextInputState input;

    double lastReport = 0.0;
};

Rect radioBox(int i) {
    return Rect{ kBoxRadio.x, kBoxRadio.y + kRadioStep * static_cast<float>(i),
                 kBoxRadio.w, kBoxRadio.h };
}

// ---------------------------------------------------------------------------
// Build — every interaction below is *declared*, never tested for.
// ---------------------------------------------------------------------------

ui::VNode* buildUi(ui::Builder& b, App& app) {
    auto root = b.stack();

    ui::label(b, 0, { 24.0f, 20.0f, 600.0f, 28.0f },
              { .text = "TacUI controls", .size = 22.0f, .strong = true });
    ui::label(b, 0, { 24.0f, 50.0f, 700.0f, 20.0f },
              { .text = "L2 composite controls - functions, not classes; "
                        "behaviour declared, never hand-rolled" });
    ui::divider(b, 0, { 24.0f, 78.0f, 842.0f, 1.0f });

    // ---- buttons ---------------------------------------------------------
    {
        auto card = ui::panel(b, kPanelLeft, { 24.0f, 92.0f, 270.0f, 218.0f });
        ui::label(b, 0, { 40.0f, 102.0f, 200.0f, 20.0f },
                  { .text = "BUTTONS", .size = 11.0f, .strong = true });

        ui::button(b, kBtnPrimary, kBoxPrimary,
                   { .label = "Primary", .style = ui::ButtonStyle::Primary },
                   [&app] { ++app.clicks; app.ui->invalidate(); });
        ui::button(b, kBtnSecondary, kBoxSecondary,
                   { .label = "Secondary", .style = ui::ButtonStyle::Secondary },
                   [&app] { ++app.clicks; app.ui->invalidate(); });
        ui::button(b, kBtnGhost, kBoxGhost,
                   { .label = "Ghost", .style = ui::ButtonStyle::Ghost },
                   [&app] { ++app.clicks; app.ui->invalidate(); });
        ui::button(b, kBtnDanger, kBoxDanger,
                   { .label = "Danger", .style = ui::ButtonStyle::Danger },
                   [&app] { ++app.clicks; app.ui->invalidate(); });
        ui::button(b, kBtnDisabled, kBoxDisabled,
                   { .label = "Disabled", .style = ui::ButtonStyle::Primary,
                     .enabled = false },
                   {});
    }

    // ---- selection -------------------------------------------------------
    {
        auto card = ui::panel(b, kPanelMid, { 310.0f, 92.0f, 270.0f, 218.0f });
        ui::label(b, 0, { 326.0f, 102.0f, 200.0f, 20.0f },
                  { .text = "SELECTION", .size = 11.0f, .strong = true });

        ui::checkbox(b, kCheckA, kBoxCheckA,
                     { .label = "Snap to grid", .checked = app.checkA },
                     [&app](bool v) { app.checkA = v; app.ui->invalidate(); });
        ui::checkbox(b, kCheckB, kBoxCheckB,
                     { .label = "Show normals", .checked = app.checkB },
                     [&app](bool v) { app.checkB = v; app.ui->invalidate(); });

        for (int i = 0; i < kRadioCount; ++i) {
            ui::radio(b, kRadioBase + static_cast<ui::Key>(i), radioBox(i),
                      { .label = kRadios[i], .selected = (app.radio == i) },
                      [&app, i] { app.radio = i; app.ui->invalidate(); });
        }
    }

    // ---- values ----------------------------------------------------------
    {
        auto card = ui::panel(b, kPanelRight, { 596.0f, 92.0f, 270.0f, 218.0f });
        ui::label(b, 0, { 612.0f, 102.0f, 200.0f, 20.0f },
                  { .text = "VALUES", .size = 11.0f, .strong = true });

        ui::toggle(b, kSwitchA, kBoxSwitchA,
                   { .label = "Wireframe", .on = app.switchA },
                   [&app](bool v) { app.switchA = v; app.ui->invalidate(); });
        ui::toggle(b, kSwitchB, kBoxSwitchB,
                   { .label = "Disabled", .on = app.switchB, .enabled = false },
                   {});

        ui::slider(b, kSlider, kBoxSlider,
                   { .value = app.slider, .min = 0.0f, .max = 1.0f },
                   [&app](float v) { app.slider = v; app.ui->invalidate(); });

        char pct[32];
        std::snprintf(pct, sizeof pct, "LOD bias  %.0f%%", app.slider * 100.0f);
        ui::label(b, 0, { 612.0f, 244.0f, 238.0f, 18.0f }, { .text = pct });

        ui::progressBar(b, kProgress, { 612.0f, 270.0f, 238.0f, 8.0f },
                        { .value = app.progress });
        ui::label(b, 0, { 612.0f, 284.0f, 238.0f, 18.0f },
                  { .text = "Bake progress", .size = 12.0f });
    }

    // ---- tabs ------------------------------------------------------------
    {
        auto card = ui::panel(b, kPanelTabs, { 596.0f, 328.0f, 270.0f, 122.0f });
        ui::label(b, 0, { 612.0f, 338.0f, 200.0f, 20.0f },
                  { .text = "TABS", .size = 11.0f, .strong = true });

        ui::tabs(b, kTabsBase, kBoxTabs,
                 { .labels = kTabs, .count = kTabCount, .selected = app.tab },
                 [&app](int i) { app.tab = i; app.ui->invalidate(); });

        ui::label(b, 0, { 612.0f, 416.0f, 238.0f, 18.0f },
                  { .text = kTabs[std::clamp(app.tab, 0, kTabCount - 1)] });
    }

    // ---- text ------------------------------------------------------------
    {
        auto card = ui::panel(b, kPanelText, { 24.0f, 328.0f, 556.0f, 122.0f });
        ui::label(b, 0, { 40.0f, 338.0f, 200.0f, 20.0f },
                  { .text = "TEXT INPUT", .size = 11.0f, .strong = true });

        ui::textInput(b, kInput, kBoxInput,
                      { .placeholder = "Type here - Tab to focus",
                        .suffix = "UTF-8", .maxLength = 48 },
                      app.input,
                      [&app](const std::string&) { app.ui->invalidate(); });

        char info[96];
        std::snprintf(info, sizeof info, "%d chars - cursor %d",
                      static_cast<int>(app.input.text.size()), app.input.cursor);
        ui::label(b, 0, { 40.0f, 430.0f, 400.0f, 18.0f },
                  { .text = info, .size = 12.0f });
    }

    char status[160];
    std::snprintf(status, sizeof status,
                  "clicks %d - snap %s - mode %s - wireframe %s - tab %s",
                  app.clicks, app.checkA ? "on" : "off",
                  kRadios[std::clamp(app.radio, 0, kRadioCount - 1)],
                  app.switchA ? "on" : "off",
                  kTabs[std::clamp(app.tab, 0, kTabCount - 1)]);
    ui::label(b, 0, { 24.0f, 464.0f, 840.0f, 20.0f },
              { .text = status, .size = 12.0f });

    return b.root();
}

// ---------------------------------------------------------------------------
// Headless input harness
// ---------------------------------------------------------------------------

ui::InputEvent moveTo(float x, float y) {
    ui::InputEvent e;
    e.type = ui::InputEventType::MouseMove;
    e.x = x;
    e.y = y;
    return e;
}

ui::InputEvent mouse(ui::InputEventType type, float x, float y) {
    ui::InputEvent e;
    e.type = type;
    e.x = x;
    e.y = y;
    return e;
}

ui::InputEvent key(ui::KeyCode code, bool shift = false) {
    ui::InputEvent e;
    e.type  = ui::InputEventType::KeyDown;
    e.code  = code;
    e.shift = shift;
    return e;
}

ui::InputEvent typed(uint32_t cp) {
    ui::InputEvent e;
    e.type      = ui::InputEventType::KeyDown;
    e.code      = ui::KeyCode::Character;
    e.codepoint = cp;
    return e;
}

float centreX(const Rect& r) { return r.x + r.w * 0.5f; }
float centreY(const Rect& r) { return r.y + r.h * 0.5f; }

void click(ui::Ui& ui, const Rect& box) {
    ui.dispatchEvent(mouse(ui::InputEventType::MouseDown, centreX(box), centreY(box)));
    ui.dispatchEvent(mouse(ui::InputEventType::MouseUp, centreX(box), centreY(box)));
    ui.update();
}

int runInputTest(ui::Ui& ui, App& app) {
    ui.update();

    bool ok = true;
    auto check = [&ok](bool cond, const char* what) {
        std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
        if (!cond) ok = false;
    };

    std::printf("\n--- control layer self-test ---\n");

    // --- hover / click ----------------------------------------------------
    ui.dispatchEvent(moveTo(centreX(kBoxPrimary), centreY(kBoxPrimary)));
    check(ui.hoveredKey() == kBtnPrimary, "hover lands on the button");

    click(ui, kBoxPrimary);
    check(app.clicks == 1, "primary button click fires once");
    check(ui.focusedKey() == kBtnPrimary, "clicking a button focuses it");

    // --- disabled controls are inert --------------------------------------
    click(ui, kBoxDisabled);
    check(app.clicks == 1, "disabled button does not fire");
    check(ui.focusedKey() != kBtnDisabled, "disabled button is not focusable");

    // --- checkbox ---------------------------------------------------------
    click(ui, kBoxCheckA);
    check(!app.checkA, "checkbox toggles off");
    click(ui, kBoxCheckA);
    check(app.checkA, "checkbox toggles back on");

    // --- radio ------------------------------------------------------------
    const Rect r2 = radioBox(2);
    click(ui, r2);
    check(app.radio == 2, "radio selects the clicked option");

    // --- switch -----------------------------------------------------------
    click(ui, kBoxSwitchA);
    check(app.switchA, "switch turns on");
    click(ui, kBoxSwitchB);
    check(app.switchB, "disabled switch is inert");

    // --- slider: click to jump -------------------------------------------
    click(ui, kBoxSlider);
    check(std::fabs(app.slider - 0.5f) < 0.06f,
          "clicking a slider track jumps the thumb");

    // --- slider: capture, drag off the control, release -------------------
    const float trackL = kBoxSlider.x + 7.0f;
    const float trackR = kBoxSlider.right() - 7.0f;
    ui.dispatchEvent(mouse(ui::InputEventType::MouseDown, trackL, centreY(kBoxSlider)));
    check(ui.capturedKey() == kSlider, "slider takes the drag capture on press");

    ui.dispatchEvent(moveTo(trackL + 4.0f, 900.0f));    // off the control entirely
    ui.dispatchEvent(moveTo(trackR + 40.0f, 900.0f));   // past the right edge
    ui.update();
    check(app.slider > 0.99f, "drag past the end clamps to max");

    ui.dispatchEvent(mouse(ui::InputEventType::MouseUp, trackR + 40.0f, 900.0f));
    ui.update();
    check(ui.capturedKey() == ui::kNoKey, "release ends the capture");

    // --- text input -------------------------------------------------------
    click(ui, kBoxInput);
    check(ui.focusedKey() == kInput, "clicking the field focuses it");

    app.input.text.clear();
    app.input.moveTo(0);
    ui.dispatchEvent(typed('h'));
    ui.dispatchEvent(typed('i'));
    ui.dispatchEvent(typed(0x00E9));            // e-acute: two UTF-8 bytes
    ui.update();
    check(app.input.text == "hi\xC3\xA9", "typing inserts characters");
    check(app.input.cursor == 4, "cursor advances by byte length");

    ui.dispatchEvent(key(ui::KeyCode::Backspace));
    ui.update();
    check(app.input.text == "hi", "backspace deletes a whole codepoint");

    ui.dispatchEvent(key(ui::KeyCode::Home));
    check(app.input.cursor == 0, "Home moves to the start");
    ui.dispatchEvent(key(ui::KeyCode::End));
    check(app.input.cursor == 2, "End moves to the end");

    ui.dispatchEvent(key(ui::KeyCode::Left, true));     // shift+left selects
    check(app.input.hasSelection(), "shift+arrow extends a selection");
    ui.dispatchEvent(typed('X'));
    ui.update();
    check(app.input.text == "hX", "typing replaces the selection");

    // --- tab order --------------------------------------------------------
    ui.clearFocus();
    ui.dispatchEvent(key(ui::KeyCode::Tab));
    check(ui.focusedKey() == kBtnPrimary, "Tab focuses the first focusable node");
    ui.dispatchEvent(key(ui::KeyCode::Tab));
    check(ui.focusedKey() == kBtnSecondary, "Tab advances in tree order");
    ui.dispatchEvent(key(ui::KeyCode::Tab, true));
    check(ui.focusedKey() == kBtnPrimary, "Shift+Tab goes back");

    // --- keyboard activation ---------------------------------------------
    ui.dispatchEvent(key(ui::KeyCode::Space));
    ui.update();
    check(app.clicks == 2, "Space activates the focused button");

    // --- theme swap -------------------------------------------------------
    ui.setTheme(ui::lightTheme());
    ui.update();
    ui.setTheme(ui::darkTheme());
    ui.update();
    check(true, "theme can be swapped without disturbing the tree");

    std::printf("--- %s ---\n", ok ? "ALL PASS" : "FAILURES PRESENT");
    return ok ? 0 : 2;
}

void report(const App& app) {
    const auto& s = app.ui->stats();
    std::printf("rebuilds=%4llu  paints=%5llu  |  glyphs=%3llu quads=%4llu  "
                "|  liveOverrides=%u\n",
                static_cast<unsigned long long>(s.rebuilds),
                static_cast<unsigned long long>(s.paints),
                static_cast<unsigned long long>(s.glyphsRasterized),
                static_cast<unsigned long long>(s.glyphQuads),
                s.liveOverrides);
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
    app.input.text = "TacUI";
    app.input.moveTo(static_cast<int>(app.input.text.size()));

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

    if (inputTest) return runInputTest(ui, app);

    auto onFrame = [&app, capturePath](host::Control& ctl, double t) {
        if (t - app.lastReport >= 2.0) {
            app.lastReport = t;
            report(app);
        }
        if (capturePath && ctl.frame() + 1 >= 30) {
            ctl.captureNextFrame(capturePath);
        }
    };

    host::Options opts;
    opts.title      = "TacUI - Controls";
    opts.width      = 890;
    opts.height     = 500;
    opts.clear      = ui::darkTheme().background;
    opts.maxSeconds = maxSeconds;

    return host::run(ui, opts, onFrame);
}
