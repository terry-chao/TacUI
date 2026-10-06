#pragma once

#include "tacui/geometry.hpp"

namespace tac::ui {

// Design tokens (docs/components.md §5).
//
// Controls never hardcode a colour, radius or type size: they read them from
// here. The point is not configurability for its own sake, it is that the
// example apps and any host-written component stay on the same palette by
// construction instead of by copy-paste. A skin is then one struct, not a
// sweep through thirty components.
//
// Extracted *before* the controls were written on purpose — the doc's C4
// decision: doing it afterwards means editing every control.
struct Theme {
    // Surfaces, back to front.
    Color background   = Color::rgba8(24, 26, 32);
    Color surface      = Color::rgba8(34, 37, 46);
    Color surfaceAlt   = Color::rgba8(28, 31, 39);
    Color surfaceHover = Color::rgba8(44, 48, 60);
    Color surfaceActive= Color::rgba8(52, 57, 72);

    // Type.
    Color textStrong  = Color::rgba8(232, 236, 245);
    Color textDim     = Color::rgba8(150, 158, 176);
    Color textFaint   = Color::rgba8(104, 112, 132);
    Color textOnAccent= Color::rgba8(248, 250, 252);

    // Lines and accents.
    Color border       = Color::rgba8(48, 52, 64);
    Color borderStrong = Color::rgba8(72, 78, 94);
    Color accent       = Color::rgba8(96, 156, 246);
    Color accentHover  = Color::rgba8(124, 176, 255);
    Color accentSoft   = Color::rgba8(40, 62, 105);
    Color danger       = Color::rgba8(232, 92, 92);
    Color success      = Color::rgba8(110, 220, 160);
    Color focusRing    = Color::rgba8(130, 180, 255);

    // Geometry.
    float radiusSm   = 6.0f;
    float radiusMd   = 10.0f;
    float radiusLg   = 14.0f;
    float radiusPill = 999.0f;

    // Spacing scale.
    float spaceXs = 4.0f;
    float spaceSm = 8.0f;
    float spaceMd = 12.0f;
    float spaceLg = 20.0f;

    // Type scale.
    float fontSm = 12.0f;
    float fontMd = 14.0f;
    float fontLg = 16.0f;
    float fontXl = 22.0f;
};

// Built-in skins. `darkTheme()` is the default and matches the example apps'
// original hardcoded palette, so extracting the tokens did not change how
// they look.
const Theme& darkTheme();
const Theme& lightTheme();

} // namespace tac::ui
