#include "rhi/d3d12/d3d12_text_batch.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

// Generated at build time by dxc -Fh (see CMakeLists.txt).
#include "glyph_vs.h"
#include "glyph_ps.h"

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

constexpr UINT kParamConstants = 0;   // b0: viewport size + instance base
constexpr UINT kParamTextures  = 1;   // t0 instances, t1 atlas
constexpr UINT kRootConstantDwords = 4;

struct FrameConstants {
    float    viewport[2];
    uint32_t instanceBase;
    float    pad0;
};
static_assert(sizeof(FrameConstants) == kRootConstantDwords * 4);

constexpr uint32_t align256(uint32_t v) {
    return (v + 255u) & ~255u;
}

} // namespace

bool D3D12TextBatch::init(ID3D12Device* device, DXGI_FORMAT rtvFormat, uint32_t atlasSize) {
    device_    = device;
    atlasSize_ = atlasSize;
    atlasRowPitch_ = align256(atlasSize);

    // ---- root signature -------------------------------------------------
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors                    = 1;
    ranges[0].BaseShaderRegister                = 0;   // t0: glyph instances
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors                    = 1;
    ranges[1].BaseShaderRegister                = 1;   // t1: coverage atlas
    ranges[1].OffsetInDescriptorsFromTableStart = 1;

    D3D12_ROOT_PARAMETER params[2]{};
    params[kParamConstants].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[kParamConstants].Constants.ShaderRegister = 0;
    params[kParamConstants].Constants.Num32BitValues = kRootConstantDwords;
    params[kParamConstants].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

    params[kParamTextures].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kParamTextures].DescriptorTable.NumDescriptorRanges = 2;
    params[kParamTextures].DescriptorTable.pDescriptorRanges   = ranges;
    params[kParamTextures].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    // Linear filtering: glyph quads land on fractional pixel positions because
    // advances are fractional, so point sampling would alias badly.
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister   = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters     = 2;
    rsd.pParameters       = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers   = &sampler;
    rsd.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    const HRESULT sigHr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0,
                                                      &signature, &error);
    if (FAILED(sigHr)) {
        if (error) {
            std::fprintf(stderr, "[rhi] glyph root signature: %s\n",
                         static_cast<const char*>(error->GetBufferPointer()));
        }
        check(sigHr, "D3D12SerializeRootSignature(glyph)");
    }
    check(device->CreateRootSignature(0, signature->GetBufferPointer(),
                                      signature->GetBufferSize(), IID_PPV_ARGS(&rootSig_)),
          "CreateRootSignature(glyph)");

    // ---- pipeline state -------------------------------------------------
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature        = rootSig_.Get();
    pd.VS                    = { g_VSMain, sizeof(g_VSMain) };
    pd.PS                    = { g_PSMain, sizeof(g_PSMain) };
    pd.InputLayout           = { nullptr, 0 };
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
          "CreateGraphicsPipelineState(glyph)");

    // ---- instance ring buffer -------------------------------------------
    const UINT64 instanceBytes =
        static_cast<UINT64>(sizeof(Instance)) * kMaxInstancesPerFrame * kFramesInFlight;

    D3D12_HEAP_PROPERTIES upload{};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = instanceBytes;
    bd.Height           = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels        = 1;
    bd.Format           = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    check(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &bd,
                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                          IID_PPV_ARGS(&instances_)),
          "CreateCommittedResource(glyph instances)");
    check(instances_->Map(0, nullptr, reinterpret_cast<void**>(&mapped_)),
          "Map(glyph instances)");

    // ---- coverage atlas + staging ---------------------------------------
    D3D12_RESOURCE_DESC td{};
    td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width            = atlasSize;
    td.Height           = atlasSize;
    td.DepthOrArraySize = 1;
    td.MipLevels        = 1;
    td.Format           = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags            = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES def{};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;

    check(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td,
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                          nullptr, IID_PPV_ARGS(&atlas_)),
          "CreateCommittedResource(atlas)");

    D3D12_RESOURCE_DESC sd{};
    sd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    sd.Width            = static_cast<UINT64>(atlasRowPitch_) * atlasSize;
    sd.Height           = 1;
    sd.DepthOrArraySize = 1;
    sd.MipLevels        = 1;
    sd.Format           = DXGI_FORMAT_UNKNOWN;
    sd.SampleDesc.Count = 1;
    sd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    check(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &sd,
                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                          IID_PPV_ARGS(&atlasStaging_)),
          "CreateCommittedResource(atlas staging)");
    check(atlasStaging_->Map(0, nullptr, reinterpret_cast<void**>(&mappedAtlas_)),
          "Map(atlas staging)");
    std::memset(mappedAtlas_, 0, static_cast<size_t>(atlasRowPitch_) * atlasSize);

    // ---- descriptors -----------------------------------------------------
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap_)),
          "CreateDescriptorHeap(glyph)");
    srvStride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_CPU_DESCRIPTOR_HANDLE base = srvHeap_->GetCPUDescriptorHandleForHeapStart();

    // t0: glyph instances
    D3D12_SHADER_RESOURCE_VIEW_DESC isd{};
    isd.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
    isd.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    isd.Format                     = DXGI_FORMAT_UNKNOWN;
    isd.Buffer.NumElements         = kMaxInstancesPerFrame * kFramesInFlight;
    isd.Buffer.StructureByteStride = sizeof(Instance);
    device->CreateShaderResourceView(instances_.Get(), &isd, base);

    // t1: coverage atlas
    D3D12_CPU_DESCRIPTOR_HANDLE atlasHandle = base;
    atlasHandle.ptr += srvStride_;

    D3D12_SHADER_RESOURCE_VIEW_DESC asd{};
    asd.Format                    = DXGI_FORMAT_R8_UNORM;
    asd.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
    asd.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    asd.Texture2D.MipLevels       = 1;
    device->CreateShaderResourceView(atlas_.Get(), &asd, atlasHandle);

    return true;
}

