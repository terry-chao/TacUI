#include "platform/windows/win32_window.h"

namespace tac::platform {

namespace {

constexpr const wchar_t* kClassName = L"TacUIWindowClass";

inline int32_t mouseAxis(LPARAM lp) {
    return static_cast<int32_t>(static_cast<int16_t>(LOWORD(lp)));
}

} // namespace

Win32Window::~Win32Window() {
    if (hwnd_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

bool Win32Window::create(const wchar_t* title, uint32_t width, uint32_t height) {
    HINSTANCE inst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &Win32Window::wndProcThunk;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    RECT r{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

    hwnd_ = CreateWindowExW(
        0, kClassName, title, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, inst, this);

    return hwnd_ != nullptr;
}

void Win32Window::show() {
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
}

bool Win32Window::pumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            quit_ = true;
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !quit_;
}

bool Win32Window::consumeResize(uint32_t& width, uint32_t& height) {
    if (!resized_) return false;
    width  = pendingWidth_;
    height = pendingHeight_;
    resized_ = false;
    return true;
}

LRESULT CALLBACK Win32Window::wndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs   = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self = static_cast<Win32Window*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    auto* self = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        return self->handleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT Win32Window::handleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CLOSE:
    case WM_DESTROY:
        quit_ = true;
        if (msg == WM_CLOSE) PostQuitMessage(0);
        return 0;

    case WM_SIZE: {
        const uint32_t w = LOWORD(lParam);
        const uint32_t h = HIWORD(lParam);
        if (w > 0 && h > 0) {
            resized_       = true;
            pendingWidth_  = w;
            pendingHeight_ = h;
            if (onEvent_) onEvent_({ EventType::Resize, 0, 0, w, h, 0, false });
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        // Arm WM_MOUSELEAVE so hover state can be cleared when the pointer
        // leaves the client area.
        if (!trackingLeave_) {
            TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd_, 0 };
            if (TrackMouseEvent(&tme)) trackingLeave_ = true;
        }
        if (onEvent_) onEvent_({ EventType::MouseMove, mouseAxis(lParam), mouseAxis(lParam >> 16), 0, 0, 0, false });
        return 0;
    }

    case WM_MOUSELEAVE:
        trackingLeave_ = false;
        if (onEvent_) onEvent_({ EventType::MouseLeave, -1, -1, 0, 0, 0, false });
        return 0;

    case WM_LBUTTONDOWN:
        SetCapture(hwnd_);
        if (onEvent_) onEvent_({ EventType::MouseDown, mouseAxis(lParam), mouseAxis(lParam >> 16), 0, 0, 0, false });
        return 0;

    case WM_LBUTTONUP:
        ReleaseCapture();
        if (onEvent_) onEvent_({ EventType::MouseUp, mouseAxis(lParam), mouseAxis(lParam >> 16), 0, 0, 0, false });
        return 0;

    case WM_KEYDOWN: {
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (onEvent_) {
            onEvent_({ EventType::KeyDown, 0, 0, 0, 0,
                       static_cast<uint32_t>(wParam), shift });
        }
        return 0;
    }

    case WM_CHAR: {
        // WM_CHAR is the translated character, which is what text input wants;
        // WM_KEYDOWN above is for editing and navigation keys. Control
        // characters (0x08 backspace, 0x0D enter, 0x09 tab, 0x1B escape) arrive
        // here too and are dropped, because the key path already handled them.
        //
        // M0 note: this is a single UTF-16 code unit, so astral-plane
        // characters (emoji) arrive as two halves and are not combined.
        const uint32_t cp = static_cast<uint32_t>(wParam);
        if (cp >= 0x20u && cp != 0x7Fu && onEvent_) {
            onEvent_({ EventType::Character, 0, 0, 0, 0, 0, false, cp });
        }
        return 0;
    }

    case WM_ERASEBKGND:
        return 1; // we own the whole client area

    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

} // namespace tac::platform
