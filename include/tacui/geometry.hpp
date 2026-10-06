#pragma once

#include <cstdint>

namespace tac {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    constexpr float right() const { return x + w; }
    constexpr float bottom() const { return y + h; }
    constexpr bool empty() const { return w <= 0.0f || h <= 0.0f; }

    constexpr bool contains(float px, float py) const {
        return px >= x && px < right() && py >= y && py < bottom();
    }
};

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    static constexpr Color rgba8(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
        return Color{ r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f };
    }
};

} // namespace tac
