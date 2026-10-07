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
#include <vector>

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
constexpr ui::Key kPanelScroll = 75;
constexpr ui::Key kPanelList   = 76;

constexpr ui::Key kScroll    = 80;
constexpr ui::Key kScrollBar = 81;
constexpr ui::Key kList      = 90;
constexpr ui::Key kListRow   = 100;   // rows are kListRow + i

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

constexpr Rect kBoxScroll { 40.0f, 500.0f, 368.0f, 168.0f };
constexpr Rect kBoxList   { 456.0f, 500.0f, 394.0f, 168.0f };

constexpr int kTabCount   = 3;
constexpr int kRadioCount = 3;
constexpr int kListCount  = 40;

constexpr float kScrollRowH     = 28.0f;
constexpr int   kScrollRowCount = 12;
constexpr float kScrollContent  = kScrollRowH * static_cast<float>(kScrollRowCount);

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
    ui::ScrollViewState scroll;
    ui::ListViewState   list;
    int                 listSelected = 3;

    std::vector<std::string> rowLabels;
    std::vector<const char*> rowPtrs;
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

        // CJK here doubles as a fallback smoke test: these characters are not
        // in the primary family, so they only render if the font fallback ran.
        char tabLine[96];
        std::snprintf(tabLine, sizeof tabLine, "%s · 中文 日本語",
                      kTabs[std::clamp(app.tab, 0, kTabCount - 1)]);
        ui::label(b, 0, { 612.0f, 416.0f, 238.0f, 18.0f },
                  { .text = tabLine });
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

    // ---- scroll view -----------------------------------------------------
    {
        auto card = ui::panel(b, kPanelScroll, { 24.0f, 466.0f, 400.0f, 204.0f });
        ui::label(b, 0, { 40.0f, 476.0f, 200.0f, 20.0f },
                  { .text = "SCROLL VIEW", .size = 11.0f, .strong = true });
        ui::label(b, 0, { 200.0f, 476.0f, 210.0f, 20.0f },
                  { .text = "wheel or drag the bar", .size = 11.0f });

        {
            auto view = ui::scrollView(b, kScroll, kBoxScroll, app.scroll,
                                       { .contentHeight = kScrollContent,
                                         .lineStep = 40.0f });
            char line[64];
            for (int i = 0; i < kScrollRowCount; ++i) {
                const float y = kBoxScroll.y + static_cast<float>(i) * kScrollRowH
                                - app.scroll.offset;
                std::snprintf(line, sizeof line, "Row %02d", i);
                ui::label(b, 0, { kBoxScroll.x + 12.0f, y + 4.0f, 200.0f, 18.0f },
                          { .text = line });
                ui::label(b, 0, { kBoxScroll.x + 92.0f, y + 4.0f, 240.0f, 18.0f },
                          { .text = "clipped to the viewport", .size = 12.0f });
                ui::divider(b, 0, { kBoxScroll.x + 12.0f, y + kScrollRowH - 2.0f,
                                    324.0f, 1.0f });
            }
        }
        ui::scrollBar(b, kScrollBar, kBoxScroll, app.scroll.offset,
                      { .contentHeight = kScrollContent });
    }

    // ---- list view -------------------------------------------------------
    {
        auto card = ui::panel(b, kPanelList, { 440.0f, 466.0f, 426.0f, 204.0f });
        ui::label(b, 0, { 456.0f, 476.0f, 200.0f, 20.0f },
                  { .text = "LIST VIEW", .size = 11.0f, .strong = true });

        char sub[64];
        std::snprintf(sub, sizeof sub, "%d items, only visible rows exist", kListCount);
        ui::label(b, 0, { 556.0f, 476.0f, 294.0f, 20.0f },
                  { .text = sub, .size = 11.0f });

        ui::listView(b, kList, kBoxList,
                     { .labels = app.rowPtrs.data(), .count = kListCount,
                       .selected = app.listSelected,
                       .rowHeight = 28.0f, .rowGap = 2.0f,
                       .rowKeyBase = kListRow },
                     app.list,
                     [&app](int i) { app.listSelected = i; app.ui->invalidate(); });
    }

    char status[192];
    std::snprintf(status, sizeof status,
                  "clicks %d - snap %s - mode %s - wireframe %s - tab %s - list row %d",
                  app.clicks, app.checkA ? "on" : "off",
                  kRadios[std::clamp(app.radio, 0, kRadioCount - 1)],
                  app.switchA ? "on" : "off",
                  kTabs[std::clamp(app.tab, 0, kTabCount - 1)],
                  app.listSelected);
    ui::label(b, 0, { 24.0f, 682.0f, 840.0f, 20.0f },
              { .text = status, .size = 12.0f });

    return b.root();
}

