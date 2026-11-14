// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <cstdint>
#include <string>
#include <vector>

struct Rgba16 { uint16_t r = 0, g = 0, b = 0, a = 65535; };
struct DepthRegion { RECT rect{}; int bits = 8; };
// BGRA8 is a preview/compatibility copy. High-depth captures keep independent
// sRGB RGBA16 samples until the final encoder; bits records the captured output.
struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    int bits = 8;
    std::vector<Rgba16> rgba;
    std::vector<DepthRegion> depths;  // monitor-local regions, in image coordinates
    bool HighPrecision() const { return bits > 8 && rgba.size() == size_t(w) * h && !rgba.empty(); }
    Rgba16 Pixel16(size_t i) const {
        if (HighPrecision()) return rgba[i];
        const uint32_t p = px[i];
        return {uint16_t(((p >> 16) & 255) * 257), uint16_t(((p >> 8) & 255) * 257),
                uint16_t((p & 255) * 257), uint16_t((p >> 24) * 257)};
    }
};
void UpdatePreview(Image& img);
// Blend an 8-bit straight-alpha annotation layer over the retained RGBA16 base.
void CompositeAnnotations(Image& img, const Image& layer);

// Minimal COM smart pointer.
template <class T> struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};

// dwExtraInfo tag on keystrokes we inject, so our own hook ignores them.
constexpr ULONG_PTR kInjectTag = 0x5343524E;  // 'SCRN'

// app/application.cpp
void Log(const wchar_t* fmt, ...);  // appends to %TEMP%\Screenshot.log
std::wstring SaveFolder();          // custom save folder if usable, else Pictures\Screenshots
// Interface language: 0 = Windows, 1 = Chinese, 2 = English.
int GetLanguageSetting();
void SetLanguageSetting(int value);
void RefreshLanguage();
bool English();
const wchar_t* Tr(const wchar_t* chinese, const wchar_t* english);

// capture/tone_mapping.cpp: HDR (scRGB FP16) -> sRGB SDR, preserving midtones and applying
// a shoulder to HDR highlights. `sdrWhite` is the display's SDR white in scRGB
// (Windows "SDR content brightness": SDRWhiteLevel / 1000, 1.0 = 80 nits).
class ToneMapper {
  public:
    explicit ToneMapper(float sdrWhite);
    // src: R16G16B16A16_FLOAT rows. dst: BGRA, dstPitch in pixels.
    void Map(const BYTE* src, size_t srcPitch, int w, int h, uint32_t* dst, size_t dstPitch) const;
    // The colour an SDR app drew: x / sdrWhite, plain sRGB encoding, no tone
    // mapping. Used by the colour picker so it reports the app's real value.
    void MapExact(const BYTE* src, size_t srcPitch, int w, int h, uint32_t* dst, size_t dstPitch) const;
    void Map16(const BYTE* src, size_t srcPitch, int w, int h, Rgba16* dst, size_t dstPitch,
               bool highlights = true) const;

  private:
    void Rows(const BYTE* src, size_t srcPitch, int w, int y0, int y1, uint32_t* dst, size_t dstPitch,
              const float* scale) const;
    float white_ = 1;
    std::vector<uint8_t> srgb_;   // linear [0,1] -> 8-bit sRGB code
    std::vector<float> srgbFloat_;  // unquantised OETF, interpolated before RGBA16 quantisation
    std::vector<float> half_;     // half bits -> float
};

// capture/desktop_capture.cpp: every monitor via DXGI Desktop Duplication.
struct Desktop {
    Image img;                  // whole virtual screen, tone mapped to sRGB SDR
    Image exact;                // same, with SDR colours exactly as apps drew them
    RECT bounds{};              // virtual screen, physical pixels
    std::vector<RECT> monitors;
    int hdrMonitors = 0;
};
bool CaptureDesktop(Desktop& out);
enum class CapturePixels { Bgra8, Rgba8, Rgb10, ScRgb16 };
bool DecodeCapturePixels(const BYTE* src, size_t pitch, int w, int h, CapturePixels format, int outputBits,
                         bool hdr, float white, Image& image, Image& exact);
std::vector<RECT> VisibleWindowRects();  // top-level windows, z-order top first

