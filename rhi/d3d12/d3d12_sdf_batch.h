#pragma once

#include "tacui/rhi.hpp"

#include <windows.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

namespace tac::rhi {

// Owns the rounded-rectangle pipeline: root signature, PSO, and a persistently
// mapped instance ring buffer with one slot per frame in flight.
//
// Everything here is created once and reused for the lifetime of the device —
// no per-frame allocation, which is the retention property M0 criterion 1 is
// meant to demonstrate (plan.md §6.1).
class D3D12SdfBatch {
public:
    static constexpr uint32_t kFramesInFlight      = 2;
    static constexpr uint32_t kMaxInstancesPerFrame = 4096;

    // Must match RectInstance in core/renderer/shaders/sdf_rect.hlsl.
    struct Instance {
        float center[2];
        float halfSize[2];
        float color[4];
        float radius;
        float pad0;
        float pad1[2];
    };
    static_assert(sizeof(Instance) == 48, "Instance layout must match the HLSL RectInstance");

    bool init(ID3D12Device* device, DXGI_FORMAT rtvFormat);
    void shutdown();

    // Records the draw into an already-open command list. `frameIndex` selects
    // the ring-buffer slot; the caller guarantees the GPU is done with that
    // slot before this is called.
    void record(ID3D12GraphicsCommandList* cmd,
                uint32_t frameIndex,
                float viewportWidth,
                float viewportHeight,
                const SdfRect* rects,
                uint32_t count);

    bool valid() const { return pso_ != nullptr; }

private:
    ID3D12Device* device_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D12RootSignature>  rootSig_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  pso_;
    Microsoft::WRL::ComPtr<ID3D12Resource>       instanceBuffer_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap_;

    Instance* mapped_ = nullptr;
};

} // namespace tac::rhi
