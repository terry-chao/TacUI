#include "tools/image_writer.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace tac::tools {

namespace {

struct ComInit {
    bool owned = false;
    ComInit() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        owned = SUCCEEDED(hr);
    }
    ~ComInit() {
        if (owned) CoUninitialize();
    }
};

} // namespace

bool writePng(const wchar_t* path, const uint8_t* rgba, uint32_t width, uint32_t height) {
    if (!path || !rgba || width == 0 || height == 0) return false;

    ComInit com;

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        std::fprintf(stderr, "[image] CoCreateInstance(WICImagingFactory) failed\n");
        return false;
    }

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) {
        std::fprintf(stderr, "[image] could not open output stream\n");
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
        std::fprintf(stderr, "[image] could not create PNG encoder\n");
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2>         props;
    if (FAILED(encoder->CreateNewFrame(&frame, &props)) ||
        FAILED(frame->Initialize(props.Get())) ||
        FAILED(frame->SetSize(width, height))) {
        std::fprintf(stderr, "[image] could not create PNG frame\n");
        return false;
    }

    // BGRA8 is what the D3D12 surface uses and what the PNG encoder takes
    // natively, so no conversion happens along the way.
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format))) {
        std::fprintf(stderr, "[image] PNG encoder rejected 32bppBGRA\n");
        return false;
    }
    if (!IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) {
        std::fprintf(stderr, "[image] PNG encoder silently converted the pixel format\n");
        return false;
    }

    const UINT stride = width * 4u;
    if (FAILED(frame->WritePixels(height, stride, stride * height,
                                  const_cast<BYTE*>(rgba)))) {
        std::fprintf(stderr, "[image] WritePixels failed\n");
        return false;
    }

    if (FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        std::fprintf(stderr, "[image] commit failed\n");
        return false;
    }
    return true;
}

} // namespace tac::tools
