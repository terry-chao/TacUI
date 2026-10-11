#include "tacui/host.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

    // -----------------------------------------------------------------------
    // Two threads from here on.
    //
    // Windows' modal move/size loop calls WM_SIZE *synchronously*, so any GPU
    // work done in the message handler stalls the very loop that is moving the
    // window — that is what made resizing judder. So the device and every GPU
    // call move to a render thread, and the window procedure is left with
    // nothing but two atomic stores.
    //
    // The mutex is the one place the two threads meet: the UI thread holds it
    // while it touches the tree (input, app callback, update), the render
    // thread holds it while it paints. Sizes travel through atomics.
    // -----------------------------------------------------------------------
    std::mutex            uiMutex;
    std::atomic<bool>     running{ true };
    std::atomic<bool>     deviceFailed{ false };
    std::atomic<bool>     captureRequested{ false };
    std::atomic<bool>     captureDone{ false };
    std::mutex            captureMutex;
    std::string           capturePath;

    std::mutex              frameMutex;
    std::condition_variable frameCv;
    uint64_t                renderedFrames = 0;

    // Size handshake. The window procedure publishes the requested size and, if
    // the user is dragging, waits until the renderer has actually resized and
    // presented it. Without that wait the window is already at the new size
    // while the back buffer is still the old one, and the desktop scales the
    // stale frame to fit — a few transient milliseconds of the whole UI
    // changing size, which is exactly what reads as the content jittering.
    std::mutex              sizeMutex;
    std::condition_variable appliedCv;   // renderer  -> UI thread
    uint32_t                reqW = physWidth;
    uint32_t                reqH = physHeight;
    uint32_t                appW = physWidth;
    uint32_t                appH = physHeight;
    std::atomic<bool>       inDrag{ false };

    Control control;
    control.width_  = opts.width;
    control.height_ = opts.height;

    // Window events arrive in physical client pixels; the UI is authored in
    // logical units, so convert once here and everything downstream (hit
    // testing, app code) stays logical.
    window.setEventFn([&](const platform::WindowEvent& in) {
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

        std::lock_guard<std::mutex> lock(uiMutex);

        // The framework owns interaction: hit testing, hover, press, focus.
        ui::InputEvent ie;
        if (toInputEvent(logical, ie)) ui.dispatchEvent(ie);

        // The app callback is for app-level concerns only. It never has to
        // test a bounding box.
        if (!event) return;
        Event out;
        if (toHostEvent(logical, out)) event(out);
    });

    // Nothing but bookkeeping plus the handshake: this runs *inside* Windows'
    // modal drag loop, so the only cost here is the swapchain resize the
    // renderer is doing concurrently.
    window.setResizeFn([&](uint32_t w, uint32_t h) {
        if (w == 0 || h == 0) return;

        control.width_  = static_cast<uint32_t>(std::lround(w / scale));
        control.height_ = static_cast<uint32_t>(std::lround(h / scale));

        {
            std::lock_guard<std::mutex> lock(sizeMutex);
            reqW = w;
            reqH = h;
        }
        // The renderer parks between size requests while a drag is in flight
        // (see the render loop), so publishing the size is also what has to
        // wake it.
        appliedCv.notify_all();

        // Wait for the renderer only while the user is dragging: that is when
        // the window is changing continuously and a buffer one size behind is
        // visible. A programmatic resize can lag harmlessly.
        if (inDrag.load(std::memory_order_relaxed)) {
            std::unique_lock<std::mutex> lock(sizeMutex);
            appliedCv.wait_for(lock, std::chrono::milliseconds(100), [&] {
                return (appW == reqW && appH == reqH) || !running.load();
            });
        }
    });

    window.setDragFn([&](bool dragging) {
        inDrag.store(dragging, std::memory_order_relaxed);
    });

    // Build once before the first frame callback so the callback can resolve
    // node handles immediately rather than from frame two onwards.
    { std::lock_guard<std::mutex> lock(uiMutex); ui.update(); }

    std::thread renderThread([&] {
        rhi::SwapchainDesc desc{};
        desc.window      = window.handle();
        desc.width       = physWidth;
        desc.height      = physHeight;
        desc.bufferCount = 2;

        std::unique_ptr<rhi::Device> device = rhi::createD3D12Device(desc);
        if (!device) {
            deviceFailed.store(true);
            running.store(false);
            frameCv.notify_all();
            return;
        }

        while (running.load(std::memory_order_relaxed)) {
            uint32_t w = 0;
            uint32_t h = 0;
            bool     resizing = false;
            {
                // No timed wait here: Present is what paces this loop (vsync
                // when idle, unthrottled while the size is moving). A timed
                // wait would land on the system timer granularity — ~15.6ms —
                // and silently cap the whole application to 60Hz.
                std::lock_guard<std::mutex> lock(sizeMutex);
                w        = reqW;
                h        = reqH;
                resizing = (w != appW || h != appH);
            }

            // While the user is dragging the window, the UI thread drives this
            // loop: it publishes a size and then blocks in the window procedure
            // until the swapchain has caught up. Rendering freely in between is
            // what burns the frame that is still on the flip queue, and the
            // next ResizeBuffers/waitForGpu then has to wait for the compositor
            // to hand that buffer back — a multi-millisecond stall that lands
            // *inside* Windows' modal sizing loop, which is exactly what reads
            // as jitter while dragging. So park here between size requests.
            //
            // This costs nothing visually: the drag has the UI thread blocked
            // inside the modal loop, so neither the app's frame callback nor
            // update() is running and the content cannot change.
            if (inDrag.load(std::memory_order_relaxed) && !resizing &&
                !captureRequested.load(std::memory_order_relaxed)) {
                std::unique_lock<std::mutex> lock(sizeMutex);
                appliedCv.wait_for(lock, std::chrono::milliseconds(5), [&] {
                    return reqW != appW || reqH != appH ||
                           !inDrag.load(std::memory_order_relaxed) ||
                           !running.load(std::memory_order_relaxed);
                });
                continue;
            }

            if (resizing) device->resize(w, h);

            // Never wait for vblank while the size is still moving: the buffer
            // has to catch up with the window, not fall a frame behind it.
            device->setVSync(!resizing && !inDrag.load(std::memory_order_relaxed));

            const bool doCapture = captureRequested.load(std::memory_order_relaxed);

            device->beginFrame(opts.clear);
            {
                std::lock_guard<std::mutex> lock(uiMutex);
                ui.paint(*device);
            }
            if (doCapture) device->captureFrame();
            device->endFrame();
            device->reportDiagnostics();

            if (doCapture) {
                std::string path;
                { std::lock_guard<std::mutex> lock(captureMutex); path = capturePath; }

                std::vector<uint8_t> pixels;
                uint32_t capW = 0;
                uint32_t capH = 0;
                if (device->readbackFrame(pixels, capW, capH)) {
                    writeCapture(path, pixels, capW, capH);
                }
                captureRequested.store(false, std::memory_order_relaxed);
                captureDone.store(true, std::memory_order_relaxed);
            }

            if (resizing) {
                {
                    std::lock_guard<std::mutex> lock(sizeMutex);
                    appW = w;
                    appH = h;
                }
                appliedCv.notify_all();
            }

            {
                std::lock_guard<std::mutex> lock(frameMutex);
                ++renderedFrames;
            }
            frameCv.notify_all();
        }

        device.reset();   // D3D12 objects die on the thread that used them
    });

    window.show();

    const auto start = std::chrono::steady_clock::now();
    uint64_t   seenFrames = 0;

    while (!control.exitRequested_ && window.pumpMessages()) {
        // One UI iteration per presented frame. Without this the loop would
        // spin, and the app's frame callback would run far more often than
        // anything actually gets drawn.
        {
            std::unique_lock<std::mutex> lock(frameMutex);
            frameCv.wait_for(lock, std::chrono::milliseconds(50),
                             [&] { return renderedFrames > seenFrames; });
            seenFrames = renderedFrames;
        }
        if (deviceFailed.load()) break;

        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        {
            std::lock_guard<std::mutex> lock(uiMutex);
            if (frame) frame(control, elapsed);
            if (elapsed >= opts.maxSeconds && opts.maxSeconds > 0.0) {
                control.exitRequested_ = true;
            }
            ui.update();
        }

        // Hand a capture request to the renderer; it owns the device.
        if (control.captureRequested_) {
            {
                std::lock_guard<std::mutex> lock(captureMutex);
                capturePath = control.capturePath_;
            }
            captureRequested.store(true, std::memory_order_relaxed);
            control.captureRequested_ = false;
        }
        if (captureDone.load()) control.exitRequested_ = true;

        ++control.frame_;
    }

    running.store(false);
    frameCv.notify_all();
    appliedCv.notify_all();   // the renderer may be parked for the next size
    if (renderThread.joinable()) renderThread.join();

    // Deliberately no ui.shutdown() here: the caller may still want to inspect
    // the tree (the M0 harness asserts on it after the loop). ~Ui handles it.
    return deviceFailed.load() ? -2 : 0;
}

} // namespace tac::host
