// SPDX-License-Identifier: GPL-3.0-only
// Captures every monitor with DXGI Desktop Duplication. HDR monitors are read
// as FP16 scRGB (what DWM composes) and converted with ToneMapper using that
// monitor's SDR white level, with a shoulder for the frame's HDR highlights.
#include "core/common.h"
#include <d3d11.h>
#include <dxgi1_6.h>
#include <dwmapi.h>
#include <algorithm>
#include <cstring>

namespace {
// SDR white level, in scRGB units, of the display with GDI name `gdi`
// (e.g. \\.\DISPLAY1). 0 if unknown.
float SdrWhiteLevel(const wchar_t* gdi) {
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return 0;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS)
        return 0;
    for (UINT32 i = 0; i < np; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof src;
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS || lstrcmpiW(src.viewGdiDeviceName, gdi)) continue;
        DISPLAYCONFIG_SDR_WHITE_LEVEL wl = {};
        wl.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        wl.header.size = sizeof wl;
        wl.header.adapterId = paths[i].targetInfo.adapterId;
        wl.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&wl.header) == ERROR_SUCCESS) return wl.SDRWhiteLevel / 1000.f;
    }
    return 0;
}

struct Shot {
    RECT rc{};
    Image img;
    Image exact;  // HDR outputs only
    bool hdr = false;
};

