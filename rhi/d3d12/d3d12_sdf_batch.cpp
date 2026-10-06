#include "rhi/d3d12/d3d12_sdf_batch.h"

#include <cstdio>
#include <stdexcept>

// Generated at build time by dxc -Fh (see CMakeLists.txt).
#include "sdf_rect_vs.h"
#include "sdf_rect_ps.h"

using Microsoft::WRL::ComPtr;

namespace tac::rhi {

namespace {

void check(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) return;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s failed (hr=0x%08lX)", what,
                  static_cast<unsigned long>(hr));
    throw std::runtime_error(buf);
}

// Root parameter indices, matching the root signature built in init().
constexpr UINT kParamConstants = 0;   // b0: viewport size + instance base
constexpr UINT kParamInstances = 1;   // t0: structured buffer

constexpr UINT kRootConstantDwords = 4;

// Mirrors cbuffer FrameConstants in sdf_rect.hlsl. Root constants are raw
// 32-bit values, so the instance base must stay an integer — passing it as a
// float would reinterpret its bit pattern.
struct FrameConstants {
    float    viewport[2];
    uint32_t instanceBase;
    float    pad0;
};
static_assert(sizeof(FrameConstants) == kRootConstantDwords * 4);

} // namespace

bool D3D12SdfBatch::init(ID3D12Device* device, DXGI_FORMAT rtvFormat) {
    device_ = device;

    // ---- root signature -------------------------------------------------
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors                    = 1;
    srvRange.BaseShaderRegister                = 0;
    srvRange.RegisterSpace                     = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2]{};
    params[kParamConstants].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[kParamConstants].Constants.ShaderRegister = 0;
    params[kParamConstants].Constants.RegisterSpace  = 0;
    params[kParamConstants].Constants.Num32BitValues = kRootConstantDwords;
    params[kParamConstants].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

    params[kParamInstances].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kParamInstances].DescriptorTable.NumDescriptorRanges = 1;
    params[kParamInstances].DescriptorTable.pDescriptorRanges   = &srvRange;
    params[kParamInstances].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters     = 2;
    rsd.pParameters       = params;
    rsd.NumStaticSamplers = 0;
    rsd.pStaticSamplers   = nullptr;
    rsd.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0,
                                             &signature, &error);
    if (FAILED(hr)) {
        if (error) {
            std::fprintf(stderr, "[rhi] root signature: %s\n",
                         static_cast<const char*>(error->GetBufferPointer()));
        }
        check(hr, "D3D12SerializeRootSignature");
    }
    check(device->CreateRootSignature(0, signature->GetBufferPointer(),
                                      signature->GetBufferSize(),
                                      IID_PPV_ARGS(&rootSig_)),
          "CreateRootSignature");

    // ---- pipeline state -------------------------------------------------
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature        = rootSig_.Get();
    pd.VS                    = { g_VSMain, sizeof(g_VSMain) };
    pd.PS                    = { g_PSMain, sizeof(g_PSMain) };
    pd.InputLayout           = { nullptr, 0 };   // quad is generated from SV_VertexID
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets      = 1;
    pd.RTVFormats[0]         = rtvFormat;
    pd.SampleDesc.Count      = 1;
    pd.SampleMask            = UINT_MAX;
    pd.DSVFormat             = DXGI_FORMAT_UNKNOWN;

    auto& rs = pd.RasterizerState;
    rs.FillMode              = D3D12_FILL_MODE_SOLID;
    rs.CullMode              = D3D12_CULL_MODE_NONE;
    rs.FrontCounterClockwise = FALSE;
    rs.DepthClipEnable       = TRUE;

    auto& rt = pd.BlendState.RenderTarget[0];
    rt.BlendEnable           = TRUE;
    rt.BlendOp               = D3D12_BLEND_OP_ADD;
    rt.SrcBlend              = D3D12_BLEND_SRC_ALPHA;
    rt.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha         = D3D12_BLEND_ONE;
    rt.DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    pd.DepthStencilState.DepthEnable   = FALSE;
    pd.DepthStencilState.StencilEnable = FALSE;

    check(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_)),
          "CreateGraphicsPipelineState");

    // ---- instance ring buffer (upload, persistently mapped) -------------
    const UINT64 bufferBytes =
        static_cast<UINT64>(sizeof(Instance)) * kMaxInstancesPerFrame * kFramesInFlight;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = bufferBytes;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                          IID_PPV_ARGS(&instanceBuffer_)),
          "CreateCommittedResource(instances)");

    check(instanceBuffer_->Map(0, nullptr, reinterpret_cast<void**>(&mapped_)),
          "Map(instances)");

    // One descriptor for the whole ring; the per-frame slice is selected by
    // the gInstanceBase root constant, so this never needs rewriting.
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap_)),
          "CreateDescriptorHeap(SRV)");

    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension                 = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Format                        = DXGI_FORMAT_UNKNOWN;
    sd.Buffer.FirstElement           = 0;
    sd.Buffer.NumElements            = kMaxInstancesPerFrame * kFramesInFlight;
    sd.Buffer.StructureByteStride    = sizeof(Instance);
    sd.Buffer.Flags                  = D3D12_BUFFER_SRV_FLAG_NONE;
    device->CreateShaderResourceView(instanceBuffer_.Get(), &sd,
                                     srvHeap_->GetCPUDescriptorHandleForHeapStart());

    return true;
}

