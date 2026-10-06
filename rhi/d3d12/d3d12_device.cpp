#include "rhi/d3d12/d3d12_device.h"

#include "tacui/rhi_factory.hpp"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

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

constexpr D3D12_RESOURCE_STATES kStatePresent      = D3D12_RESOURCE_STATE_PRESENT;
constexpr D3D12_RESOURCE_STATES kStateRenderTarget = D3D12_RESOURCE_STATE_RENDER_TARGET;

// BGRA8 is the native Windows surface order and is what WIC's PNG encoder
// accepts directly, so the capture path needs no swizzle.
constexpr DXGI_FORMAT kSurfaceFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

static_assert(kSurfaceFormat == DXGI_FORMAT_B8G8R8A8_UNORM);

} // namespace

std::unique_ptr<Device> createD3D12Device(const SwapchainDesc& desc) {
    return D3D12Device::create(desc);
}

std::unique_ptr<Device> D3D12Device::create(const SwapchainDesc& desc) {
    std::unique_ptr<D3D12Device> dev(new D3D12Device());
    if (!dev->init(desc)) {
        return nullptr;
    }
    return dev;
}

D3D12Device::~D3D12Device() {
    if (device_ && queue_ && fence_) {
        waitForGpu();
    }
    releaseRenderTargets();
    if (fenceEvent_) {
        CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }
}

bool D3D12Device::init(const SwapchainDesc& desc) {
    if (!desc.window) return false;

    width_       = desc.width  ? desc.width  : 1280;
    height_      = desc.height ? desc.height : 720;
    bufferCount_ = desc.bufferCount ? desc.bufferCount : 2;
    if (bufferCount_ > kMaxBuffers) bufferCount_ = kMaxBuffers;

#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
        }
    }
#endif

    ComPtr<IDXGIFactory6> factory;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");

    // Pick the first hardware adapter that can actually create a D3D12 device.
    ComPtr<IDXGIAdapter1> chosen;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT hr = factory->EnumAdapterByGpuPreference(
            i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) break;

        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                        __uuidof(ID3D12Device), nullptr))) {
            chosen = adapter;
            break;
        }
    }
    if (!chosen) {
        std::fprintf(stderr, "[rhi] no D3D12-capable hardware adapter found\n");
        return false;
    }

    check(D3D12CreateDevice(chosen.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)),
          "D3D12CreateDevice");

    // Keep the info queue around so validation messages can be surfaced per
    // frame instead of vanishing into OutputDebugString.
    if (SUCCEEDED(device_.As(&infoQueue_))) {
        D3D12_MESSAGE_SEVERITY deny[] = { D3D12_MESSAGE_SEVERITY_INFO };
        D3D12_INFO_QUEUE_FILTER filter{};
        filter.DenyList.NumSeverities = 1;
        filter.DenyList.pSeverityList = deny;
        infoQueue_->PushStorageFilter(&filter);
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    check(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)), "CreateCommandQueue");

    createSwapchain(factory.Get(), static_cast<HWND>(desc.window));

    check(factory->MakeWindowAssociation(static_cast<HWND>(desc.window), DXGI_MWA_NO_ALT_ENTER),
          "MakeWindowAssociation");

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = bufferCount_;
    check(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap_)), "CreateDescriptorHeap");
    rtvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    createCommandObjects();
    createRenderTargets();

    if (!batch_.init(device_.Get(), kSurfaceFormat)) {
        std::fprintf(stderr, "[rhi] failed to initialise the SDF rect batch\n");
        return false;
    }

    if (!text_.init(device_.Get(), kSurfaceFormat, kGlyphAtlasSize)) {
        std::fprintf(stderr, "[rhi] failed to initialise the glyph batch\n");
        return false;
    }

    check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence");
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent_) {
        std::fprintf(stderr, "[rhi] CreateEventW failed\n");
        return false;
    }

    return true;
}

void D3D12Device::createSwapchain(IDXGIFactory6* factory, HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width            = width_;
    sd.Height           = height_;
    sd.Format           = kSurfaceFormat;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount      = bufferCount_;
    sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode        = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> sc1;
    check(factory->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd, nullptr, nullptr, &sc1),
          "CreateSwapChainForHwnd");
    check(sc1.As(&swapchain_), "IDXGISwapChain3");
}

