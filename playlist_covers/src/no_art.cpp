#include "no_art.h"

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <map>
#include <vector>

namespace covers {

namespace {

using Microsoft::WRL::ComPtr;

constexpr int kNotFoundResource = 169; // IDR_IMAGE_NOTFOUND in ml_local's resource.h

struct Decoded {
    int width = 0, height = 0;
    std::vector<std::uint32_t> pixels; // premultiplied BGRA
    bool tried = false;
};

Decoded g_decoded;
std::map<COLORREF, HBITMAP> g_tinted;

// ml_local's PNG, decoded once.
const Decoded& Picture() {
    if (g_decoded.tried) return g_decoded;
    g_decoded.tried = true;
    const HMODULE library = ::GetModuleHandleW(L"ml_local.dll");
    if (!library) return g_decoded;
    const HRSRC found = ::FindResourceW(library, MAKEINTRESOURCEW(kNotFoundResource), L"PNG");
    const HGLOBAL loaded = found ? ::LoadResource(library, found) : nullptr;
    const auto* bytes = loaded ? static_cast<const BYTE*>(::LockResource(loaded)) : nullptr;
    const DWORD size = found ? ::SizeofResource(library, found) : 0;
    if (!bytes || !size) return g_decoded;

    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapSource> converted;
    UINT width = 0, height = 0;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes), size)) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(::WICConvertBitmapSource(GUID_WICPixelFormat32bppPBGRA, frame.Get(), &converted)) ||
        FAILED(converted->GetSize(&width, &height)) || width == 0 || height == 0 || width > 1024 || height > 1024) {
        return g_decoded;
    }
    std::vector<std::uint32_t> pixels(static_cast<size_t>(width) * height);
    if (FAILED(converted->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size() * 4),
                                     reinterpret_cast<BYTE*>(pixels.data())))) {
        return g_decoded;
    }
    g_decoded.width = static_cast<int>(width);
    g_decoded.height = static_cast<int>(height);
    g_decoded.pixels = std::move(pixels);
    return g_decoded;
}

} // namespace

HBITMAP NoArtPicture(COLORREF tint, int& size) {
    const Decoded& picture = Picture();
    size = picture.width;
    if (picture.pixels.empty() || picture.width != picture.height) return nullptr;
    if (const auto found = g_tinted.find(tint); found != g_tinted.end()) return found->second;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = picture.width;
    info.bmiHeader.biHeight = -picture.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    const HBITMAP bitmap = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) return nullptr;
    // Each channel times the tint's, as ml_local's adjustbmp does: a white llama comes out
    // in the tint, its see-through parts stay see-through.
    auto* out = static_cast<std::uint32_t*>(bits);
    const std::uint32_t red = GetRValue(tint), green = GetGValue(tint), blue = GetBValue(tint);
    for (size_t i = 0; i < picture.pixels.size(); ++i) {
        const std::uint32_t pixel = picture.pixels[i];
        const std::uint32_t b = (pixel & 0xFF) * blue / 255, g = ((pixel >> 8) & 0xFF) * green / 255;
        const std::uint32_t r = ((pixel >> 16) & 0xFF) * red / 255;
        out[i] = (pixel & 0xFF000000) | r << 16 | g << 8 | b;
    }
    g_tinted.emplace(tint, bitmap);
    return bitmap;
}

void FreeNoArtPictures() {
    for (auto& [tint, bitmap] : g_tinted) ::DeleteObject(bitmap);
    g_tinted.clear();
    g_decoded = {};
}

} // namespace covers
