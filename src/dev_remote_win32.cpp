// -----------------------------------------------------------------------------
// dev_remote_win32.cpp — Windows window screenshot for the dev remote (#696).
//
// The Windows counterpart of dev_remote_macos.mm. maybe_capture_screenshot()
// runs on the main thread right after sg_commit() and BEFORE sokol_app calls
// Present(), so the swap chain's back buffer still holds the finished frame
// (scene + HUD + ImGui). We copy it into a CPU-readable staging texture and
// let GDI+ encode the PNG.
//
// Reading the GPU buffer rather than the desktop means the shot is correct
// even when another window covers the game.
// -----------------------------------------------------------------------------

#include "sokol_app.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// gdiplus.h leans on the min/max macros that NOMINMAX (CMakeLists) removes.
// WIN32_LEAN_AND_MEAN drops objidl.h (IStream), which gdiplus.h needs.
namespace Gdiplus { using std::min; using std::max; }
#include <objidl.h>
#include <gdiplus.h>

extern "C" bool dev_remote_capture_window(const char* path);

namespace {

// GDI+'s built-in PNG encoder. The CLSID is fixed across Windows versions,
// which spares us the GetImageEncoders() enumeration dance.
const CLSID kPngEncoder = { 0x557cf406, 0x1a04, 0x11d3,
                            { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };

// Releases a COM pointer at scope exit so the early-outs below can't leak.
template <typename T>
struct ComRef {
    T* p = nullptr;
    ~ComRef() { if (p) p->Release(); }
};

std::wstring widen(const char* utf8) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring out(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), n);
    return out;
}

// Copies the current back buffer into tightly packed BGRA rows.
bool read_back_buffer(std::vector<uint8_t>& bgra, UINT& w, UINT& h) {
    // sokol_app only exposes the swap chain; the device and its immediate
    // context hang off it.
    auto* sc = (IDXGISwapChain*)sapp_d3d11_get_swap_chain();
    if (!sc) return false;
    ComRef<ID3D11Device> dev;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev.p))) return false;
    ComRef<ID3D11DeviceContext> ctx;
    dev.p->GetImmediateContext(&ctx.p);

    ComRef<ID3D11Texture2D> back;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back.p))) return false;

    D3D11_TEXTURE2D_DESC desc{};
    back.p->GetDesc(&desc);
    if (desc.SampleDesc.Count != 1) {
        // MSAA is off everywhere (render_config.h); a resolve step would go
        // here if that ever changes.
        std::fprintf(stderr, "[dev_remote] multisampled back buffer not supported\n");
        return false;
    }
    // The swap chain is BGRA8 (render_config.h). Anything else would need
    // a swizzle; fail loudly rather than write a colour-swapped PNG.
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        std::fprintf(stderr, "[dev_remote] unexpected back buffer format %d\n",
                     (int)desc.Format);
        return false;
    }
    w = desc.Width;
    h = desc.Height;

    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage          = D3D11_USAGE_STAGING;
    sd.BindFlags      = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags      = 0;
    ComRef<ID3D11Texture2D> staging;
    if (FAILED(dev.p->CreateTexture2D(&sd, nullptr, &staging.p))) return false;
    ctx.p->CopyResource(staging.p, back.p);

    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx.p->Map(staging.p, 0, D3D11_MAP_READ, 0, &map))) return false;
    bgra.resize((size_t)w * h * 4);
    for (UINT y = 0; y < h; ++y) {
        std::memcpy(&bgra[(size_t)y * w * 4],
                    (const uint8_t*)map.pData + (size_t)y * map.RowPitch,
                    (size_t)w * 4);
    }
    ctx.p->Unmap(staging.p, 0);
    return true;
}

bool write_png(const char* path, std::vector<uint8_t>& bgra, UINT w, UINT h) {
    Gdiplus::GdiplusStartupInput in;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &in, nullptr) != Gdiplus::Ok) return false;
    bool ok = false;
    {
        // 32bppRGB ignores alpha: the swap chain's alpha is meaningless.
        Gdiplus::Bitmap bmp((INT)w, (INT)h, (INT)(w * 4),
                            PixelFormat32bppRGB, bgra.data());
        ok = bmp.Save(widen(path).c_str(), &kPngEncoder, nullptr) == Gdiplus::Ok;
    }   // Bitmap must die before GdiplusShutdown.
    Gdiplus::GdiplusShutdown(token);
    return ok;
}

} // namespace

bool dev_remote_capture_window(const char* path) {
    std::vector<uint8_t> bgra;
    UINT w = 0, h = 0;
    if (!read_back_buffer(bgra, w, h)) {
        std::fprintf(stderr, "[dev_remote] back buffer readback failed\n");
        return false;
    }
    if (!write_png(path, bgra, w, h)) {
        std::fprintf(stderr, "[dev_remote] PNG write failed: %s\n", path);
        return false;
    }
    return true;
}