void D3D12TextBatch::shutdown() {
    if (mapped_) {
        instances_->Unmap(0, nullptr);
        mapped_ = nullptr;
    }
    if (mappedAtlas_) {
        atlasStaging_->Unmap(0, nullptr);
        mappedAtlas_ = nullptr;
    }
    pso_.Reset();
    rootSig_.Reset();
    instances_.Reset();
    atlas_.Reset();
    atlasStaging_.Reset();
    srvHeap_.Reset();
    device_ = nullptr;
}

void D3D12TextBatch::uploadAtlas(const uint8_t* pixels, uint32_t size) {
    if (!mappedAtlas_ || size != atlasSize_ || !pixels) return;

    // The whole atlas is repacked into the staging buffer. Uploads only happen
    // when a glyph is seen for the first time, so the 1 MiB copy is off the
    // steady-state path; M1 narrows this to the dirty rows the atlas reports.
    for (uint32_t y = 0; y < atlasSize_; ++y) {
        std::memcpy(mappedAtlas_ + static_cast<size_t>(y) * atlasRowPitch_,
                    pixels + static_cast<size_t>(y) * atlasSize_,
                    atlasSize_);
    }
    atlasPending_ = true;
}

void D3D12TextBatch::record(ID3D12GraphicsCommandList* cmd,
                            uint32_t frameIndex,
                            float viewportWidth,
                            float viewportHeight,
                            const GlyphQuad* quads,
                            uint32_t count) {
    if (!pso_ || !cmd) return;

    // Fold any pending atlas upload into this command list. Rare after startup.
    if (atlasPending_) {
        D3D12_RESOURCE_BARRIER toCopy{};
        toCopy.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy.Transition.pResource   = atlas_.Get();
        toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toCopy.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        cmd->ResourceBarrier(1, &toCopy);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource        = atlas_.Get();
        dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource                          = atlasStaging_.Get();
        src.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset             = 0;
        src.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8_UNORM;
        src.PlacedFootprint.Footprint.Width    = atlasSize_;
        src.PlacedFootprint.Footprint.Height   = atlasSize_;
        src.PlacedFootprint.Footprint.Depth    = 1;
        src.PlacedFootprint.Footprint.RowPitch = atlasRowPitch_;
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER toShader{};
        toShader.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toShader.Transition.pResource   = atlas_.Get();
        toShader.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toShader.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        toShader.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        cmd->ResourceBarrier(1, &toShader);

        atlasPending_ = false;
    }

    if (count == 0) return;
    if (count > kMaxInstancesPerFrame) {
        std::fprintf(stderr,
                     "[rhi] drawGlyphQuads: %u quads exceeds the %u-per-frame cap; clipped\n",
                     count, kMaxInstancesPerFrame);
        count = kMaxInstancesPerFrame;
    }

    const uint32_t base = frameIndex * kMaxInstancesPerFrame;
    Instance* dst = mapped_ + base;

    for (uint32_t i = 0; i < count; ++i) {
        const GlyphQuad& q = quads[i];
        dst[i].center[0]   = q.bounds.x + q.bounds.w * 0.5f;
        dst[i].center[1]   = q.bounds.y + q.bounds.h * 0.5f;
        dst[i].halfSize[0] = q.bounds.w * 0.5f;
        dst[i].halfSize[1] = q.bounds.h * 0.5f;
        dst[i].uv[0]       = q.u0;
        dst[i].uv[1]       = q.v0;
        dst[i].uv[2]       = q.u1;
        dst[i].uv[3]       = q.v1;
        dst[i].color[0]    = q.color.r;
        dst[i].color[1]    = q.color.g;
        dst[i].color[2]    = q.color.b;
        dst[i].color[3]    = q.color.a;
    }

    cmd->SetGraphicsRootSignature(rootSig_.Get());
    cmd->SetPipelineState(pso_.Get());

    ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
    cmd->SetDescriptorHeaps(1, heaps);

    const FrameConstants constants{ { viewportWidth, viewportHeight }, base, 0.0f };
    cmd->SetGraphicsRoot32BitConstants(kParamConstants, kRootConstantDwords, &constants, 0);
    cmd->SetGraphicsRootDescriptorTable(kParamConstants + 1,
                                        srvHeap_->GetGPUDescriptorHandleForHeapStart());

    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(6, count, 0, 0);
}

} // namespace tac::rhi
