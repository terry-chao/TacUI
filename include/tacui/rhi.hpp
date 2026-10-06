#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "tacui/geometry.hpp"

namespace tac::rhi {

// Opaque native window handle (HWND on Windows). The RHI never includes
// platform headers itself; the platform layer hands us the raw handle.
using NativeWindow = void*;

// Coverage atlas page size every backend is built for. A caller's glyph atlas
// must be exactly this size — setGlyphAtlas rejects anything else.
constexpr uint32_t kGlyphAtlasSize = 1024;

struct SwapchainDesc {
    NativeWindow window      = nullptr;
    uint32_t     width       = 0;
    uint32_t     height      = 0;
    uint32_t     bufferCount = 2;
};

// A filled, optionally rounded rectangle — M0's only primitive.
//
// This exists purely so the harness can put pixels on screen without knowing
// anything about the backend. M1 replaces it with a real DrawBatch + RHI
// resource model (architecture.md §3.2).
struct SdfRect {
    Rect  bounds;                   // pixel space, top-left origin
    float cornerRadius = 0.0f;
    Color color;
};

// One glyph quad, already positioned and mapped into the coverage atlas
// supplied via setGlyphAtlas.
struct GlyphQuad {
    Rect  bounds;                   // pixel space, top-left origin
    float u0 = 0.0f, v0 = 0.0f;     // top-left of the atlas rect
    float u1 = 0.0f, v1 = 0.0f;     // bottom-right
    Color color;
};

// Backend-agnostic frame surface.
//
// Deliberately narrow for M0: frame lifecycle only, so the renderer layer
// never sees a d3d12.h. Pipelines, buffers and textures land in slices B/C;
// they will be added here as opaque handles rather than leaking COM types.
class Device {
public:
    virtual ~Device() = default;

    Device(const Device&)            = delete;
    Device& operator=(const Device&) = delete;

    // Begins recording a frame and clears the back buffer.
    virtual void beginFrame(Color clear) = 0;

    // Ends recording, submits, and presents (vsync).
    virtual void endFrame() = 0;

    // Recreates size-dependent resources. Safe to call every frame; no-ops
    // when the dimensions are unchanged.
    virtual void resize(uint32_t width, uint32_t height) = 0;

    // Drains backend validation messages (D3D12 debug layer, Vulkan validation
    // layers) and reports them. Cheap no-op when validation is compiled out.
    virtual void reportDiagnostics() {}

    // Records a batch of rounded rectangles. Must be called between beginFrame
    // and endFrame; draw order follows array order (painter's algorithm).
    virtual void drawSdfRects(const SdfRect* rects, uint32_t count) = 0;

    // Uploads the coverage atlas. `pixels` is a size*size single-channel image,
    // row-major. Safe to call any time; the upload is folded into the next
    // frame. Call only when the contents actually changed.
    virtual void setGlyphAtlas(const uint8_t* /*pixels*/, uint32_t /*size*/) {}

    // Records a batch of glyph quads. Like drawSdfRects, must sit between
    // beginFrame and endFrame.
    virtual void drawGlyphQuads(const GlyphQuad* /*quads*/, uint32_t /*count*/) {}

    // Records a full-frame copy into a staging buffer. Must be called between
    // beginFrame and endFrame, after all draws. Pair with readbackFrame().
    // Diagnostic path — also the seed of the golden-image test harness.
    virtual void captureFrame() {}

    // Copies the last captured frame into `out` as tightly packed BGRA8 (the
    // native Windows surface order). Returns false if nothing was captured.
    virtual bool readbackFrame(std::vector<uint8_t>& /*out*/,
                               uint32_t& /*width*/,
                               uint32_t& /*height*/) {
        return false;
    }

protected:
    Device() = default;
};

} // namespace tac::rhi