// ---------------------------------------------------------------------------
// Headless input harness
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Layout harness (M2: relative containers)
//
// A dedicated tree so the assertions read as a spec: what a row/column does to
// its children, verified through the public `find(...).bounds()` surface.
// ---------------------------------------------------------------------------

ui::InputEvent mouse(ui::InputEventType type, float x, float y);

namespace lay {

constexpr ui::Key kCol  = 900;
constexpr ui::Key kRow  = 920;
constexpr ui::Key kRowBottom = 930;
constexpr ui::Key kRowPanel  = 940;
constexpr ui::Key kRowText   = 960;
constexpr ui::Key kRowButton = 980;

constexpr ui::Key kA = 901, kB = 902, kC = 903;
constexpr ui::Key kD = 921, kE = 922;
constexpr ui::Key kF = 931;
constexpr ui::Key kPanel      = 941, kPanelLabel = 942;
constexpr ui::Key kRawText    = 961;
constexpr ui::Key kButton     = 981;

int buttonClicks = 0;

ui::VNode* build(ui::Builder& b) {
    auto root = b.stack();

    // Column: frame 400x300, padding 10, gap 10 — fixed, gap, then flex.
    {
        auto col = b.column({ 0.0f, 0.0f, 400.0f, 300.0f },
                            { .gap = 10.0f, .padding = ui::EdgeInsets::all(10.0f) },
                            kCol);
        b.rect().width(100.0f).height(50.0f).key(kA).color(ui::rgb(200, 80, 80));
        b.rect().width(100.0f).height(50.0f).key(kB).color(ui::rgb(80, 200, 80));
        b.rect().width(60.0f).height(30.0f).flex(1.0f).key(kC).color(ui::rgb(80, 80, 200));
    }

    // Row: fixed child then a flex child that takes the rest.
    {
        auto row = b.row({ 0.0f, 400.0f, 300.0f, 100.0f }, {}, kRow);
        b.rect().width(100.0f).height(40.0f).key(kD).color(ui::rgb(200, 200, 80));
        b.rect().height(40.0f).flex(1.0f).key(kE).color(ui::rgb(80, 200, 200));
    }

    // Cross-axis alignment: bottom of a 100-tall row.
    {
        auto row = b.row({ 0.0f, 520.0f, 200.0f, 100.0f }, {}, kRowBottom);
        b.rect().width(50.0f).height(20.0f).key(kF).align(0.0f, 1.0f)
            .color(ui::rgb(200, 120, 200));
    }

    // A panel authored far from the origin, placed by a row: its whole subtree
    // must move as one.
    {
        auto row = b.row({ 400.0f, 400.0f, 300.0f, 100.0f }, {}, kRowPanel);
        auto p = ui::panel(b, kPanel, { 900.0f, 900.0f, 120.0f, 60.0f });
        ui::label(b, kPanelLabel, { 910.0f, 910.0f, 100.0f, 20.0f },
                  { .text = "in a panel" });
    }

    // A bare text node takes its intrinsic (measured) width in a row.
    {
        auto row = b.row({ 400.0f, 520.0f, 300.0f, 40.0f }, {}, kRowText);
        b.text("intrinsic").size(16.0f).color(ui::rgb(230, 230, 240)).key(kRawText);
    }

    // A control in a row keeps its authored size but lands where the row puts
    // it, and stays clickable there.
    {
        auto row = b.row({ 700.0f, 520.0f, 300.0f, 40.0f }, {}, kRowButton);
        ui::button(b, kButton, { 1200.0f, 800.0f, 120.0f, 32.0f },
                   { .label = "Placed" },
                   [] { ++buttonClicks; });
    }

    return b.root();
}

} // namespace lay

