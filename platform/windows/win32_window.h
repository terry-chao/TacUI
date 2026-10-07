#pragma once

#include <cstdint>
#include <functional>

#include <windows.h>

namespace tac::platform {

// Declares per-monitor DPI awareness for the process. Idempotent; must run
// before any window is created. Win32Window::create calls it too, but a host
// that needs the DPI before creating a window (to size it) calls it first.
void enableDpiAwareness();

enum class EventType {
    Resize,
    Close,
    MouseMove,
    MouseLeave,   // x and y are -1
    MouseDown,
    MouseUp,
    MouseWheel,
    KeyDown,
    Character,   // a translated WM_CHAR; `codepoint` is valid
};

struct WindowEvent {
    EventType type = EventType::Close;
    int32_t   x = 0;          // client-relative, MouseMove / MouseDown / MouseUp
    int32_t   y = 0;
    uint32_t  width = 0;      // Resize
    uint32_t  height = 0;
    uint32_t  key = 0;        // KeyDown (virtual key code)
    bool      shift = false;  // KeyDown: modifier state
    uint32_t  codepoint = 0;  // Character: a UTF-16 code unit (BMP codepoint)
    int32_t   wheel = 0;      // MouseWheel: notches, positive = away from user
};

class Win32Window {
public:
    using EventFn = std::function<void(const WindowEvent&)>;

    Win32Window() = default;
    ~Win32Window();

    Win32Window(const Win32Window&)            = delete;
    Win32Window& operator=(const Win32Window&) = delete;

    bool create(const wchar_t* title, uint32_t width, uint32_t height);
    void show();

    HWND handle() const { return hwnd_; }

    // DPI of the monitor this window is on, as a scale over 96 DPI (so 1.5 at
    // 150%). Requires the process to be per-monitor DPI aware — create() sets
    // that up before the window exists.
    float dpiScale() const;

    void setEventFn(EventFn fn) { onEvent_ = std::move(fn); }

    // Drains the message queue. Returns false once the window has closed.
    bool pumpMessages();

    // True when a WM_SIZE arrived since the last call, and reports the new
    // client size. Clears the flag.
    bool consumeResize(uint32_t& width, uint32_t& height);

private:
    static LRESULT CALLBACK wndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    HWND     hwnd_ = nullptr;
    EventFn  onEvent_;
    bool     quit_ = false;

    bool     resized_       = false;
    uint32_t pendingWidth_  = 0;
    uint32_t pendingHeight_ = 0;

    bool     trackingLeave_ = false;
};

} // namespace tac::platform
