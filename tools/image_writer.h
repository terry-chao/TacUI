#pragma once

#include <cstdint>

namespace tac::tools {

// Writes tightly packed RGBA8 pixels to a PNG via WIC. Used by the M0 capture
// path and, later, by the golden-image regression tests (plan.md §9).
bool writePng(const wchar_t* path, const uint8_t* rgba, uint32_t width, uint32_t height);

} // namespace tac::tools