void D3D12SdfBatch::shutdown() {
    if (mapped_) {
        instanceBuffer_->Unmap(0, nullptr);
        mapped_ = nullptr;
    }
    pso_.Reset();
    rootSig_.Reset();
    instanceBuffer_.Reset();
    srvHeap_.Reset();
    device_ = nullptr;
}

void D3D12SdfBatch::record(ID3D12GraphicsCommandList* cmd,
                           uint32_t frameIndex,
                           float viewportWidth,
                           float viewportHeight,
                           const SdfRect* rects,
                           uint32_t count) {
    if (count == 0 || !pso_) return;
    if (count > kMaxInstancesPerFrame) {
        std::fprintf(stderr,
                     "[rhi] drawSdfRects: %u rects exceeds the %u-per-frame cap; clipped\n",
                     count, kMaxInstancesPerFrame);
        count = kMaxInstancesPerFrame;
    }

    const uint32_t base = frameIndex * kMaxInstancesPerFrame;
    Instance* dst = mapped_ + base;

    for (uint32_t i = 0; i < count; ++i) {
        const SdfRect& r = rects[i];
        dst[i].center[0]   = r.bounds.x + r.bounds.w * 0.5f;
        dst[i].center[1]   = r.bounds.y + r.bounds.h * 0.5f;
        dst[i].halfSize[0] = r.bounds.w * 0.5f;
        dst[i].halfSize[1] = r.bounds.h * 0.5f;
        dst[i].color[0]    = r.color.r;
        dst[i].color[1]    = r.color.g;
        dst[i].color[2]    = r.color.b;
        dst[i].color[3]    = r.color.a;
        dst[i].radius      = r.cornerRadius;
        dst[i].pad0        = 0.0f;
        dst[i].pad1[0]     = 0.0f;
        dst[i].pad1[1]     = 0.0f;
        dst[i].clip[0]     = r.clip.x0;
        dst[i].clip[1]     = r.clip.y0;
        dst[i].clip[2]     = r.clip.x1;
        dst[i].clip[3]     = r.clip.y1;
        dst[i].clipEnabled = r.clip.enabled ? 1.0f : 0.0f;
        dst[i].pad2[0]     = 0.0f;
        dst[i].pad2[1]     = 0.0f;
        dst[i].pad2[2]     = 0.0f;
    }

    cmd->SetGraphicsRootSignature(rootSig_.Get());
    cmd->SetPipelineState(pso_.Get());

    ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
    cmd->SetDescriptorHeaps(1, heaps);

    const FrameConstants constants{ { viewportWidth, viewportHeight }, base, 0.0f };
    cmd->SetGraphicsRoot32BitConstants(kParamConstants, kRootConstantDwords, &constants, 0);
    cmd->SetGraphicsRootDescriptorTable(kParamInstances,
                                        srvHeap_->GetGPUDescriptorHandleForHeapStart());

    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(6, count, 0, 0);
}

} // namespace tac::rhi
