#include "tacui/controls.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "tacui/ui.hpp"

namespace tac::ui {

namespace {

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

Rect inset(const Rect& r, float d) {
    return Rect{ r.x + d, r.y + d, r.w - 2.0f * d, r.h - 2.0f * d };
}

Rect inflate(const Rect& r, float d) {
    return Rect{ r.x - d, r.y - d, r.w + 2.0f * d, r.h + 2.0f * d };
}

void rect(Builder& b, const Rect& r, float radius, const Color& c, Key key = kNoKey) {
    b.rect().box(r).radius(radius).color(c).key(key);
}

void text(Builder& b, const Rect& r, const char* s, float size, const Color& c,
          Key key = kNoKey) {
    if (!s || !*s) return;
    b.text(s).box(r).size(size).color(c).key(key);
}

// The box a line of text should occupy to sit centred in `box` vertically.
// Only `y` matters for the baseline, but carrying the real line height keeps
// the tree dump and the metrics honest.
Rect lineBox(const Ui& ui, const Rect& box, float size) {
    const float lineH = ui.textAscent(size) + ui.textDescent(size);
    return Rect{ box.x, box.y + (box.h - lineH) * 0.5f, box.w, lineH };
}

void textCentred(Builder& b, const Rect& box, const char* s, float size,
                 const Color& c) {
    if (!s || !*s) return;
    const float w = b.ui().measureText(s, size);
    Rect r = lineBox(b.ui(), box, size);
    r.x += (r.w - w) * 0.5f;
    r.w  = w > 0.0f ? w : r.w;
    text(b, r, s, size, c);
}

// The framework has no stroke primitive yet, so a 1px outline is a slightly
// larger rounded rect with the fill drawn on top of it. The key goes on the
// outer rect — which is also the visual extent — so the hit box and the drawn
// box stay the same object rather than two numbers that can drift.
void surface(Builder& b, const Rect& r, float radius, const Color& fill,
             const Color& border, Key key) {
    if (border.a > 0.0f) {
        rect(b, r, radius, border, key);
        rect(b, inset(r, 1.0f), std::max(radius - 1.0f, 0.0f), fill);
    } else {
        rect(b, r, radius, fill, key);
    }
}

// A keyed, fully transparent rect. Controls whose visuals are smaller than the
// row that should respond (checkbox, slider, text field) put their key on one
// of these so that clicking the label works too.
void hitRect(Builder& b, const Rect& r, Key key, float radius = 0.0f) {
    rect(b, r, radius, Color{ 0.0f, 0.0f, 0.0f, 0.0f }, key);
}

void focusRing(Builder& b, Key key, const Rect& r, float radius, bool enabled) {
    if (!enabled) return;
    if (!b.ui().isFocused(key)) return;
    rect(b, inflate(r, 2.0f), radius + 2.0f, b.ui().theme().focusRing);
}

// Registers a click target. `activates` also wires Space/Enter, which is what
// makes the tab order useful rather than decorative.
void clickTarget(Builder& b, Key key, ClickFn fn, bool enabled, bool focusable) {
    NodeBehavior nb;
    nb.interactive = enabled;
    nb.focusable   = enabled && focusable;
    if (enabled && fn) {
        nb.onClick = fn;
        nb.onKey   = [fn](const InputEvent& ev) {
            if (ev.code == KeyCode::Space || ev.code == KeyCode::Enter) fn();
        };
    }
    b.behavior(key, std::move(nb));
}

// ---------------------------------------------------------------------------
// UTF-8 helpers for TextInput. M0 shuffles bytes, not codepoints: the cursor is
// always kept on a codepoint edge, which is enough for the single-font,
// no-ligature text stack this release ships.
// ---------------------------------------------------------------------------

int prevCodepoint(const std::string& s, int i) {
    if (i <= 0) return 0;
    --i;
    while (i > 0 && (static_cast<unsigned char>(s[static_cast<size_t>(i)]) & 0xC0) == 0x80) {
        --i;
    }
    return i;
}

int nextCodepoint(const std::string& s, int i) {
    const int n = static_cast<int>(s.size());
    if (i >= n) return n;
    ++i;
    while (i < n && (static_cast<unsigned char>(s[static_cast<size_t>(i)]) & 0xC0) == 0x80) {
        ++i;
    }
    return i;
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Label
// ---------------------------------------------------------------------------

void label(Builder& b, Key key, Rect box, const LabelProps& p) {
    const Theme& t    = b.ui().theme();
    const float  size = p.size > 0.0f ? p.size : t.fontMd;
    const Color  col  = isSet(p.color) ? p.color : (p.strong ? t.textStrong : t.textDim);
    text(b, lineBox(b.ui(), box, size), p.text, size, col, key);
}

// ---------------------------------------------------------------------------
// Button
// ---------------------------------------------------------------------------

void button(Builder& b, Key key, Rect box, const ButtonProps& p, ClickFn onClick) {
    const Theme& t       = b.ui().theme();
    const Ui&    ui      = b.ui();
    const bool   enabled = p.enabled;
    const bool   hot     = enabled && ui.isHovered(key);
    const bool   down    = enabled && ui.isPressed(key);
    const float  radius  = p.radius > 0.0f ? p.radius : t.radiusMd;

    Color fill;
    Color border = kUnset;
    Color fg;

    switch (p.style) {
    case ButtonStyle::Primary:
        fill = (hot || down) ? t.accentHover : t.accent;
        fg   = t.textOnAccent;
        break;
    case ButtonStyle::Secondary:
        fill   = down ? t.surfaceActive : (hot ? t.surfaceHover : t.surface);
        border = hot ? t.borderStrong : t.border;
        fg     = t.textStrong;
        break;
    case ButtonStyle::Ghost:
        fill = down ? t.surfaceActive : (hot ? t.surfaceHover : kUnset);
        fg   = (hot || down) ? t.textStrong : t.textDim;
        break;
    case ButtonStyle::Danger:
        fill = hot || down ? Color::rgba8(240, 110, 110) : t.danger;
        fg   = t.textOnAccent;
        break;
    }

    if (!enabled) {
        fill   = t.surfaceAlt;
        border = t.border;
        fg     = t.textFaint;
    }

    focusRing(b, key, box, radius, enabled);
    surface(b, box, radius, fill, border, key);
    textCentred(b, box, p.label, t.fontMd, fg);

    clickTarget(b, key, std::move(onClick), enabled, true);
}

// ---------------------------------------------------------------------------
// Checkbox
// ---------------------------------------------------------------------------

void checkbox(Builder& b, Key key, Rect box, const CheckboxProps& p, BoolFn onChange) {
    const Theme& t       = b.ui().theme();
    const bool   enabled = p.enabled;
    const bool   hot     = enabled && b.ui().isHovered(key);
    const float  s       = 18.0f;

    const Rect indicator{ box.x, box.y + (box.h - s) * 0.5f, s, s };
    const Color border = p.checked ? t.accent : (hot ? t.borderStrong : t.border);

    focusRing(b, key, indicator, t.radiusSm, enabled);
    // The key sits on a rect covering the whole row, so the label is clickable
    // and the hit box matches the row the user sees.
    hitRect(b, box, key, t.radiusSm);
    surface(b, indicator, t.radiusSm, p.checked ? t.accent : t.surfaceAlt, border, kNoKey);
    if (p.checked) {
        // A filled box already reads as "on", so a missing check glyph
        // degrades to a filled box rather than to an empty one.
        textCentred(b, indicator, "\xE2\x9C\x93", t.fontSm, t.textOnAccent);
    }
    if (p.label && *p.label) {
        const Rect rest{ indicator.right() + t.spaceSm, box.y,
                         box.w - s - t.spaceSm, box.h };
        text(b, lineBox(b.ui(), rest, t.fontMd), p.label, t.fontMd,
             enabled ? (p.checked ? t.textStrong : t.textDim) : t.textFaint);
    }

    const bool checked = p.checked;
    clickTarget(b, key,
                enabled && onChange ? ClickFn{ [onChange, checked] { onChange(!checked); } }
                                    : ClickFn{},
                enabled, true);
}

// ---------------------------------------------------------------------------
// Radio
// ---------------------------------------------------------------------------

void radio(Builder& b, Key key, Rect box, const RadioProps& p, ClickFn onSelect) {
    const Theme& t       = b.ui().theme();
    const bool   enabled = p.enabled;
    const bool   hot     = enabled && b.ui().isHovered(key);
    const float  s       = 18.0f;

    const Rect indicator{ box.x, box.y + (box.h - s) * 0.5f, s, s };
    const float r = s * 0.5f;
    const Color border = p.selected ? t.accent : (hot ? t.borderStrong : t.border);

    focusRing(b, key, indicator, r, enabled);
    hitRect(b, box, key, t.radiusSm);
    surface(b, indicator, r, p.selected ? t.accent : t.surfaceAlt, border, kNoKey);
    if (p.selected) {
        const float d = r * 0.45f;
        rect(b, Rect{ indicator.x + r - d, indicator.y + r - d, d * 2.0f, d * 2.0f },
             d, t.textOnAccent);
    }
    if (p.label && *p.label) {
        const Rect rest{ indicator.right() + t.spaceSm, box.y,
                         box.w - s - t.spaceSm, box.h };
        text(b, lineBox(b.ui(), rest, t.fontMd), p.label, t.fontMd,
             enabled ? (p.selected ? t.textStrong : t.textDim) : t.textFaint);
    }

    clickTarget(b, key, std::move(onSelect), enabled, true);
}

// ---------------------------------------------------------------------------
// Switch
// ---------------------------------------------------------------------------

void toggle(Builder& b, Key key, Rect box, const SwitchProps& p, BoolFn onChange) {
    const Theme& t       = b.ui().theme();
    const bool   enabled = p.enabled;
    const bool   hot     = enabled && b.ui().isHovered(key);

    const float w = 40.0f;
    const float h = 22.0f;
    const Rect track{ box.x, box.y + (box.h - h) * 0.5f, w, h };
    const float r = h * 0.5f;

    focusRing(b, key, track, r, enabled);
    hitRect(b, box, key, r);
    surface(b, track, r, p.on ? t.accent : (hot ? t.surfaceHover : t.surfaceAlt),
            p.on ? t.accent : t.border, kNoKey);

    const float kr = r - 3.0f;
    const float kx = p.on ? track.right() - r : track.x + r;
    rect(b, Rect{ kx - kr, track.y + r - kr, kr * 2.0f, kr * 2.0f }, kr,
         p.on ? t.textOnAccent : t.textFaint);

    if (p.label && *p.label) {
        const Rect rest{ track.right() + t.spaceSm, box.y,
                         box.w - w - t.spaceSm, box.h };
        text(b, lineBox(b.ui(), rest, t.fontMd), p.label, t.fontMd,
             enabled ? t.textDim : t.textFaint);
    }

    const bool on = p.on;
    clickTarget(b, key,
                enabled && onChange ? ClickFn{ [onChange, on] { onChange(!on); } } : ClickFn{},
                enabled, true);
}

// ---------------------------------------------------------------------------
// Slider
// ---------------------------------------------------------------------------

void slider(Builder& b, Key key, Rect box, const SliderProps& p, FloatFn onChange) {
    const Theme& t       = b.ui().theme();
    const Ui&    ui      = b.ui();
    const bool   enabled = p.enabled;
    const bool   hot     = enabled && (ui.isHovered(key) || ui.isCaptured(key));

    const float knobR  = 7.0f;
    const float trackH = 6.0f;
    const float cy     = box.y + box.h * 0.5f;
    const Rect  track{ box.x + knobR, cy - trackH * 0.5f,
                       box.w - 2.0f * knobR, trackH };

    const float lo    = std::min(p.min, p.max);
    const float hi    = std::max(p.min, p.max);
    const float range = std::max(hi - lo, 1e-6f);
    const float t01   = clampf((p.value - lo) / range, 0.0f, 1.0f);

    // The key goes on a rect covering the whole box: a slider should respond to
    // a click anywhere on its row, not only on the 6px track.
    hitRect(b, box, key, t.radiusSm);
    rect(b, track, trackH * 0.5f, t.surfaceActive);
    if (t01 > 0.0f) {
        rect(b, Rect{ track.x, track.y, track.w * t01, track.h }, trackH * 0.5f,
             enabled ? t.accent : t.textFaint);
    }

    const float kx = track.x + track.w * t01;
    const Rect  knob{ kx - knobR, cy - knobR, knobR * 2.0f, knobR * 2.0f };
    if (ui.isFocused(key) && enabled) {
        rect(b, inflate(knob, 2.0f), knobR + 2.0f, t.focusRing);
    }
    rect(b, knob, knobR, !enabled ? t.textFaint : (hot ? t.accentHover : t.accent));

    // Drag and arrow keys are one behaviour: assigning b.behavior twice for the
    // same key would drop whichever came first.
    NodeBehavior nb;
    nb.interactive = enabled;
    nb.focusable   = enabled;
    if (enabled && onChange) {
        nb.onDrag = [track, lo, hi, onChange](float x, float) {
            const float local = clampf((x - track.x) / std::max(track.w, 1e-6f), 0.0f, 1.0f);
            onChange(lo + local * (hi - lo));
        };

        // Arrow keys nudge by `step`, or by 1% of the range when none is given.
        const float step = p.step > 0.0f ? p.step : range * 0.01f;
        const float value = p.value;
        nb.onKey = [onChange, value, step, lo, hi](const InputEvent& ev) {
            float next = value;
            if (ev.code == KeyCode::Left || ev.code == KeyCode::Down) next -= step;
            else if (ev.code == KeyCode::Right || ev.code == KeyCode::Up) next += step;
            else return;
            onChange(clampf(next, lo, hi));
        };
    }
    b.behavior(key, std::move(nb));
}

// ---------------------------------------------------------------------------
// Progress bar
// ---------------------------------------------------------------------------

void progressBar(Builder& b, Key key, Rect box, const ProgressProps& p) {
    const Theme& t   = b.ui().theme();
    const float  r   = std::min(box.h * 0.5f, t.radiusSm);
    const float  t01 = clampf(p.value, 0.0f, 1.0f);
    const Color  c   = isSet(p.color) ? p.color : t.accent;

    rect(b, box, r, t.surfaceActive, key);
    if (t01 > 0.0f) {
        rect(b, Rect{ box.x, box.y, box.w * t01, box.h }, r, c);
    }
}

// ---------------------------------------------------------------------------
// Divider
// ---------------------------------------------------------------------------

void divider(Builder& b, Key key, Rect box, const DividerProps& p) {
    const Theme& t = b.ui().theme();
    const Color  c = isSet(p.color) ? p.color : t.border;
    rect(b, box, 0.0f, c, key);
}

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------

Scope panel(Builder& b, Key key, Rect box, const PanelProps& p) {
    const Theme& t      = b.ui().theme();
    const float  radius = p.radius >= 0.0f ? p.radius : t.radiusMd;
    const Color  fill   = isSet(p.fill) ? p.fill : t.surface;

    auto scope = b.stack(key);
    if (p.border) {
        surface(b, box, radius, fill, t.border, key);
    } else {
        rect(b, box, radius, fill, key);
    }
    return scope;
}

// ---------------------------------------------------------------------------
// List item
// ---------------------------------------------------------------------------

void listItem(Builder& b, Key key, Rect box, const ListItemProps& p, ClickFn onClick) {
    const Theme& t       = b.ui().theme();
    const Ui&    ui      = b.ui();
    const bool   enabled = p.enabled;
    const bool   hot     = enabled && ui.isHovered(key);
    const float  radius  = t.radiusSm;

    Color bg;
    if (!enabled)        bg = t.surfaceAlt;
    else if (p.selected) bg = t.accentSoft;
    else if (hot)        bg = t.surfaceHover;
    else                 bg = t.surfaceAlt;

    focusRing(b, key, box, radius, enabled);
    rect(b, box, radius, bg, key);

    const float size = p.labelSize > 0.0f ? p.labelSize : t.fontMd;
    const float pad  = t.spaceMd;

    Rect labelBox{ box.x + pad, box.y, box.w - pad * 2.0f, box.h };

    float secondaryW = 0.0f;
    if (p.secondary && *p.secondary) {
        secondaryW = ui.measureText(p.secondary, t.fontSm) + t.spaceMd;
        Rect sBox = lineBox(ui, Rect{ box.right() - pad - secondaryW, box.y,
                                      secondaryW, box.h }, t.fontSm);
        text(b, sBox, p.secondary, t.fontSm,
             enabled ? t.textFaint : t.textFaint);
        labelBox.w -= secondaryW;
    }

    text(b, lineBox(ui, labelBox, size), p.label, size,
         enabled ? (p.selected ? t.textStrong : t.textDim) : t.textFaint);

    clickTarget(b, key, std::move(onClick), enabled, true);
}

// ---------------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------------

void tabs(Builder& b, Key firstKey, Rect box, const TabsProps& p, IndexFn onSelect) {
    if (!p.labels || p.count <= 0) return;

    const Theme& t     = b.ui().theme();
    const Ui&    ui    = b.ui();
    const float  segW  = box.w / static_cast<float>(p.count);
    const float  radius = t.radiusSm;

    rect(b, box, radius, t.surfaceAlt);

    for (int i = 0; i < p.count; ++i) {
        const Key  k   = firstKey + static_cast<Key>(i);
        const bool sel = (i == p.selected);
        const bool hot = ui.isHovered(k);

        const Rect seg{ box.x + segW * static_cast<float>(i), box.y, segW, box.h };
        // Always emit a keyed rect, even when it is invisible, so every segment
        // is hit-testable regardless of whether it is the selected one.
        rect(b, seg, radius, sel ? t.accent : (hot ? t.surfaceHover : kUnset), k);
        textCentred(b, seg, p.labels[i], t.fontMd,
                    sel ? t.textOnAccent : (hot ? t.textStrong : t.textDim));

        clickTarget(b, k,
                    onSelect ? ClickFn{ [onSelect, i] { onSelect(i); } } : ClickFn{},
                    true, true);
    }
}

// ---------------------------------------------------------------------------
// Text input
// ---------------------------------------------------------------------------

void textInput(Builder& b, Key key, Rect box, const TextInputProps& p,
               TextInputState& state, TextFn onChange) {
    const Theme& t       = b.ui().theme();
    Ui&          ui      = b.ui();
    Ui*          uiPtr   = &ui;
    const bool   enabled = p.enabled;
    const bool   focused = enabled && ui.isFocused(key);
    const float  size    = p.size > 0.0f ? p.size : t.fontMd;
    const float  pad     = t.spaceSm + 2.0f;
    const float  radius  = t.radiusSm;

    // Clamp defensively: the host owns the buffer and may have replaced it.
    const int len = static_cast<int>(state.text.size());
    state.cursor = std::clamp(state.cursor, 0, len);
    state.anchor = std::clamp(state.anchor, 0, len);

    const Rect field{ box.x, box.y, box.w, box.h };
    const Rect inner{ field.x + pad, field.y, field.w - pad * 2.0f, field.h };

    focusRing(b, key, field, radius, enabled);
    surface(b, field, radius, enabled ? t.surfaceAlt : t.surface,
            focused ? t.accent : t.border, key);

    float suffixW = 0.0f;
    if (p.suffix && *p.suffix) {
        suffixW = ui.measureText(p.suffix, t.fontSm) + t.spaceSm;
        text(b, lineBox(ui, Rect{ field.right() - pad - suffixW, field.y, suffixW, field.h },
                        t.fontSm),
             p.suffix, t.fontSm, t.textFaint);
    }

    Rect textArea = inner;
    textArea.w -= suffixW;

    const bool empty = state.text.empty();
    const char* shown = empty ? p.placeholder : state.text.c_str();
    const Color shownColor = empty ? t.textFaint : (enabled ? t.textStrong : t.textDim);

    if (focused && state.hasSelection()) {
        const int  a = state.selectionMin();
        const int  bIdx = state.selectionMax();
        const float x0 = ui.measureText(std::string(state.text, 0, static_cast<size_t>(a)).c_str(), size);
        const float x1 = ui.measureText(std::string(state.text, 0, static_cast<size_t>(bIdx)).c_str(), size);
        Rect sel = lineBox(ui, textArea, size);
        sel.x += x0;
        sel.w  = std::max(x1 - x0, 1.0f);
        rect(b, sel, 2.0f, t.accentSoft);
    }

    text(b, lineBox(ui, textArea, size), shown, size, shownColor);

    if (focused) {
        const float caretX =
            textArea.x + ui.measureText(
                             std::string(state.text, 0, static_cast<size_t>(state.cursor)).c_str(),
                             size);
        rect(b, Rect{ caretX, field.y + 5.0f, 1.0f, field.h - 10.0f }, 0.0f, t.accent);
    }

    NodeBehavior nb;
    nb.interactive = enabled;
    nb.focusable   = enabled;
    nb.onKey       = [&state, onChange, maxLength = p.maxLength, uiPtr](const InputEvent& ev) {
        bool changed = false;
        bool moved   = false;

        auto eraseSelection = [&state]() {
            if (!state.hasSelection()) return false;
            const int a = state.selectionMin();
            const int b = state.selectionMax();
            state.text.erase(static_cast<size_t>(a), static_cast<size_t>(b - a));
            state.cursor = a;
            state.anchor = a;
            return true;
        };

        switch (ev.code) {
        case KeyCode::Character: {
            if (ev.codepoint == 0) break;
            eraseSelection();
            if (maxLength > 0 && static_cast<int>(state.text.size()) >= maxLength) break;
            std::string ins;
            appendUtf8(ins, ev.codepoint);
            state.text.insert(static_cast<size_t>(state.cursor), ins);
            state.cursor += static_cast<int>(ins.size());
            state.anchor = state.cursor;
            changed = true;
            break;
        }

        case KeyCode::Backspace:
            if (!eraseSelection() && state.cursor > 0) {
                const int prev = prevCodepoint(state.text, state.cursor);
                state.text.erase(static_cast<size_t>(prev),
                                 static_cast<size_t>(state.cursor - prev));
                state.cursor = prev;
                state.anchor = prev;
                changed = true;
            }
            break;

        case KeyCode::Delete:
            if (!eraseSelection() && state.cursor < static_cast<int>(state.text.size())) {
                const int next = nextCodepoint(state.text, state.cursor);
                state.text.erase(static_cast<size_t>(state.cursor),
                                 static_cast<size_t>(next - state.cursor));
                changed = true;
            }
            break;

        case KeyCode::Left:
            if (state.hasSelection() && !ev.shift) {
                state.cursor = state.anchor = state.selectionMin();
            } else {
                state.cursor = prevCodepoint(state.text, state.cursor);
                if (!ev.shift) state.anchor = state.cursor;
            }
            moved = true;
            break;

        case KeyCode::Right:
            if (state.hasSelection() && !ev.shift) {
                state.cursor = state.anchor = state.selectionMax();
            } else {
                state.cursor = nextCodepoint(state.text, state.cursor);
                if (!ev.shift) state.anchor = state.cursor;
            }
            moved = true;
            break;

        case KeyCode::Home:
            state.cursor = 0;
            if (!ev.shift) state.anchor = 0;
            moved = true;
            break;

        case KeyCode::End:
            state.cursor = static_cast<int>(state.text.size());
            if (!ev.shift) state.anchor = state.cursor;
            moved = true;
            break;

        default:
            break;
        }

        if (changed && onChange) onChange(state.text);
        // The caret is part of the picture, so a pure cursor move still has to
        // rebuild — this is base-property territory, not an override.
        if (changed || moved) uiPtr->invalidate();
    };
    b.behavior(key, std::move(nb));
}

} // namespace tac::ui
