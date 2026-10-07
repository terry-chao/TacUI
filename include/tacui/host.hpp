#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "tacui/geometry.hpp"
#include "tacui/ui.hpp"

namespace tac::host {

// A ready-made Win32 + D3D12 host.
//
// TacUI's core is embeddable — an application that already owns a window and a
// renderer drives ui.update() / ui.paint() itself. This is the convenience
// path for everything else, and it is what the examples use so they can stay
// about the UI rather than about window plumbing.

struct Options {
    // UTF-8, like every other string in the public API. wchar_t is avoided on
    // purpose: its width is platform-dependent, and this struct also feeds the
    // C ABI.
    const char* title  = "TacUI";
    uint32_t    width  = 1280;
    uint32_t    height = 720;
    Color       clear  = Color::rgba8(24, 26, 32);

    // Stop after this many seconds. 0 runs until the window closes.
    double maxSeconds = 0.0;

    // Write the final frame to a PNG and exit. UTF-8 path; null disables.
    const char* capturePath        = nullptr;
    uint32_t    captureAfterFrames = 90;
};

// Frame-loop controls handed to the callback.
class Control {
public:
    void requestExit() { exitRequested_ = true; }

    // Capture a frame to `path` (UTF-8) and then stop. Used by the examples to
    // produce a screenshot without a human at the keyboard.
    void captureNextFrame(const char* path);

    uint32_t frame() const { return frame_; }
    uint32_t clientWidth() const { return width_; }
    uint32_t clientHeight() const { return height_; }

    // Written by host::run; not part of the API surface.
    bool        exitRequested_    = false;
    bool        captureRequested_ = false;
    std::string capturePath_;
    uint32_t    frame_  = 0;
    uint32_t    width_  = 0;
    uint32_t    height_ = 0;
};

// Called once per frame, before the rebuild. Animations belong here, driven
// through the direct-write channel — which must never mark the tree dirty.
using FrameFn = std::function<void(Control&, double elapsedSeconds)>;

// Input events. Deliberately a separate type from the platform layer's, so
// this header stays free of windows.h.
enum class EventType {
    Resize,
    Close,
    MouseMove,
    MouseLeave,
    MouseDown,
    MouseUp,
    MouseWheel,
    KeyDown,
    Character,   // a translated character; `codepoint` is valid
};

struct Event {
    EventType type = EventType::Close;
    int32_t   x    = 0;       // client-relative pixels; -1,-1 for MouseLeave
    int32_t   y    = 0;
    uint32_t  key   = 0;      // virtual key code, for KeyDown
    uint32_t  codepoint = 0;  // Character: the translated codepoint
    float     wheel = 0.0f;   // MouseWheel: notches, positive away from the user
    bool      shift = false;  // KeyDown: was shift held
};

using EventFn = std::function<void(const Event&)>;

// Window -> device -> loop: frame callback, ui.update(), ui.paint(), present.
// Returns 0 on a clean exit, negative if setup failed.
int run(ui::Ui&        ui,
        const Options& opts,
        const FrameFn& frame = {},
        const EventFn& event = {});

} // namespace tac::host
