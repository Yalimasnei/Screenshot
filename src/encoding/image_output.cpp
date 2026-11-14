// SPDX-License-Identifier: GPL-3.0-only
// Cropping, masking, PNG/JPEG encoding, clipboard and the Screenshots folder.
#include "core/common.h"
#include "encoding/wic_heif.h"
#include "platform/windows_paths.h"
#include <knownfolders.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

std::wstring KnownFolder(REFKNOWNFOLDERID id, DWORD flags) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, flags, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}

static bool WicBufferSizes(int w, int h, size_t bytesPerPixel, UINT& width, UINT& height, UINT& stride, UINT& bytes) {
    if (w <= 0 || h <= 0 || !bytesPerPixel) return false;
    const size_t sw = size_t(w), sh = size_t(h);
    if (sw > UINT_MAX || sh > UINT_MAX || sw > SIZE_MAX / bytesPerPixel) return false;
    const size_t row = sw * bytesPerPixel;
    if (sh > SIZE_MAX / row) return false;
    const size_t total = row * sh;
    if (row > UINT_MAX || total > UINT_MAX) return false;
    width = UINT(w);
    height = UINT(h);
    stride = UINT(row);
    bytes = UINT(total);
    return true;
}

static bool CompleteFile(HANDLE file, const std::vector<BYTE>& data) {
    bool ok = true;
    size_t at = 0;
    while (at < data.size()) {
        const DWORD chunk = DWORD(std::min<size_t>(data.size() - at, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(file, data.data() + at, chunk, &written, nullptr) || !written || written > chunk) {
            ok = false;
            break;
        }
        at += written;
    }
    if (ok) ok = FlushFileBuffers(file) != FALSE;
    if (!CloseHandle(file)) ok = false;
    return ok;
}

bool WriteBytes(const std::wstring& path, const std::vector<BYTE>& data) {
    // Keep the destination intact until a complete sibling file is on disk.
    GUID id{};
    wchar_t suffix[40];
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, suffix, _countof(suffix))) return false;
    const std::wstring dst = ExtendedFilePath(path);
    if (dst.empty()) return false;
    std::wstring dir = dst.substr(0, dst.find_last_of(L"\\/") + 1);
    // A short, independent name avoids exceeding the component limit for a
    // long destination name; the prefix also handles deeply nested folders.
    if (dir.compare(0, 4, L"\\\\?\\") != 0)
        dir = dir.compare(0, 2, L"\\\\") == 0 ? L"\\\\?\\UNC\\" + dir.substr(2) : L"\\\\?\\" + dir;
    const std::wstring tmp = dir + suffix + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool ok = CompleteFile(f, data);
    if (ok) ok = MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

Image Crop(const Image& src, RECT r) {
    r.left = std::max<LONG>(r.left, 0);
    r.top = std::max<LONG>(r.top, 0);
    r.right = std::min<LONG>(r.right, src.w);
    r.bottom = std::min<LONG>(r.bottom, src.h);
    Image out;
    if (r.right <= r.left || r.bottom <= r.top) return out;
    out.w = r.right - r.left;
    out.h = r.bottom - r.top;
    out.bits = src.depths.empty() ? src.bits : 8;
    for (const auto& region : src.depths) {
        RECT overlap;
        if (IntersectRect(&overlap, &region.rect, &r)) {
            OffsetRect(&overlap, -r.left, -r.top);
            out.depths.push_back({overlap, region.bits});
            out.bits = std::max(out.bits, region.bits);
        }
    }
    out.px.resize(size_t(out.w) * out.h);
    if (out.bits > 8 && src.HighPrecision()) out.rgba.resize(out.px.size());
    for (int y = 0; y < out.h; ++y)
        memcpy(&out.px[size_t(y) * out.w], &src.px[size_t(r.top + y) * src.w + r.left], size_t(out.w) * 4);
    if (!out.rgba.empty()) for (int y = 0; y < out.h; ++y)
        memcpy(&out.rgba[size_t(y) * out.w], &src.rgba[size_t(r.top + y) * src.w + r.left], size_t(out.w) * sizeof(Rgba16));
    return out;
}

