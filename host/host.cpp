#include "tacui/host.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "platform/windows/win32_window.h"
#include "tacui/rhi.hpp"
#include "tacui/rhi_factory.hpp"
#include "tools/image_writer.h"

namespace tac::host {

void Control::captureNextFrame(const char* path) {
    if (!path) return;
    captureRequested_ = true;
    capturePath_      = path;
}

namespace {

// The public API is UTF-8; Win32 is wide. Convert at the boundary.
std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int need = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (need <= 0) return {};
    std::wstring out(static_cast<size_t>(need - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), need);
    return out;
}

bool writeCapture(const std::string& utf8Path,
                  const std::vector<uint8_t>& pixels,
                  uint32_t width,
                  uint32_t height) {
    const std::wstring wide = toWide(utf8Path);
    if (wide.empty()) {
        std::fprintf(stderr, "[host] bad capture path\n");
        return false;
    }
    return tools::writePng(wide.c_str(), pixels.data(), width, height);
}

bool toHostEvent(const platform::WindowEvent& in, Event& out) {
    switch (in.type) {
    case platform::EventType::Resize:     out.type = EventType::Resize;     break;
    case platform::EventType::Close:      out.type = EventType::Close;      break;
    case platform::EventType::MouseMove:  out.type = EventType::MouseMove;  break;
    case platform::EventType::MouseLeave: out.type = EventType::MouseLeave; break;
    case platform::EventType::MouseDown:  out.type = EventType::MouseDown;  break;
    case platform::EventType::MouseUp:    out.type = EventType::MouseUp;    break;
    case platform::EventType::MouseWheel: out.type = EventType::MouseWheel; break;
    case platform::EventType::KeyDown:    out.type = EventType::KeyDown;    break;
    case platform::EventType::Character:  out.type = EventType::Character;  break;
    default:                              return false;
    }
    out.x         = in.x;
    out.y         = in.y;
    out.key       = in.key;
    out.codepoint = in.codepoint;
    out.wheel     = static_cast<float>(in.wheel);
    out.shift     = in.shift;
    return true;
}

// Platform virtual keys -> TacUI key codes. The core never sees a VK_*.
ui::KeyCode toKeyCode(uint32_t vk) {
    switch (vk) {
    case VK_TAB:    return ui::KeyCode::Tab;
    case VK_RETURN: return ui::KeyCode::Enter;
    case VK_ESCAPE: return ui::KeyCode::Escape;
    case VK_BACK:   return ui::KeyCode::Backspace;
    case VK_DELETE: return ui::KeyCode::Delete;
    case VK_LEFT:   return ui::KeyCode::Left;
    case VK_RIGHT:  return ui::KeyCode::Right;
    case VK_UP:     return ui::KeyCode::Up;
    case VK_DOWN:   return ui::KeyCode::Down;
    case VK_HOME:   return ui::KeyCode::Home;
    case VK_END:    return ui::KeyCode::End;
    case VK_PRIOR:  return ui::KeyCode::PageUp;
    case VK_NEXT:   return ui::KeyCode::PageDown;
    case VK_SPACE:  return ui::KeyCode::Space;
    default:        return ui::KeyCode::Unknown;
    }
}

bool toInputEvent(const platform::WindowEvent& in, ui::InputEvent& out) {
    out.x     = static_cast<float>(in.x);
    out.y     = static_cast<float>(in.y);
    out.shift = in.shift;
    out.codepoint = in.codepoint;
    out.wheel = static_cast<float>(in.wheel);

    switch (in.type) {
    case platform::EventType::MouseMove:  out.type = ui::InputEventType::MouseMove;  return true;
    case platform::EventType::MouseLeave: out.type = ui::InputEventType::MouseLeave; return true;
    case platform::EventType::MouseDown:  out.type = ui::InputEventType::MouseDown;  return true;
    case platform::EventType::MouseUp:    out.type = ui::InputEventType::MouseUp;    return true;
    case platform::EventType::MouseWheel:
        out.type = ui::InputEventType::MouseWheel;
        return in.wheel != 0;
    case platform::EventType::KeyDown:
        out.type = ui::InputEventType::KeyDown;
        out.code = toKeyCode(in.key);
        return out.code != ui::KeyCode::Unknown;
    case platform::EventType::Character:
        // A translated character, not a key: this is what TextInput edits from.
        out.type = ui::InputEventType::KeyDown;
        out.code = ui::KeyCode::Character;
        return out.codepoint != 0;
    default:
        return false;
    }
}

} // namespace

