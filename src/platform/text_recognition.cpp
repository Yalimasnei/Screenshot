// SPDX-License-Identifier: GPL-3.0-only
// Text extraction with the OCR engine built into Windows (Windows.Media.Ocr),
// the same one Snipping Tool's text actions use. llvm-mingw ships no header
// for it, so the few WinRT interfaces needed are declared here by hand; IIDs
// and method order follow the Windows Runtime metadata for Windows.Media.Ocr.
#include "core/common.h"
#include <inspectable.h>
#include <roapi.h>
#include <winstring.h>
#include <algorithm>
#include <cstring>

namespace {
struct IVectorViewX : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32 index, IInspectable** item) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32* size) = 0;
};
struct IBufferX : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Capacity(UINT32* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Length(UINT32* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Length(UINT32 v) = 0;
};
struct IBufferFactoryX : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Create(UINT32 capacity, IBufferX** value) = 0;
};
struct IBufferByteAccessX : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Buffer(BYTE** value) = 0;
};
struct ISoftwareBitmapStaticsX : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Copy(IInspectable*, IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE Convert(IInspectable*, int, IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ConvertWithAlpha(IInspectable*, int, int, IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateCopyFromBuffer(IBufferX* source, int format, INT32 w, INT32 h,
                                                           IInspectable** value) = 0;
};
struct IAsyncInfoX : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32* id) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int* status) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT* code) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};
struct IOcrResultX;
struct IAsyncOcrResultX : IInspectable {  // IAsyncOperation<OcrResult>
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown** handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(IOcrResultX** result) = 0;
};
struct IOcrEngineX : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RecognizeAsync(IInspectable* bitmap, IAsyncOcrResultX** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_RecognizerLanguage(IInspectable** lang) = 0;
};
struct IOcrEngineStaticsX : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_MaxImageDimension(UINT32* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AvailableRecognizerLanguages(IVectorViewX** v) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsLanguageSupported(IInspectable* lang, unsigned char* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryCreateFromLanguage(IInspectable* lang, IOcrEngineX** v) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryCreateFromUserProfileLanguages(IOcrEngineX** v) = 0;
};
struct IOcrResultX : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Lines(IVectorViewX** v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TextAngle(IInspectable** v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Text(HSTRING* v) = 0;
};
struct IOcrLineX : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Words(IVectorViewX** v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Text(HSTRING* v) = 0;
};
struct IOcrWordX : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_BoundingRect(float rect[4]) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Text(HSTRING* v) = 0;
};

const IID IID_BufferFactory = {0x71af914d, 0xc10f, 0x484b, {0xbc, 0x50, 0x14, 0xbc, 0x62, 0x3b, 0x3a, 0x27}};
const IID IID_BufferByteAccess = {0x905a0fef, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};
const IID IID_SoftwareBitmapStatics = {0xdf0385db, 0x672f, 0x4a9d, {0x80, 0x6e, 0xc2, 0x44, 0x2f, 0x34, 0x3e, 0x86}};
const IID IID_OcrEngineStatics = {0x5bffa85a, 0x3384, 0x3540, {0x99, 0x40, 0x69, 0x91, 0x20, 0xd4, 0x28, 0xa8}};
const IID IID_AsyncInfo = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const IID IID_OcrLine = {0x0043a16f, 0xe31f, 0x3a24, {0x89, 0x9c, 0xd4, 0x44, 0xbd, 0x08, 0x81, 0x24}};

template <class T> HRESULT Factory(const wchar_t* cls, const IID& iid, Com<T>& out) {
    HSTRING s = nullptr;
    HRESULT hr = WindowsCreateString(cls, UINT32(wcslen(cls)), &s);
    if (SUCCEEDED(hr)) hr = RoGetActivationFactory(s, iid, reinterpret_cast<void**>(&out));
    WindowsDeleteString(s);
    return hr;
}

std::wstring Str(HSTRING s) {
    UINT32 n = 0;
    const wchar_t* p = WindowsGetStringRawBuffer(s, &n);
    std::wstring r(p ? p : L"", n);
    WindowsDeleteString(s);
    return r;
}

bool IsCjk(wchar_t c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF) ||
           (c >= 0xAC00 && c <= 0xD7AF);
}

// The engine's line text, minus the spaces it puts between CJK characters
// (Windows OCR treats every Chinese character as a separate word).
std::wstring LineText(IOcrLineX* line) {
    HSTRING s = nullptr;
    if (FAILED(line->get_Text(&s))) return {};
    const std::wstring in = Str(s);
    std::wstring out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == L' ' && !out.empty() && i + 1 < in.size() && IsCjk(out.back()) && IsCjk(in[i + 1])) continue;
        out += in[i];
    }
    return out;
}
Image Upscale(const Image& src, int scale) {  // bilinear
    if (scale <= 1) return src;
    Image img;
    img.w = src.w * scale;
    img.h = src.h * scale;
    img.px.resize(size_t(img.w) * img.h);
    for (int y = 0; y < img.h; ++y) {
        const float fy = std::max(0.f, (y + 0.5f) / scale - 0.5f);
        const int y0 = std::min(int(fy), src.h - 1), y1 = std::min(y0 + 1, src.h - 1);
        const float wy = fy - y0;
        for (int x = 0; x < img.w; ++x) {
            const float fx = std::max(0.f, (x + 0.5f) / scale - 0.5f);
            const int x0 = std::min(int(fx), src.w - 1), x1 = std::min(x0 + 1, src.w - 1);
            const float wx = fx - x0;
            const uint32_t a = src.px[size_t(y0) * src.w + x0], b = src.px[size_t(y0) * src.w + x1],
                           c = src.px[size_t(y1) * src.w + x0], d = src.px[size_t(y1) * src.w + x1];
            uint32_t out = 0xFF000000u;
            for (int s = 0; s < 24; s += 8) {
                const float top = ((a >> s) & 255) * (1 - wx) + ((b >> s) & 255) * wx;
                const float bot = ((c >> s) & 255) * (1 - wx) + ((d >> s) & 255) * wx;
                out |= uint32_t(top * (1 - wy) + bot * wy + 0.5f) << s;
            }
            img.px[size_t(y) * img.w + x] = out;
        }
    }
    return img;
}