// Scanline fill, sampling pixel centres; pixels outside become transparent.
void MaskPolygon(Image& img, const std::vector<POINT>& poly) {
    if (poly.size() < 3) return;
    std::vector<float> xs;
    for (int y = 0; y < img.h; ++y) {
        const float cy = y + 0.5f;
        xs.clear();
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
            const float y0 = float(poly[i].y), y1 = float(poly[j].y);
            if ((y0 <= cy) != (y1 <= cy))
                xs.push_back(poly[i].x + (cy - y0) / (y1 - y0) * float(poly[j].x - poly[i].x));
        }
        std::sort(xs.begin(), xs.end());
        uint32_t* row = &img.px[size_t(y) * img.w];
        int x = 0;
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            const int in0 = std::clamp(int(std::ceil(xs[k] - 0.5f)), 0, img.w);
            const int in1 = std::clamp(int(std::ceil(xs[k + 1] - 0.5f)), 0, img.w);
            for (; x < in0; ++x) { row[x] = 0; if (img.HighPrecision()) img.rgba[size_t(y) * img.w + x] = {0, 0, 0, 0}; }
            x = std::max(x, in1);
        }
        for (; x < img.w; ++x) { row[x] = 0; if (img.HighPrecision()) img.rgba[size_t(y) * img.w + x] = {0, 0, 0, 0}; }
    }
}

// Straight-alpha BGRA composited on white (for formats without alpha).
static uint32_t OnWhite(uint32_t p) {
    const uint32_t a = p >> 24;
    if (a == 255) return p;
    uint32_t out = 0xFF000000u;
    for (int s = 0; s < 24; s += 8) out |= ((((p >> s) & 255) * a + 255 * (255 - a) + 127) / 255) << s;
    return out;
}