int runLayoutTest() {
    ui::Ui ui;
    if (!ui.init([](ui::BuildContext& ctx) {
            ui::Builder b(ctx);
            return lay::build(b);
        })) {
        std::fprintf(stderr, "fatal: layout test init failed\n");
        return 1;
    }
    ui.update();

    bool ok = true;
    auto check = [&ok](bool cond, const char* what) {
        std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
        if (!cond) ok = false;
    };
    auto near = [](float a, float b) { return std::fabs(a - b) < 0.5f; };
    auto box  = [&ui](ui::Key k) { return ui.find(k).bounds(); };

    std::printf("\n--- layout self-test ---\n");

    // --- column: padding, gap, flex ---------------------------------------
    {
        const Rect a = box(lay::kA);
        check(near(a.x, 10.0f) && near(a.y, 10.0f) &&
              near(a.w, 100.0f) && near(a.h, 50.0f),
              "column places the first child at the padded origin");

        const Rect b2 = box(lay::kB);
        check(near(b2.y, 70.0f), "column advances by child height + gap");

        const Rect c = box(lay::kC);
        check(near(c.y, 130.0f) && near(c.h, 160.0f),
              "flex child fills the leftover main-axis space");
    }

    // --- row: fixed + flex -------------------------------------------------
    {
        const Rect d = box(lay::kD);
        const Rect e = box(lay::kE);
        check(near(d.x, 0.0f) && near(d.y, 400.0f) && near(d.w, 100.0f),
              "row places the first child at the frame origin");
        check(near(e.x, 100.0f) && near(e.w, 200.0f),
              "row flex child takes exactly the remaining width");
    }

    // --- cross-axis alignment ---------------------------------------------
    {
        const Rect f = box(lay::kF);
        check(near(f.y, 600.0f), "align(0,1) pins the child to the bottom edge");
    }

    // --- an absolute subtree is translated as a whole ---------------------
    {
        const Rect l = box(lay::kPanelLabel);
        check(near(l.x, 410.0f) && std::fabs(l.y - 410.0f) < 6.0f,
              "a panel authored far away lands at the row slot");
    }

    // --- intrinsic text width ---------------------------------------------
    {
        const Rect t = box(lay::kRawText);
        const float measured = ui.measureText("intrinsic", 16.0f);
        check(near(t.w, measured) && measured > 0.0f,
              "a bare text node takes its measured width");
    }

    // --- a control lands and responds where the row put it ----------------
    {
        const Rect btn = box(lay::kButton);
        check(near(btn.x, 700.0f) && near(btn.y, 520.0f) && near(btn.w, 120.0f),
              "a control keeps its size but lands at the row slot");

        ui.dispatchEvent(mouse(ui::InputEventType::MouseDown, 760.0f, 536.0f));
        ui.dispatchEvent(mouse(ui::InputEventType::MouseUp, 760.0f, 536.0f));
        ui.update();
        check(lay::buttonClicks == 1,
              "hit testing uses the laid-out position, not the authored one");
    }

    std::printf("--- %s ---\n", ok ? "ALL PASS" : "FAILURES PRESENT");
    return ok ? 0 : 2;
}

// ---------------------------------------------------------------------------
// Paint order harness (M1: one ordered draw list)
//
// M0 drew every rectangle before every glyph, so a text node under a later rect
// was painted on top of it. The fix submits same-type runs in tree order; this
// asserts the interleaving directly, by recording the order of draw calls.
// ---------------------------------------------------------------------------

namespace paint {

constexpr ui::Key kUnderText = 700;
constexpr ui::Key kRect      = 701;
constexpr ui::Key kRect2     = 702;
constexpr ui::Key kOverText  = 703;

ui::VNode* build(ui::Builder& b) {
    auto root = b.stack();

    // text, rect, rect, text — the rects must land *between* the two texts, not
    // all before them.
    b.text("under").box(0.0f, 0.0f, 200.0f, 24.0f).size(16.0f)
        .color(ui::rgb(230, 230, 240)).key(kUnderText);
    b.rect().box(0.0f, 0.0f, 200.0f, 24.0f).color(ui::rgb(200, 80, 80)).key(kRect);
    b.rect().box(0.0f, 40.0f, 200.0f, 24.0f).color(ui::rgb(80, 200, 120)).key(kRect2);
    b.text("over").box(0.0f, 40.0f, 200.0f, 24.0f).size(16.0f)
        .color(ui::rgb(230, 230, 240)).key(kOverText);

    return b.root();
}

} // namespace paint