HRESULT Recognize(const Image& img, std::wstring& text) {
    HRESULT hr = S_OK;
    text.clear();
    do {
        const UINT32 bytes = UINT32(img.px.size() * 4);
        Com<IBufferFactoryX> bufFactory;
        Com<IBufferX> buf;
        Com<IBufferByteAccessX> access;
        BYTE* data = nullptr;
        if (FAILED(hr = Factory(L"Windows.Storage.Streams.Buffer", IID_BufferFactory, bufFactory)) ||
            FAILED(hr = bufFactory->Create(bytes, &buf)) ||
            FAILED(hr = buf->QueryInterface(IID_BufferByteAccess, reinterpret_cast<void**>(&access))) ||
            FAILED(hr = access->Buffer(&data)))
            break;
        for (size_t i = 0; i < img.px.size(); ++i) {  // opaque BGRA
            const uint32_t p = img.px[i] | 0xFF000000u;
            memcpy(data + i * 4, &p, 4);
        }
        buf->put_Length(bytes);

        Com<ISoftwareBitmapStaticsX> sbStatics;
        Com<IInspectable> bitmap;
        Com<IOcrEngineStaticsX> ocrStatics;
        Com<IOcrEngineX> engine;
        if (FAILED(hr = Factory(L"Windows.Graphics.Imaging.SoftwareBitmap", IID_SoftwareBitmapStatics, sbStatics)) ||
            FAILED(hr = sbStatics->CreateCopyFromBuffer(buf.p, 87 /* Bgra8 */, img.w, img.h, &bitmap)) ||
            FAILED(hr = Factory(L"Windows.Media.Ocr.OcrEngine", IID_OcrEngineStatics, ocrStatics)))
            break;
        ocrStatics->TryCreateFromUserProfileLanguages(&engine);
        if (!engine.p) {  // fall back to any installed OCR language
            Com<IVectorViewX> langs;
            Com<IInspectable> lang;
            UINT32 n = 0;
            if (SUCCEEDED(ocrStatics->get_AvailableRecognizerLanguages(&langs)) && SUCCEEDED(langs->get_Size(&n)) &&
                n > 0 && SUCCEEDED(langs->GetAt(0, &lang)))
                ocrStatics->TryCreateFromLanguage(lang.p, &engine);
        }
        if (!engine.p) { hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED); break; }

        Com<IAsyncOcrResultX> op;
        Com<IAsyncInfoX> info;
        if (FAILED(hr = engine->RecognizeAsync(bitmap.p, &op)) ||
            FAILED(hr = op->QueryInterface(IID_AsyncInfo, reinterpret_cast<void**>(&info))))
            break;
        int status = 0;  // 0 Started, 1 Completed, 2 Canceled, 3 Error
        for (int i = 0; i < 3000 && SUCCEEDED(info->get_Status(&status)) && status == 0; ++i) Sleep(10);
        if (status != 1) {
            info->get_ErrorCode(&hr);
            if (SUCCEEDED(hr)) hr = E_FAIL;
            break;
        }
        Com<IOcrResultX> result;
        Com<IVectorViewX> lines;
        UINT32 n = 0;
        if (FAILED(hr = op->GetResults(&result)) || FAILED(hr = result->get_Lines(&lines)) ||
            FAILED(hr = lines->get_Size(&n)))
            break;
        for (UINT32 i = 0; i < n; ++i) {
            Com<IInspectable> item;
            Com<IOcrLineX> line;
            if (FAILED(lines->GetAt(i, &item)) ||
                FAILED(item->QueryInterface(IID_OcrLine, reinterpret_cast<void**>(&line))))
                continue;
            if (!text.empty()) text += L"\r\n";
            text += LineText(line.p);
        }
    } while (false);
    return hr;
}

size_t Letters(const std::wstring& s) {
    size_t n = 0;
    for (wchar_t c : s) n += c != L' ' && c != L'\r' && c != L'\n';
    return n;
}
}  // namespace

// Runs on any thread; initialises the WinRT apartment itself. Screen text
// recognises best enlarged: 2x suits 14-24 px text, 3x the smallest UI text
// in the tested language combinations, so both run and the fuller result wins.
HRESULT RecognizeText(const Image& src, std::wstring& text) {
    const HRESULT init = RoInitialize(RO_INIT_MULTITHREADED);
    const int maxScale = 10000 / std::max(src.w, src.h);
    HRESULT hr = Recognize(Upscale(src, std::max(1, std::min(2, maxScale))), text);
    if (SUCCEEDED(hr) && maxScale >= 3) {
        std::wstring alt;
        if (SUCCEEDED(Recognize(Upscale(src, 3), alt)) && Letters(alt) > Letters(text)) text = std::move(alt);
    }
    if (SUCCEEDED(init)) RoUninitialize();
    return hr;
}