static bool Encode(REFGUID container, const Image& img, std::vector<BYTE>& out) {
    const bool jpeg = IsEqualGUID(container, GUID_ContainerFormatJpeg);
    const bool heif = IsEqualGUID(container, GUID_ContainerFormatHeif);
    const bool alpha = heif && std::any_of(img.px.begin(), img.px.end(), [](uint32_t p) { return (p >> 24) != 255; });
    UINT width = 0, height = 0, stride = 0, bytes = 0;
    if (img.w <= 0 || img.h <= 0 || size_t(img.w) > SIZE_MAX / size_t(img.h) ||
        img.px.size() != size_t(img.w) * size_t(img.h) ||
        !WicBufferSizes(img.w, img.h, jpeg ? 3 : 4, width, height, stride, bytes))
        return false;
    std::vector<BYTE> px;  // JPEG has no alpha: 24-bit BGR on white
    if (jpeg) {
        if (img.px.size() > SIZE_MAX / 3) return false;
        px.resize(size_t(img.w) * img.h * 3);
        for (size_t i = 0; i < img.px.size(); ++i) {
            const uint32_t p = OnWhite(img.px[i]);
            px[i * 3] = BYTE(p), px[i * 3 + 1] = BYTE(p >> 8), px[i * 3 + 2] = BYTE(p >> 16);
        }
    }
    const GUID want = jpeg ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
    Com<IWICImagingFactory> factory;
    Com<IStream> stream;
    Com<IWICBitmapEncoder> enc;
    Com<IWICBitmapFrameEncode> frame;
    Com<IPropertyBag2> props;
    WICPixelFormatGUID fmt = want;
    BYTE* data = jpeg ? px.data() : reinterpret_cast<BYTE*>(const_cast<uint32_t*>(img.px.data()));
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWICImagingFactory, reinterpret_cast<void**>(&factory))) ||
        FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(factory->CreateEncoder(container, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.p, WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(&frame, &props)))
        return false;
    if (jpeg) {  // quality 0.95
        PROPBAG2 opt = {};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.95f;
        props->Write(1, &opt, &v);
    } else if (heif) {  // best quality; the encoder writes HEVC Main, 8-bit 4:2:0
        PROPBAG2 opt = {};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 1.0f;
        if (FAILED(props->Write(1, &opt, &v))) return false;
    }
    // Lossy HEIF negotiates 32bppBGR: same bytes as BGRA, alpha ignored.
    if (FAILED(frame->Initialize(props.p)) || FAILED(frame->SetSize(width, height)) ||
        FAILED(frame->SetPixelFormat(&fmt)) ||
        (fmt != want && !(heif && IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGR))) ||
        FAILED(frame->WritePixels(height, stride, bytes, data)))
        return false;

    if (alpha) {
        Com<HeifWic::Writer> chain;
        Com<IWICBitmapFrameEncode> mask;
        Com<IPropertyBag2> options;
        BOOL supported = FALSE;
        if (FAILED(frame->QueryInterface(HeifWic::WriterId, reinterpret_cast<void**>(&chain))) ||
            FAILED(chain->DoesSupportChainType(HeifWic::AlphaMap, &supported)) || !supported ||
            FAILED(chain->AppendFrameToChain(HeifWic::AlphaMap, &mask, &options))) return false;
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v{};
        v.vt = VT_R4; v.fltVal = 1.0f;
        if (FAILED(options->Write(1, &opt, &v))) return false;
        // Keep the alpha image HEVC too. WIC's Lossless option instead writes
        // 'unci', which is not supported by common HEIC decoders.
        opt.pstrName = const_cast<LPOLESTR>(L"HeifCompressionMethod");
        v.vt = VT_UI1; v.bVal = 2;  // WICHeifCompressionHEVC
        WICPixelFormatGUID maskFmt = GUID_WICPixelFormat8bppGray;
        if (FAILED(options->Write(1, &opt, &v)) || FAILED(mask->Initialize(options.p)) ||
            FAILED(mask->SetSize(width, height)) || FAILED(mask->SetPixelFormat(&maskFmt))) return false;
        UINT channels = 0;
        if (IsEqualGUID(maskFmt, GUID_WICPixelFormat8bppGray)) channels = 1;
        else if (IsEqualGUID(maskFmt, GUID_WICPixelFormat32bppBGR) ||
                 IsEqualGUID(maskFmt, GUID_WICPixelFormat32bppBGRA)) channels = 4;
        else return false;
        UINT maskWidth = 0, maskHeight = 0, maskStride = 0, maskBytes = 0;
        if (!WicBufferSizes(img.w, img.h, channels, maskWidth, maskHeight, maskStride, maskBytes) ||
            maskWidth != width || maskHeight != height ||
            img.px.size() > SIZE_MAX / channels || img.px.size() * channels != maskBytes)
            return false;
        std::vector<BYTE> samples(img.px.size() * channels);
        for (size_t i = 0; i < img.px.size(); ++i) {
            const BYTE a = BYTE(img.px[i] >> 24);
            for (UINT c = 0; c < channels; ++c) samples[i * channels + c] = c == 3 ? 255 : a;
        }
        if (FAILED(mask->WritePixels(maskHeight, maskStride, maskBytes, samples.data())) ||
            FAILED(mask->Commit())) return false;
    }
    if (FAILED(frame->Commit()) || FAILED(enc->Commit())) return false;

    STATSTG st{};
    HGLOBAL mem = nullptr;
    if (FAILED(stream->Stat(&st, STATFLAG_NONAME)) || FAILED(GetHGlobalFromStream(stream.p, &mem)))
        return false;
    const void* p = GlobalLock(mem);
    if (!p) return false;
    if (st.cbSize.QuadPart > SIZE_MAX) {
        GlobalUnlock(mem);
        return false;
    }
    out.assign(static_cast<const BYTE*>(p), static_cast<const BYTE*>(p) + size_t(st.cbSize.QuadPart));
    GlobalUnlock(mem);
    return true;
}

bool EncodePng(const Image& img, std::vector<BYTE>& out) { return Encode(GUID_ContainerFormatPng, img, out); }
bool EncodeJpeg(const Image& img, std::vector<BYTE>& out) { return Encode(GUID_ContainerFormatJpeg, img, out); }
bool EncodeHeicWindows(const Image& img, std::vector<BYTE>& out) { return Encode(GUID_ContainerFormatHeif, img, out); }

bool HeicWindowsAvailable() {
    static int cached = -1;
    if (cached < 0) {
        Com<IWICImagingFactory> f;
        Com<IWICBitmapEncoder> enc;
        cached = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                            reinterpret_cast<void**>(&f))) &&
                 SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatHeif, nullptr, &enc));
    }
    return cached == 1;
}

// Decodes `file` with WIC and checks it looks like `img`: the same size, and
// colours that match block by block. 4:2:0 HEIC is lossy, so this catches
// wrong colour handling (swapped channels, wrong matrix), not small loss.
std::vector<BYTE>* g_rejectedHeic = nullptr;