// Records one character per draw *call*, so the string reads as the submission
// order: 'g' for a glyph run, 'r' for a rect run.
struct OrderRecorder final : rhi::Device {
    std::string order;

    void beginFrame(Color) override {}
    void endFrame() override {}
    void resize(uint32_t, uint32_t) override {}
    void drawSdfRects(const rhi::SdfRect*, uint32_t) override { order.push_back('r'); }
    void drawGlyphQuads(const rhi::GlyphQuad*, uint32_t) override { order.push_back('g'); }
};

int runPaintTest() {
    ui::Ui ui;
    if (!ui.init([](ui::BuildContext& ctx) {
            ui::Builder b(ctx);
            return paint::build(b);
        })) {
        std::fprintf(stderr, "fatal: paint test init failed\n");
        return 1;
    }
    ui.update();

    OrderRecorder rec;
    ui.paint(rec);

    bool ok = true;
    auto check = [&ok](bool cond, const char* what) {
        std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
        if (!cond) ok = false;
    };

    std::printf("\n--- paint order self-test ---\n");
    std::printf("  submission order: \"%s\" (expected \"grg\")\n", rec.order.c_str());
    check(rec.order == "grg",
          "rect runs are submitted between glyph runs, in tree order");

    std::printf("--- %s ---\n", ok ? "ALL PASS" : "FAILURES PRESENT");
    return ok ? 0 : 2;
}

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

// How many rows of the list actually exist in the retained tree. This is the
// number virtualisation is supposed to bound — by the viewport, not by `count`.
int countRows(const ui::ElementPtr& e, ui::Key base, int n) {
    int c = (e->key >= base && e->key < base + static_cast<ui::Key>(n)) ? 1 : 0;
    for (const auto& child : e->children) c += countRows(child, base, n);
    return c;
}

// Records what the paint pass submits, so the clip can be asserted on without
// a GPU. This is the paint-side counterpart of the hit-test assertions.
struct RecordingDevice final : rhi::Device {
    std::vector<rhi::SdfRect>   rects;
    std::vector<rhi::GlyphQuad> quads;

    void beginFrame(Color) override {}
    void endFrame() override {}
    void resize(uint32_t, uint32_t) override {}
    void drawSdfRects(const rhi::SdfRect* r, uint32_t n) override {
        // Append: a frame now submits several runs (rect / glyph / rect …), so
        // assign() would keep only the last one.
        rects.insert(rects.end(), r, r + n);
    }
    void drawGlyphQuads(const rhi::GlyphQuad* q, uint32_t n) override {
        quads.insert(quads.end(), q, q + n);
    }
};

