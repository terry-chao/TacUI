#pragma once

#include "tacui/rhi.hpp"
#include "rhi/d3d12/d3d12_sdf_batch.h"
#include "rhi/d3d12/d3d12_text_batch.h"

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace tac::rhi {

// D3D12 frame surface: device, direct queue, flip-model swapchain and the
// per-frame synchronisation. M0 keeps this to the frame lifecycle only —
// pipelines, buffers and textures arrive in slices B/C.
class D3D12Device final : public Device {
public:
    static std::unique_ptr<Device> create(const SwapchainDesc& desc);

    ~D3D12Device() override;

    void beginFrame(Color clear) override;
    void endFrame() override;
    void resize(uint32_t width, uint32_t height) override;

    void reportDiagnostics() override;
    void drawSdfRects(const SdfRect* rects, uint32_t count) override;
    void setGlyphAtlas(const uint8_t* pixels, uint32_t size) override;
    void drawGlyphQuads(const GlyphQuad* quads, uint32_t count) override;
    void captureFrame() override;
    bool readbackFrame(std::vector<uint8_t>& out, uint32_t& width, uint32_t& height) override;

private:
    D3D12Device() = default;

    bool init(const SwapchainDesc& desc);
    void createSwapchain(IDXGIFactory6* factory, HWND hwnd);
    void createCommandObjects();
    void createRenderTargets();
    void releaseRenderTargets();
    void waitForFrame(uint32_t slot);
    void waitForGpu();

    static constexpr uint32_t kFramesInFlight = 2;
    static constexpr uint32_t kMaxBuffers     = 3;

    Microsoft::WRL::ComPtr<ID3D12Device>              device_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3>           swapchain_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>        queue_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    allocators_[kFramesInFlight];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmd_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>      rtvHeap_;
    Microsoft::WRL::ComPtr<ID3D12Resource>            backBuffers_[kMaxBuffers];
    Microsoft::WRL::ComPtr<ID3D12Fence>               fence_;
    Microsoft::WRL::ComPtr<ID3D12InfoQueue>           infoQueue_;
    HANDLE                                            fenceEvent_ = nullptr;

    D3D12SdfBatch  batch_;
    D3D12TextBatch text_;

    // Capture path: a READBACK-heap buffer holding the last captured frame.
    Microsoft::WRL::ComPtr<ID3D12Resource> captureBuffer_;
    uint32_t captureRowPitch_ = 0;
    uint32_t captureWidth_    = 0;
    uint32_t captureHeight_   = 0;

    bool ensureCaptureBuffer(uint32_t width, uint32_t height);

    uint64_t fenceValues_[kFramesInFlight] = {};
    uint64_t fenceCounter_ = 0;

    uint32_t rtvStride_   = 0;
    uint32_t frameIndex_  = 0;
    uint32_t width_       = 0;
    uint32_t height_      = 0;
    uint32_t bufferCount_ = 2;

    // Swapchain back buffers start in COMMON, not PRESENT; the first barrier
    // for each buffer must come from COMMON or the debug layer complains.
    bool firstUse_[kMaxBuffers] = { true, true, true };
};

} // namespace tac::rhi