void D3D12Device::createCommandObjects() {
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        check(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&allocators_[i])),
              "CreateCommandAllocator");
    }
    check(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                     allocators_[0].Get(), nullptr, IID_PPV_ARGS(&cmd_)),
          "CreateCommandList");
    cmd_->Close();
}

void D3D12Device::createRenderTargets() {
    D3D12_CPU_DESCRIPTOR_HANDLE base = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < bufferCount_; ++i) {
        check(swapchain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i])), "SwapChain::GetBuffer");
        D3D12_CPU_DESCRIPTOR_HANDLE dst = base;
        dst.ptr += static_cast<SIZE_T>(i) * rtvStride_;
        device_->CreateRenderTargetView(backBuffers_[i].Get(), nullptr, dst);
        firstUse_[i] = true;
    }
}

void D3D12Device::releaseRenderTargets() {
    for (auto& b : backBuffers_) {
        b.Reset();
    }
}

void D3D12Device::waitForFrame(uint32_t slot) {
    const uint64_t target = fenceValues_[slot];
    if (target == 0) return;
    if (fence_->GetCompletedValue() >= target) return;
    check(fence_->SetEventOnCompletion(target, fenceEvent_), "SetEventOnCompletion");
    WaitForSingleObject(fenceEvent_, INFINITE);
}

void D3D12Device::waitForGpu() {
    const uint64_t target = ++fenceCounter_;
    if (FAILED(queue_->Signal(fence_.Get(), target))) return;
    if (fence_->GetCompletedValue() >= target) return;
    if (FAILED(fence_->SetEventOnCompletion(target, fenceEvent_))) return;
    WaitForSingleObject(fenceEvent_, INFINITE);
}

void D3D12Device::beginFrame(Color clear) {
    waitForFrame(frameIndex_);

    const uint32_t idx = swapchain_->GetCurrentBackBufferIndex();

    check(allocators_[frameIndex_]->Reset(), "CommandAllocator::Reset");
    check(cmd_->Reset(allocators_[frameIndex_].Get(), nullptr), "CommandList::Reset");

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = backBuffers_[idx].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = firstUse_[idx] ? D3D12_RESOURCE_STATE_COMMON : kStatePresent;
    barrier.Transition.StateAfter  = kStateRenderTarget;
    cmd_->ResourceBarrier(1, &barrier);
    firstUse_[idx] = false;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(idx) * rtvStride_;
    cmd_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    const float rgba[4] = { clear.r, clear.g, clear.b, clear.a };
    cmd_->ClearRenderTargetView(rtv, rgba, 0, nullptr);

    // Without an explicit viewport no geometry is rasterised at all.
    const D3D12_VIEWPORT vp{
        0.0f, 0.0f,
        static_cast<float>(width_), static_cast<float>(height_),
        0.0f, 1.0f
    };
    const D3D12_RECT sc{
        0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)
    };
    cmd_->RSSetViewports(1, &vp);
    cmd_->RSSetScissorRects(1, &sc);

    // Draws for this frame are recorded here by drawSdfRects(), before endFrame().
}

void D3D12Device::drawSdfRects(const SdfRect* rects, uint32_t count) {
    batch_.record(cmd_.Get(), frameIndex_,
                  static_cast<float>(width_), static_cast<float>(height_),
                  rects, count);
}

void D3D12Device::setGlyphAtlas(const uint8_t* pixels, uint32_t size) {
    text_.uploadAtlas(pixels, size);
}

void D3D12Device::drawGlyphQuads(const GlyphQuad* quads, uint32_t count) {
    text_.record(cmd_.Get(), frameIndex_,
                 static_cast<float>(width_), static_cast<float>(height_),
                 quads, count);
}

bool D3D12Device::ensureCaptureBuffer(uint32_t width, uint32_t height) {
    if (captureBuffer_ && width == captureWidth_ && height == captureHeight_) {
        return true;
    }

    captureBuffer_.Reset();

    // CopyTextureRegion requires a 256-byte aligned row pitch.
    captureRowPitch_ = (width * 4u + 255u) & ~255u;
    captureWidth_    = width;
    captureHeight_   = height;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = static_cast<UINT64>(captureRowPitch_) * height;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                           IID_PPV_ARGS(&captureBuffer_)),
          "CreateCommittedResource(capture)");
    return true;
}

