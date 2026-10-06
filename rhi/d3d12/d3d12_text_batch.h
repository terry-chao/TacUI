#pragma once

#include "tacui/rhi.hpp"

#include <windows.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

namespace tac::rhi {

// The glyph pipeline: root signature, PSO, an R8 coverage atlas and the
// instance ring buffer feeding it.
//
// The atlas texture and the instance buffer are created once and reused for
// the lifetime of the device. Uploads only happen when new glyphs were
// rasterised, which after startup is rare — that is the retention property
// M0 criterion 1 is checking (plan.md §6.1).
class D3D12TextBatch {
public:
    static constexpr uint32_t kFramesInFlight       = 2;
    static constexpr uint32_t kMaxInstancesPerFrame = 8192;

    // Must match GlyphInstance in core/renderer/shaders/glyph.hlsl.
    struct Instance {
        float center[2];
        float halfSize[2];
        float uv[4];
        float color[4];
    };
    // center(8) + halfSize(8) + uv(16) + color(16)
    static_assert(sizeof(Instance) == 48, "Instance layout must match the HLSL GlyphInstance");

    bool init(ID3D12Device* device, DXGI_FORMAT rtvFormat, uint32_t atlasSize);
    void shutdown();

    // Copies `pixels` into the staging buffer. The GPU-side copy is recorded
    // during the next record() call.
    void uploadAtlas(const uint8_t* pixels, uint32_t size);

    void record(ID3D12GraphicsCommandList* cmd,
                uint32_t frameIndex,
                float viewportWidth,
                float viewportHeight,
                const GlyphQuad* quads,
                uint32_t count);

    bool valid() const { return pso_ != nullptr; }

private:
    ID3D12Device* device_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D12RootSignature>  rootSig_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  pso_;
    Microsoft::WRL::ComPtr<ID3D12Resource>       instances_;
    Microsoft::WRL::ComPtr<ID3D12Resource>       atlas_;
    Microsoft::WRL::ComPtr<ID3D12Resource>       atlasStaging_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap_;

    Instance* mapped_ = nullptr;
    uint8_t*  mappedAtlas_ = nullptr;

    uint32_t atlasSize_    = 0;
    uint32_t atlasRowPitch_ = 0;
    bool     atlasPending_  = false;
    uint32_t srvStride_     = 0;
};

} // namespace tac::rhi