// Turns texture-space pixels (as scanned out) into desktop orientation.
template<class T> void Unrotate(std::vector<T>& t, int tw, int th, DXGI_MODE_ROTATION rot, std::vector<T>& pixels) {
    const bool swap = rot == DXGI_MODE_ROTATION_ROTATE90 || rot == DXGI_MODE_ROTATION_ROTATE270;
    const int w = swap ? th : tw, h = swap ? tw : th;
    if (rot != DXGI_MODE_ROTATION_ROTATE90 && rot != DXGI_MODE_ROTATION_ROTATE180 && rot != DXGI_MODE_ROTATION_ROTATE270) {
        pixels = std::move(t);
        return;
    }
    pixels.resize(size_t(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int u, v;
            if (rot == DXGI_MODE_ROTATION_ROTATE90) { u = y; v = th - 1 - x; }
            else if (rot == DXGI_MODE_ROTATION_ROTATE180) { u = tw - 1 - x; v = th - 1 - y; }
            else { u = tw - 1 - y; v = x; }
            pixels[size_t(y) * w + x] = t[size_t(v) * tw + u];
        }
}

void UnrotateImage(Image& image, DXGI_MODE_ROTATION rotation) {
    const int w = image.w, h = image.h;
    auto px = std::move(image.px);
    Unrotate(px, w, h, rotation, image.px);
    if (!image.rgba.empty()) {
        auto rgba = std::move(image.rgba);
        Unrotate(rgba, w, h, rotation, image.rgba);
    }
    if (rotation == DXGI_MODE_ROTATION_ROTATE90 || rotation == DXGI_MODE_ROTATION_ROTATE270) {
        image.w = h; image.h = w;
    }
    image.depths = {{{0, 0, image.w, image.h}, image.bits}};
}

bool CaptureOutput(ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGIOutput* output, Shot& shot) {
    Com<IDXGIOutput6> out;
    DXGI_OUTPUT_DESC1 desc{};
    if (FAILED(output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&out))) ||
        FAILED(out->GetDesc1(&desc)) || !desc.AttachedToDesktop)
        return false;
    shot.rc = desc.DesktopCoordinates;
    const bool hdr = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;

    // Preserve native 10-bit SDR surfaces too. Always accept BGRA8 as required
    // by DXGI, but record the returned format instead of claiming extra bits.
    const DXGI_FORMAT hdrFormats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM};
    const DXGI_FORMAT highFormats[] = {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                     DXGI_FORMAT_B8G8R8A8_UNORM};
    const DXGI_FORMAT lowFormats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
    const DXGI_FORMAT* formats = hdr ? hdrFormats : desc.BitsPerColor > 8 ? highFormats : lowFormats;
    const UINT formatCount = hdr ? 2 : desc.BitsPerColor > 8 ? 3 : 1;
    Com<IDXGIOutputDuplication> dup;
    HRESULT hr = out->DuplicateOutput1(dev, 0, formatCount, formats, &dup);
    if (FAILED(hr)) hr = out->DuplicateOutput(dev, &dup);
    if (FAILED(hr)) {
        Log(L"DuplicateOutput(%ls) failed 0x%08X", desc.DeviceName, unsigned(hr));
        return false;
    }
    DXGI_OUTDUPL_DESC dd{};
    dup->GetDesc(&dd);

    // The first frame normally holds the whole desktop; take a later one if
    // it arrives quickly, in case the first is incomplete.
    // The first frame after DuplicateOutput is a blank placeholder (always
    // FP16, LastPresentTime 0), even when BGRA8 was requested. Wait for a real
    // presented frame and size the staging texture from that one.
    Com<ID3D11Texture2D> staging;
    int frames = 0, timeouts = 0;
    const DWORD deadline = GetTickCount() + 1500;
    while (!staging.p && int(deadline - GetTickCount()) > 0) {
        DXGI_OUTDUPL_FRAME_INFO fi{};
        Com<IDXGIResource> res;
        hr = dup->AcquireNextFrame(std::max(1, int(deadline - GetTickCount())), &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) { ++timeouts; continue; }
        if (FAILED(hr)) {
            Log(L"AcquireNextFrame(%ls) failed 0x%08X", desc.DeviceName, unsigned(hr));
            break;
        }
        ++frames;
        Com<ID3D11Texture2D> tex;
        if (fi.LastPresentTime.QuadPart != 0 &&
            SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
            D3D11_TEXTURE2D_DESC td{};
            tex->GetDesc(&td);
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            td.MiscFlags = 0;
            if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &staging))) ctx->CopyResource(staging.p, tex.p);
        }
        dup->ReleaseFrame();
    }
    if (!staging.p) {
        Log(L"%ls: no presented frame (frames %d, timeouts %d)", desc.DeviceName, frames, timeouts);
        return false;
    }

    D3D11_TEXTURE2D_DESC td{};
    staging->GetDesc(&td);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(staging.p, 0, D3D11_MAP_READ, 0, &m))) return false;
    const int tw = int(td.Width), th = int(td.Height);
    const BYTE* src = static_cast<const BYTE*>(m.pData);
    float white = 1.f;
    if (td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        white = SdrWhiteLevel(desc.DeviceName);
        if (white <= 0) {
            white = 2.5f;  // Windows' default "SDR content brightness" (200 nits)
            Log(L"SDR white level of %ls unknown, using %.2f", desc.DeviceName, white);
        }
        Log(L"HDR %ls: SDR white %.4f scRGB (%.1f nits)", desc.DeviceName, white, white * 80);
    }
    CapturePixels format;
    if (td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) format = CapturePixels::ScRgb16;
    else if (td.Format == DXGI_FORMAT_R10G10B10A2_UNORM) format = CapturePixels::Rgb10;
    else if (td.Format == DXGI_FORMAT_R8G8B8A8_UNORM) format = CapturePixels::Rgba8;
    else if (td.Format == DXGI_FORMAT_B8G8R8A8_UNORM) format = CapturePixels::Bgra8;
    else { ctx->Unmap(staging.p, 0); Log(L"Unsupported capture format %d", int(td.Format)); return false; }
    const bool decoded = DecodeCapturePixels(src, m.RowPitch, tw, th, format, int(desc.BitsPerColor), hdr, white,
                                             shot.img, shot.exact);
    ctx->Unmap(staging.p, 0);
    if (!decoded) return false;
    shot.hdr = hdr;
    const auto& px = shot.img.px;
    size_t lit = 0;
    for (size_t i = 0; i < px.size(); i += 97) lit += (px[i] & 0xFFFFFF) != 0;
    Log(L"%ls: colorspace %d, fmt %d, %dx%d, frames %d, timeouts %d, non-black samples %zu/%zu", desc.DeviceName,
        int(desc.ColorSpace), int(td.Format), tw, th, frames, timeouts, lit, (px.size() + 96) / 97);
    Log(L"%ls: output %u-bit, captured %d-bit SDR data", desc.DeviceName, desc.BitsPerColor, shot.img.bits);
    UnrotateImage(shot.img, dd.Rotation);
    if (!shot.exact.px.empty()) UnrotateImage(shot.exact, dd.Rotation);
    return true;
}
}  // namespace

int MaxOutputBitsPerColor() {
    Com<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return 8;
    int best = 0;
    Com<IDXGIAdapter1> adapter;
    for (UINT a = 0; ; ++a) {
        const HRESULT adapterHr = factory->EnumAdapters1(a, &adapter);
        if (adapterHr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(adapterHr) || !adapter.p) return best ? best : 8;
        Com<IDXGIOutput> output;
        for (UINT o = 0; ; ++o) {
            const HRESULT outputHr = adapter->EnumOutputs(o, &output);
            if (outputHr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(outputHr) || !output.p) return best ? best : 8;
            Com<IDXGIOutput6> o6;
            DXGI_OUTPUT_DESC1 d{};
            if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&o6))) &&
                SUCCEEDED(o6->GetDesc1(&d)) && d.AttachedToDesktop)
                best = std::max(best, int(d.BitsPerColor));
            output.p->Release();
            output.p = nullptr;
        }
        adapter.p->Release();
        adapter.p = nullptr;
    }
    return best ? best : 8;
}