int run(ui::Ui& ui, const Options& opts, const FrameFn& frame, const EventFn& event) {
    // DPI awareness has to be declared before any window exists, and we need the
    // DPI before creating one to size it — so enable it first, then read the
    // system DPI. (`Win32Window::create` also calls this; it is idempotent.)
    platform::enableDpiAwareness();

    const float initialScale =
        opts.uiScale > 0.0f ? opts.uiScale
                            : static_cast<float>(GetDpiForSystem()) / 96.0f;
    const uint32_t physWidth  = static_cast<uint32_t>(std::lround(opts.width  * initialScale));
    const uint32_t physHeight = static_cast<uint32_t>(std::lround(opts.height * initialScale));

    platform::Win32Window window;
    const std::wstring wideTitle = toWide(opts.title ? opts.title : "TacUI");
    if (!window.create(wideTitle.c_str(), physWidth, physHeight)) {
        std::fprintf(stderr, "[host] failed to create the window\n");
        return -1;
    }

    // The monitor the window actually landed on may differ from the system DPI,
    // so re-derive the scale now that the window exists.
    const float scale = opts.uiScale > 0.0f ? opts.uiScale : window.dpiScale();
    ui.setScale(scale);

    if (scale != 1.0f) {
        std::fprintf(stderr,
                     "[host] DPI scale %.2f -> rendering at %ux%u physical for a "
                     "%ux%u logical UI\n",
                     scale, physWidth, physHeight, opts.width, opts.height);
    }

    rhi::SwapchainDesc desc{};
    desc.window      = window.handle();
    desc.width       = physWidth;
    desc.height      = physHeight;
    desc.bufferCount = 2;

    std::unique_ptr<rhi::Device> device = rhi::createD3D12Device(desc);
    if (!device) {
        std::fprintf(stderr, "[host] failed to create the D3D12 device\n");
        return -2;
    }

    Control control;
    control.width_  = opts.width;
    control.height_ = opts.height;

    // Window events arrive in physical client pixels; the UI is authored in
    // logical units, so convert once here and everything downstream (hit
    // testing, app code) stays logical.
    window.setEventFn([&event, &control, &ui, scale](const platform::WindowEvent& in) {
        // Esc always quits, so examples do not each have to wire it up.
        if (in.type == platform::EventType::KeyDown && in.key == VK_ESCAPE) {
            control.exitRequested_ = true;
            return;
        }

        platform::WindowEvent logical = in;
        if (in.x != -1 || in.y != -1) {   // -1,-1 is MouseLeave, left alone
            logical.x = static_cast<int32_t>(std::lround(in.x / scale));
            logical.y = static_cast<int32_t>(std::lround(in.y / scale));
        }

        // The framework owns interaction: hit testing, hover, press, focus.
        ui::InputEvent ie;
        if (toInputEvent(logical, ie)) ui.dispatchEvent(ie);

        // The app callback is for app-level concerns only. It never has to
        // test a bounding box.
        if (!event) return;
        Event out;
        if (toHostEvent(logical, out)) event(out);
    });

    window.show();

    // Build once before the first frame callback so the callback can resolve
    // node handles immediately rather than from frame two onwards.
    ui.update();

    const auto start = std::chrono::steady_clock::now();

    while (window.pumpMessages()) {
        uint32_t w = 0;
        uint32_t h = 0;
        if (window.consumeResize(w, h)) {
            device->resize(w, h);   // physical back buffer
            control.width_  = static_cast<uint32_t>(std::lround(w / scale));
            control.height_ = static_cast<uint32_t>(std::lround(h / scale));
        }

        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        if (frame) frame(control, elapsed);

        if (elapsed >= opts.maxSeconds && opts.maxSeconds > 0.0) {
            control.exitRequested_ = true;
        }

        ui.update();

        const bool doCapture = control.captureRequested_;
        device->beginFrame(opts.clear);
        ui.paint(*device);
        if (doCapture) device->captureFrame();
        device->endFrame();
        device->reportDiagnostics();

        if (doCapture) {
            std::vector<uint8_t> pixels;
            uint32_t capW = 0;
            uint32_t capH = 0;
            if (device->readbackFrame(pixels, capW, capH)) {
                writeCapture(control.capturePath_, pixels, capW, capH);
            }
            control.captureRequested_ = false;
            control.exitRequested_    = true;
        }

        ++control.frame_;

        if (control.exitRequested_) break;
    }

    // Deliberately no ui.shutdown() here: the caller may still want to inspect
    // the tree (the M0 harness asserts on it after the loop). ~Ui handles it.
    return 0;
}

} // namespace tac::host