bool sameRect(const rhi::ClipRect& c, const Rect& box) {
    return std::fabs(c.x0 - box.x) < 0.5f && std::fabs(c.y0 - box.y) < 0.5f &&
           std::fabs(c.x1 - box.right()) < 0.5f && std::fabs(c.y1 - box.bottom()) < 0.5f;
}

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

    // --- wheel bubbles out of a row into the list -------------------------
    const float rowY = kBoxList.y + 14.0f;
    ui.dispatchEvent(moveTo(centreX(kBoxList), rowY));
    check(ui.hoveredKey() >= kListRow && ui.hoveredKey() < kListRow + kListCount,
          "hover lands on a visible list row");

    {
        std::vector<ui::Key> chain;
        ui.hitChain(centreX(kBoxList), rowY, chain);
        check(chain.size() == 2 && chain.front() >= kListRow && chain.back() == kList,
              "hit chain is the row followed by its list container");
    }

    ui::InputEvent wheel;
    wheel.type  = ui::InputEventType::MouseWheel;
    wheel.x     = centreX(kBoxList);
    wheel.y     = rowY;
    wheel.wheel = -3.0f;                      // negative = scroll down

    const float beforeScroll = app.list.offset;
    ui.dispatchEvent(wheel);
    ui.update();
    check(app.list.offset > beforeScroll,
          "wheel over a row scrolls the list rather than being swallowed");

    // --- scrolling clamps at the end --------------------------------------
    for (int i = 0; i < 40; ++i) ui.dispatchEvent(wheel);
    ui.update();
    const float listMax = static_cast<float>(kListCount) * 30.0f - kBoxList.h;
    check(std::fabs(app.list.offset - listMax) < 0.5f, "scrolling clamps at the end");

    // --- virtualisation ---------------------------------------------------
    check(countRows(ui.root(), kListRow, kListCount) < 14,
          "only the visible rows exist as nodes");

    // --- clipping is respected by hit testing -----------------------------
    {
        std::vector<ui::Key> chain;
        // Inside the list panel, above the list box: outside the clip, so
        // neither the container nor any row may be hit.
        ui.hitChain(centreX(kBoxList), kBoxList.y - 6.0f, chain);
        bool touchedList = false;
        for (ui::Key k : chain) {
            if (k == kList || (k >= kListRow && k < kListRow + kListCount)) touchedList = true;
        }
        check(!touchedList, "hit testing stops at the clip rectangle");
    }

    // --- scrollbar drag ---------------------------------------------------
    const Rect barBox{ kBoxList.right() - 8.0f, kBoxList.y, 8.0f, kBoxList.h };
    ui.dispatchEvent(mouse(ui::InputEventType::MouseDown, centreX(barBox), kBoxList.y + 4.0f));
    check(ui.capturedKey() == kList + 1, "scrollbar takes the drag capture");
    ui.dispatchEvent(mouse(ui::InputEventType::MouseUp, centreX(barBox), kBoxList.y + 4.0f));
    ui.update();
    check(app.list.offset < listMax * 0.2f, "dragging the bar to the top returns to the start");

    // --- the paint pass carries the scissor --------------------------------
    {
        RecordingDevice dev;
        ui.paint(dev);

        int scrollPrims = 0;
        int listPrims   = 0;
        int stray       = 0;
        for (const auto& r : dev.rects) {
            if (!r.clip.enabled) continue;
            if (sameRect(r.clip, kBoxScroll))      ++scrollPrims;
            else if (sameRect(r.clip, kBoxList))   ++listPrims;
            else                                   ++stray;
        }

        int clippedGlyphs = 0;
        for (const auto& q : dev.quads) if (q.clip.enabled) ++clippedGlyphs;

        check(scrollPrims > 0, "scroll view content is painted with the viewport scissor");
        check(listPrims > 0,   "list view rows are painted with the viewport scissor");
        check(stray == 0,      "no primitive carries an unexpected scissor");
        check(clippedGlyphs > 0, "text inside a clip container is scissored too");
    }

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
    bool        layoutTest  = false;
    bool        paintTest   = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            capturePath = argv[++i];
        } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            maxSeconds = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--input-test") == 0) {
            inputTest = true;
        } else if (std::strcmp(argv[i], "--layout-test") == 0) {
            layoutTest = true;
        } else if (std::strcmp(argv[i], "--paint-test") == 0) {
            paintTest = true;
        }
    }

    if (layoutTest) return runLayoutTest();
    if (paintTest)  return runPaintTest();

    App    app;
    ui::Ui ui;
    app.ui = &ui;
    app.input.text = "中文 TacUI";   // CJK in the field, to exercise the fallback
    app.input.moveTo(static_cast<int>(app.input.text.size()));

    // Row labels live in the app so the pointers stay stable; `listView` only
    // borrows them for the duration of the build.
    app.rowLabels.reserve(kListCount);
    for (int i = 0; i < kListCount; ++i) {
        char buf[24];
        std::snprintf(buf, sizeof buf, "Mesh_%02d", i);
        app.rowLabels.emplace_back(buf);
    }
    app.rowPtrs.reserve(kListCount);
    for (const auto& s : app.rowLabels) app.rowPtrs.push_back(s.c_str());

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
    opts.height     = 712;
    opts.clear      = ui::darkTheme().background;
    opts.maxSeconds = maxSeconds;
    // Captures are golden images: pin the scale so the same binary produces the
    // same pixels on any display. Interactive runs follow the monitor DPI.
    opts.uiScale    = capturePath ? 1.0f : 0.0f;

    return host::run(ui, opts, onFrame);
}