static bool LooksRight(const std::vector<BYTE>& file, const Image& img) {
    Com<IWICImagingFactory> f;
    Com<IWICStream> st;
    Com<IWICBitmapDecoder> dec;
    Com<IWICBitmapFrameDecode> fr;
    Com<IWICFormatConverter> cv;
    UINT w = 0, h = 0;
    if (file.empty() || file.size() > MAXDWORD || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                reinterpret_cast<void**>(&f))) ||
        FAILED(f->CreateStream(&st)) ||
        FAILED(st->InitializeFromMemory(const_cast<BYTE*>(file.data()), DWORD(file.size()))) ||
        FAILED(f->CreateDecoderFromStream(st.p, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &fr)) || FAILED(f->CreateFormatConverter(&cv)) ||
        FAILED(cv->Initialize(fr.p, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                              WICBitmapPaletteTypeCustom)) ||
        FAILED(cv->GetSize(&w, &h)) || int(w) != img.w || int(h) != img.h)
        return false;
    std::vector<uint32_t> px(img.px.size());
    if (w > UINT_MAX / 4 || px.size() > UINT_MAX / 4) return false;
    if (FAILED(cv->CopyPixels(nullptr, w * 4, UINT(px.size() * 4), reinterpret_cast<BYTE*>(px.data())))) return false;
    const bool alpha = std::any_of(img.px.begin(), img.px.end(), [](uint32_t p) { return (p >> 24) != 255; });
    if (alpha) {
        // WIC exposes HEIF alpha as an auxiliary frame, rather than merging it
        // into the main frame's BGRA conversion. Check the actual alpha item.
        Com<HeifWic::Reader> chain;
        Com<IWICBitmapFrameDecode> mask;
        Com<IWICFormatConverter> gray;
        UINT count = 0, mw = 0, mh = 0;
        if (FAILED(fr->QueryInterface(HeifWic::ReaderId, reinterpret_cast<void**>(&chain))) ||
            FAILED(chain->GetChainedFrameCount(HeifWic::AlphaMap, &count)) || count != 1 ||
            FAILED(chain->GetChainedFrame(HeifWic::AlphaMap, 0, &mask)) ||
            FAILED(mask->GetSize(&mw, &mh)) || mw != w || mh != h ||
            FAILED(f->CreateFormatConverter(&gray)) ||
            FAILED(gray->Initialize(mask.p, GUID_WICPixelFormat8bppGray, WICBitmapDitherTypeNone, nullptr, 0,
                                    WICBitmapPaletteTypeCustom))) return false;
        std::vector<BYTE> samples(img.px.size());
        if (samples.size() > UINT_MAX || FAILED(gray->CopyPixels(nullptr, w, UINT(samples.size()), samples.data()))) return false;
        for (size_t i = 0; i < samples.size(); ++i) {
            if (std::abs(int(samples[i]) - int(img.px[i] >> 24)) > 4) {
                Log(L"HEIC check: alpha differs at pixel %zu", i);
                return false;
            }
        }
    }
    constexpr int B = 16;
    int worst = 0;
    for (int by = 0; by < img.h; by += B)
        for (int bx = 0; bx < img.w; bx += B) {
            int64_t sa[3] = {}, sb[3] = {}, n = 0;
            for (int y = by; y < std::min(by + B, img.h); ++y)
                for (int x = bx; x < std::min(bx + B, img.w); ++x) {
                    if ((img.px[size_t(y) * img.w + x] >> 24) == 0) continue;
                    ++n;
                    for (int c = 0; c < 3; ++c) {
                        sa[c] += (img.px[size_t(y) * img.w + x] >> (8 * c)) & 255;
                        sb[c] += (px[size_t(y) * img.w + x] >> (8 * c)) & 255;
                    }
                }
            if (!n) continue;
            for (int c = 0; c < 3; ++c) worst = std::max(worst, int((sa[c] > sb[c] ? sa[c] - sb[c] : sb[c] - sa[c]) / n));
        }
    // Windows' HEIC decoder converts saturated colours up to ~25 levels off
    // (ffmpeg decodes the same files within ~7); real mistakes such as RGB
    // read as YCbCr are 100+ levels off. So only reject large differences.
    if (worst > 64) Log(L"HEIC check: a %dx%d block differs by %d levels", B, B, worst);
    return worst <= 64;
}

