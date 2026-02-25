#include "TextureLoader.h"
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

// ============================================================
//  Load image via Windows Imaging Component (WIC)
// ============================================================
TextureData LoadTextureWIC(const std::wstring& path)
{
    TextureData td;

    // Initialise COM (safe to call multiple times)
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(
        CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return td;

    // Open file
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder)))
        return td;

    // Get first frame
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)))
        return td;

    // Convert to 32-bit RGBA (handles all source formats)
    ComPtr<IWICFormatConverter> conv;
    factory->CreateFormatConverter(&conv);
    if (FAILED(conv->Initialize(
        frame.Get(),
        GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.0,
        WICBitmapPaletteTypeCustom)))
        return td;

    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    td.width  = w;
    td.height = h;
    td.pixels.resize(w * h * 4);

    conv->CopyPixels(nullptr, w * 4,
        static_cast<UINT>(td.pixels.size()), td.pixels.data());

    td.valid = true;
    return td;
}