bool CaptureDesktop(Desktop& d) {
    d = Desktop{};
    Com<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return false;
    std::vector<Shot> shots;
    Com<IDXGIAdapter1> adapter;
    for (UINT a = 0; ; ++a) {
        const HRESULT adapterHr = factory->EnumAdapters1(a, &adapter);
        if (adapterHr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(adapterHr) || !adapter.p) { Log(L"EnumAdapters1 failed 0x%08X", unsigned(adapterHr)); return false; }
        Com<ID3D11Device> dev;
        Com<ID3D11DeviceContext> ctx;
        bool haveDevice = false;
        Com<IDXGIOutput> output;
        for (UINT o = 0; ; ++o) {
            const HRESULT outputHr = adapter->EnumOutputs(o, &output);
            if (outputHr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(outputHr) || !output.p) { Log(L"EnumOutputs failed 0x%08X", unsigned(outputHr)); return false; }
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc))) return false;
            if (!desc.AttachedToDesktop) { output.p->Release(); output.p = nullptr; continue; }
            if (!haveDevice) {
                haveDevice = SUCCEEDED(D3D11CreateDevice(adapter.p, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                                         D3D11_SDK_VERSION, &dev, nullptr, &ctx));
                if (!haveDevice) { Log(L"could not create a capture device"); return false; }
            }
            Shot s;
            if (!CaptureOutput(dev.p, ctx.p, output.p, s)) return false;
            shots.push_back(std::move(s));
            output.p->Release();
            output.p = nullptr;
        }
        if (output.p) { output.p->Release(); output.p = nullptr; }
        adapter.p->Release();
        adapter.p = nullptr;
    }
    if (shots.empty()) return false;

    RECT b = shots[0].rc;
    for (auto& s : shots) UnionRect(&b, &b, &s.rc);
    d.bounds = b;
    d.img.w = b.right - b.left;
    d.img.h = b.bottom - b.top;
    d.img.px.assign(size_t(d.img.w) * d.img.h, 0xFF000000u);
    d.exact = d.img;
    d.img.bits = 8;
    d.img.depths.clear();
    for (const auto& shot : shots) d.img.bits = std::max(d.img.bits, shot.img.bits);
    if (d.img.bits > 8) d.img.rgba.assign(d.img.px.size(), Rgba16{});
    else d.img.rgba.clear();
    d.monitors.clear();
    d.hdrMonitors = 0;
    for (auto& s : shots) {
        d.monitors.push_back(s.rc);
        d.hdrMonitors += s.hdr;
        const int ox = s.rc.left - b.left, oy = s.rc.top - b.top;
        const int w = std::min(s.img.w, d.img.w - ox), h = std::min(s.img.h, d.img.h - oy);
        const Image& ex = s.exact.px.empty() ? s.img : s.exact;
        d.img.depths.push_back({{ox, oy, ox + w, oy + h}, s.img.bits});
        for (int y = 0; y < h; ++y) {
            memcpy(&d.img.px[size_t(oy + y) * d.img.w + ox], &s.img.px[size_t(y) * s.img.w], size_t(w) * 4);
            memcpy(&d.exact.px[size_t(oy + y) * d.img.w + ox], &ex.px[size_t(y) * ex.w], size_t(w) * 4);
            if (d.img.bits > 8) for (int x = 0; x < w; ++x)
                d.img.rgba[size_t(oy + y) * d.img.w + ox + x] = s.img.Pixel16(size_t(y) * s.img.w + x);
        }
    }
    return true;
}

// Candidates for "click to capture a window", topmost first.
std::vector<RECT> VisibleWindowRects() {
    std::vector<RECT> out;
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid == GetCurrentProcessId()) return TRUE;
            BOOL cloaked = FALSE;
            DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
            if (cloaked) return TRUE;
            const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
            if ((ex & WS_EX_TRANSPARENT) && (ex & WS_EX_LAYERED)) return TRUE;  // click-through overlays
            wchar_t cls[32];
            GetClassNameW(h, cls, 32);
            if (!lstrcmpW(cls, L"Progman") || !lstrcmpW(cls, L"WorkerW")) return TRUE;  // desktop
            RECT r;
            if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r)) && !GetWindowRect(h, &r))
                return TRUE;
            if (r.right - r.left < 8 || r.bottom - r.top < 8) return TRUE;
            reinterpret_cast<std::vector<RECT>*>(l)->push_back(r);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));
    return out;
}