int DisplayBitDepth() {
    static DWORD at = 0;
    static int cached = 8;
    if (at && GetTickCount() - at < 2000) return cached;
    at = GetTickCount();
    cached = std::max(8, MaxOutputBitsPerColor());
    return cached;
}

// Copy padded to even width/height by repeating the last column/row.
// 4:2:0 HEVC works on 2x2 blocks, and Windows' HEIC decoder resamples
// (blurs) odd-sized pictures instead of cropping them, so keep sizes even.
static Image PadEven(const Image& src) {
    if (src.w % 2 == 0 && src.h % 2 == 0) return src;
    Image out;
    out.w = src.w + src.w % 2;
    out.h = src.h + src.h % 2;
    out.bits = src.bits;
    out.px.resize(size_t(out.w) * out.h);
    if (src.HighPrecision()) out.rgba.resize(out.px.size());
    for (int y = 0; y < out.h; ++y) {
        const uint32_t* s = &src.px[size_t(std::min(y, src.h - 1)) * src.w];
        uint32_t* d = &out.px[size_t(y) * out.w];
        memcpy(d, s, size_t(src.w) * 4);
        if (out.w > src.w) d[src.w] = s[src.w - 1];
        if (!out.rgba.empty()) for (int x = 0; x < out.w; ++x)
            out.rgba[size_t(y) * out.w + x] = src.rgba[size_t(std::min(y, src.h - 1)) * src.w + std::min(x, src.w - 1)];
    }
    return out;
}

bool EncodeHeic(const Image& src, SaveFormat fmt, std::vector<BYTE>& out, std::wstring& why, int* bitsUsed,
                bool yuv444, bool* yuv444Used) {
    out.clear();
    why.clear();
    if (fmt == SaveFormat::HeicNvenc && src.bits > 8 && !src.HighPrecision()) {
        why = Tr(L"缺少原始高精度像素", L"High-precision pixels are missing"); return false;
    }
    if (yuv444 && fmt != SaveFormat::HeicNvenc) { why = Tr(L"Windows HEIC 编码器不支持 4:4:4", L"The Windows HEIC encoder does not support 4:4:4"); return false; }
    const Image img = PadEven(src);
    int bits = 8;
    bool ok = false;
    if (fmt == SaveFormat::HeicNvenc) {
        bits = src.bits > 8 ? 10 : 8;
        ok = EncodeHeicNvenc(img, bits, out, why, yuv444, &yuv444);
    } else if (!HeicWindowsAvailable()) {
        why = Tr(L"未安装 HEIF 图像扩展", L"HEIF Image Extensions is not installed");
    } else if (!(ok = EncodeHeicWindows(img, out))) {
        why = Tr(L"HEIC 编码失败", L"HEIC encoding failed");
    }
    if (ok && !LooksRight(out, img)) {
        ok = false;
        why = Tr(L"HEIC 解码校验失败", L"HEIC verification failed");
        if (g_rejectedHeic) *g_rejectedHeic = out;
    }
    if (ok && bitsUsed) *bitsUsed = bits;
    if (ok && yuv444Used) *yuv444Used = yuv444;
    return ok;
}

Encoded EncodeForSave(const Image& img, SaveFormat fmt, const std::vector<BYTE>& png, bool nvenc444) {
    Encoded e;
    if (fmt != SaveFormat::Png) {
        std::vector<BYTE> heic;
        std::wstring why;
        if (EncodeHeic(img, fmt, heic, why, &e.bits, fmt == SaveFormat::HeicNvenc && nvenc444, &e.yuv444)) {
            e.data = std::move(heic);
            e.ext = L".heic";
            e.used = fmt;
            Log(L"HEIC (%ls): %ls, %d bits", fmt == SaveFormat::HeicNvenc ? L"NVENC" : L"Windows",
                e.yuv444 ? L"4:4:4" : L"4:2:0", e.bits);
            return e;
        }
        e.fallback = why;
        Log(L"HEIC (%ls) not used for %dx%d: %ls", fmt == SaveFormat::HeicNvenc ? L"NVENC" : L"Windows", img.w, img.h,
            why.c_str());
    }
    e.data = png;
    e.bits = 8;
    return e;
}