// encoding/image_output.cpp
std::wstring KnownFolder(REFKNOWNFOLDERID id, DWORD flags);
Image Crop(const Image& src, RECT r);
void MaskPolygon(Image& img, const std::vector<POINT>& poly);  // alpha 0 outside (even-odd)
bool EncodePng(const Image& img, std::vector<BYTE>& out);
bool EncodeJpeg(const Image& img, std::vector<BYTE>& out);  // flattened on white
bool EncodeHeicWindows(const Image& img, std::vector<BYTE>& out);  // WIC HEIF encoder: HEVC Main, 8-bit 4:2:0
bool CopyToClipboard(HWND owner, const Image& img, const std::vector<BYTE>& png);
bool CopyTextToClipboard(HWND owner, const std::wstring& text);

// Save formats (setting "SaveFormat").
enum class SaveFormat { Png = 0, HeicNvenc = 1, HeicWindows = 2 };
struct Encoded {
    std::vector<BYTE> data;
    const wchar_t* ext = L".png";
    SaveFormat used = SaveFormat::Png;
    int bits = 8;           // encoded file depth (PNG and Windows HEIC remain 8-bit)
    bool yuv444 = false;    // actual output after NVENC capability fallback
    std::wstring fallback;  // why PNG was used instead of the chosen format (empty if not)
};
// HEIC as Y'CbCr (BT.709). NVENC offers 4:2:0 or 4:4:4; Windows uses 4:2:0.
// Odd sizes are padded by one row/column. NVENC uses the captured region's depth
// within its 8/10-bit support. Windows HEIC keeps its existing 8-bit path.
// Transparent images carry an auxiliary alpha image; NVENC uses 8-bit 4:2:0
// for this path. Both colour and alpha are decoded back and checked.
bool EncodeHeic(const Image& img, SaveFormat fmt, std::vector<BYTE>& out, std::wstring& why, int* bitsUsed,
                bool yuv444 = false, bool* yuv444Used = nullptr);
// For saving to disk: HEIC if possible, otherwise `png` with the reason.
Encoded EncodeForSave(const Image& img, SaveFormat fmt, const std::vector<BYTE>& png, bool nvenc444 = false);
bool HeicWindowsAvailable();
int DisplayBitDepth();  // highest bits per colour any display currently outputs (8, 10, ...)
extern std::vector<BYTE>* g_rejectedHeic;  // test hook: receives a HEIC the colour check rejected

// capture/desktop_capture.cpp
int MaxOutputBitsPerColor();

// encoding/nvenc_encoder.cpp: HEIC via the NVIDIA driver's video encoder.
struct NvencCapabilities {
    bool available = false, tenBit = false, yuv444 = false, yuv444TenBit = false;
    bool Supports(int bits, bool chroma444) const {
        return available && (chroma444 ? (bits >= 10 ? yuv444TenBit : yuv444)
                                       : (bits >= 10 ? tenBit : true));
    }
};
const NvencCapabilities& GetNvencCapabilities();
bool NvencAvailable();
bool EncodeHeicNvenc(const Image& img, int& bits, std::vector<BYTE>& out, std::wstring& why,
                     bool yuv444 = false, bool* yuv444Used = nullptr);

std::wstring ScreenshotFileName(const wchar_t* ext = L".png");  // "屏幕截图 2026-11-06 053920.png"
std::wstring SaveToScreenshots(const std::vector<BYTE>& data, const wchar_t* ext = L".png");
bool WriteBytes(const std::wstring& path, const std::vector<BYTE>& data);

// platform/text_recognition.cpp: Windows.Media.Ocr. Blocking; call from a worker thread.
HRESULT RecognizeText(const Image& img, std::wstring& text);

// ui/selection_overlay.cpp: Snipping Tool style capture screen over the frozen desktop,
// with its mode bar. `img` covers `screen` 1:1 and must outlive the overlay.
enum class SnipMode { Rect, Window, Full, Free, Text, Color };
struct SnipResult {
    enum Kind { Cancel, Picture, Text, Color } kind = Cancel;
    RECT rect{};               // image pixels
    std::vector<POINT> poly;   // free-form outline, image pixels (Picture only)
    uint32_t color = 0;        // BGRA (Color only)
    SnipMode mode = SnipMode::Rect;  // mode the capture finished in
};
using SnipDone = void (*)(const SnipResult& r);
// `exact` (same size as `img`) is what the colour picker samples.
bool ShowOverlay(const Image& img, const Image& exact, const RECT& screen, const std::vector<RECT>& windows,
                 const std::vector<RECT>& monitors, SnipMode mode, SnipDone done);
