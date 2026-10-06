#include "tacui/theme.hpp"

namespace tac::ui {

const Theme& darkTheme() {
    static const Theme t{};
    return t;
}

const Theme& lightTheme() {
    static const Theme t = [] {
        Theme l;
        l.background    = Color::rgba8(246, 248, 252);
        l.surface       = Color::rgba8(255, 255, 255);
        l.surfaceAlt    = Color::rgba8(240, 243, 249);
        l.surfaceHover  = Color::rgba8(232, 238, 248);
        l.surfaceActive = Color::rgba8(220, 229, 243);

        l.textStrong   = Color::rgba8(15, 23, 42);
        l.textDim      = Color::rgba8(71, 85, 105);
        l.textFaint    = Color::rgba8(148, 163, 184);
        l.textOnAccent = Color::rgba8(255, 255, 255);

        l.border       = Color::rgba8(214, 222, 235);
        l.borderStrong = Color::rgba8(186, 197, 214);
        l.accent       = Color::rgba8(37, 99, 235);
        l.accentHover  = Color::rgba8(29, 78, 216);
        l.accentSoft   = Color::rgba8(219, 232, 255);
        l.danger       = Color::rgba8(220, 38, 38);
        l.success      = Color::rgba8(16, 158, 106);
        l.focusRing    = Color::rgba8(59, 130, 246);
        return l;
    }();
    return t;
}

} // namespace tac::ui