static bool OpenClipboardRetry(HWND owner) {
    for (int i = 0; i < 20; ++i) {
        if (OpenClipboard(owner)) return true;
        Sleep(25);
    }
    return false;
}

// Puts the image on the clipboard as CF_DIB and as "PNG" (what browsers and
// Office prefer), the same pair Snipping Tool provides. The DIB has no alpha,
// so a free-form snip's outside is white there and transparent in the PNG.
bool CopyToClipboard(HWND owner, const Image& img, const std::vector<BYTE>& png) {
    if (img.w <= 0 || img.h <= 0 || img.px.size() != size_t(img.w) * img.h || png.empty() ||
        img.px.size() > (MAXDWORD - sizeof(BITMAPINFOHEADER)) / 4) return false;
    const size_t pixBytes = img.px.size() * 4;
    HGLOBAL dib = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixBytes);
    HGLOBAL pngMem = GlobalAlloc(GMEM_MOVEABLE, png.size());
    bool ok = false;
    if (dib && pngMem) {
        auto* bih = static_cast<BITMAPINFOHEADER*>(GlobalLock(dib));
        void* pngData = GlobalLock(pngMem);
        if (!bih || !pngData) {
            if (bih) GlobalUnlock(dib);
            if (pngData) GlobalUnlock(pngMem);
            GlobalFree(dib);
            GlobalFree(pngMem);
            return false;
        }
        *bih = {};
        bih->biSize = sizeof(BITMAPINFOHEADER);
        bih->biWidth = img.w;
        bih->biHeight = img.h;  // bottom-up rows
        bih->biPlanes = 1;
        bih->biBitCount = 32;
        bih->biCompression = BI_RGB;
        bih->biSizeImage = DWORD(pixBytes);
        auto* dst = reinterpret_cast<uint32_t*>(bih + 1);
        for (int y = 0; y < img.h; ++y) {
            const uint32_t* s = &img.px[size_t(y) * img.w];
            uint32_t* d = dst + size_t(img.h - 1 - y) * img.w;
            for (int x = 0; x < img.w; ++x) d[x] = OnWhite(s[x]);
        }
        GlobalUnlock(dib);
        memcpy(pngData, png.data(), png.size());
        GlobalUnlock(pngMem);
        const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
        if (pngFormat && OpenClipboardRetry(owner)) {
            if (EmptyClipboard() && SetClipboardData(CF_DIB, dib)) {
                dib = nullptr;
                if (SetClipboardData(pngFormat, pngMem)) {
                    pngMem = nullptr;
                    ok = true;
                } else {
                    // Do not leave a clipboard that contains only half of the result.
                    EmptyClipboard();
                }
            }
            if (!CloseClipboard()) ok = false;
        }
    }
    if (dib) GlobalFree(dib);
    if (pngMem) GlobalFree(pngMem);
    return ok;
}

bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (!mem) return false;
    void* data = GlobalLock(mem);
    if (!data) { GlobalFree(mem); return false; }
    memcpy(data, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(mem);
    bool ok = false;
    if (OpenClipboardRetry(owner)) {
        if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, mem)) { mem = nullptr; ok = true; }
        if (!CloseClipboard()) ok = false;
    }
    if (mem) GlobalFree(mem);
    return ok;
}

std::wstring ScreenshotFileName(const wchar_t* ext) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t base[64];
    swprintf(base, 64, Tr(L"屏幕截图 %04u-%02u-%02u %02u%02u%02u%ls", L"Screenshot %04u-%02u-%02u %02u%02u%02u%ls"), t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond, ext);
    return base;
}

// Saves like Snipping Tool: <save folder>\屏幕截图 2026-11-06 053920.png
std::wstring SaveToScreenshots(const std::vector<BYTE>& data, const wchar_t* ext) {
    std::wstring dir = SaveFolder();
    if (dir.empty()) return {};
    const std::wstring base = ScreenshotFileName(L"");
    for (int n = 0; n < 100; ++n) {
        std::wstring path = dir + L"\\" + base + (n ? L" (" + std::to_wstring(n) + L")" : L"") + ext;
        const std::wstring diskPath = ExtendedFilePath(path);
        if (diskPath.empty()) return {};
        HANDLE f = CreateFileW(diskPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS) continue;
            return {};
        }
        if (CompleteFile(f, data)) return path;
        DeleteFileW(diskPath.c_str());
        return {};
    }
    return {};
}