void D3D12Device::captureFrame() {
    if (!ensureCaptureBuffer(width_, height_)) return;

    const uint32_t idx = swapchain_->GetCurrentBackBufferIndex();

    D3D12_RESOURCE_BARRIER toSource{};
    toSource.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSource.Transition.pResource   = backBuffers_[idx].Get();
    toSource.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toSource.Transition.StateBefore = kStateRenderTarget;
    toSource.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmd_->ResourceBarrier(1, &toSource);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource        = backBuffers_[idx].Get();
    src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource                             = captureBuffer_.Get();
    dst.Type                                  = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset                = 0;
    dst.PlacedFootprint.Footprint.Format      = kSurfaceFormat;
    dst.PlacedFootprint.Footprint.Width       = width_;
    dst.PlacedFootprint.Footprint.Height      = height_;
    dst.PlacedFootprint.Footprint.Depth       = 1;
    dst.PlacedFootprint.Footprint.RowPitch    = captureRowPitch_;

    cmd_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER toTarget{};
    toTarget.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toTarget.Transition.pResource   = backBuffers_[idx].Get();
    toTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toTarget.Transition.StateAfter  = kStateRenderTarget;
    cmd_->ResourceBarrier(1, &toTarget);
}

bool D3D12Device::readbackFrame(std::vector<uint8_t>& out, uint32_t& width, uint32_t& height) {
    if (!captureBuffer_) return false;

    waitForGpu();

    void* mapped = nullptr;
    if (FAILED(captureBuffer_->Map(0, nullptr, &mapped)) || !mapped) {
        std::fprintf(stderr, "[rhi] capture buffer Map failed\n");
        return false;
    }

    width  = captureWidth_;
    height = captureHeight_;
    out.resize(static_cast<size_t>(width) * height * 4);

    const auto* src = static_cast<const uint8_t*>(mapped);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(out.data() + static_cast<size_t>(y) * width * 4,
                    src + static_cast<size_t>(y) * captureRowPitch_,
                    static_cast<size_t>(width) * 4);
    }

    captureBuffer_->Unmap(0, nullptr);
    return true;
}

void D3D12Device::endFrame() {
    const uint32_t idx = swapchain_->GetCurrentBackBufferIndex();

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = backBuffers_[idx].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = kStateRenderTarget;
    barrier.Transition.StateAfter  = kStatePresent;
    cmd_->ResourceBarrier(1, &barrier);

    check(cmd_->Close(), "CommandList::Close");

    ID3D12CommandList* lists[] = { cmd_.Get() };
    queue_->ExecuteCommandLists(1, lists);

    check(swapchain_->Present(1, 0), "Present");   // vsync

    fenceValues_[frameIndex_] = ++fenceCounter_;
    check(queue_->Signal(fence_.Get(), fenceValues_[frameIndex_]), "Signal");

    frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;
}

void D3D12Device::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    if (width == width_ && height == height_) return;

    waitForGpu();
    releaseRenderTargets();

    width_  = width;
    height_ = height;

    check(swapchain_->ResizeBuffers(bufferCount_, width_, height_, DXGI_FORMAT_UNKNOWN, 0),
          "ResizeBuffers");
    createRenderTargets();

    // Every slot is idle after waitForGpu(); restart the pacing.
    frameIndex_ = 0;
    for (auto& v : fenceValues_) v = 0;
}

void D3D12Device::reportDiagnostics() {
    if (!infoQueue_) return;

    const char* label = "info";
    const UINT64  count = infoQueue_->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T len = 0;
        if (FAILED(infoQueue_->GetMessage(i, nullptr, &len)) || len == 0) continue;

        std::vector<char> storage(len);
        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (FAILED(infoQueue_->GetMessage(i, msg, &len))) continue;

        switch (msg->Severity) {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION: label = "corruption"; break;
        case D3D12_MESSAGE_SEVERITY_ERROR:      label = "error";      break;
        case D3D12_MESSAGE_SEVERITY_WARNING:    label = "warning";    break;
        default:                                label = "info";       break;
        }
        std::fprintf(stderr, "[d3d12:%s] %s\n", label, msg->pDescription);
    }

    if (count > 0) {
        infoQueue_->ClearStoredMessages();
    }
}

} // namespace tac::rhi
