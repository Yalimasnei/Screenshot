// SPDX-License-Identifier: GPL-3.0-only
// The notice after a capture, like Snipping Tool's: bottom right of the
// primary screen, thumbnail plus text, closes after a few seconds. Clicking a
// picture notice opens the markup editor.
#include "ui/ui_common.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <cmath>

using namespace Gdiplus;

namespace {
const wchar_t kClass[] = L"ScreenshotToast";
constexpr UINT_PTR kCloseTimer = 1;
constexpr UINT kLifetimeMs = 6000;

struct Toast {
    HWND hwnd = nullptr;
    bool creating = true;
    std::wstring title, body;
    ::Image thumb;
    Bitmap* thumbBmp = nullptr;
    Bitmap* preview = nullptr;
    bool swatch = false;
    COLORREF color = 0;
    float dpi = 1;
    int w = 0, h = 0;
    RectF thumbRect;
    RECT closeBtn{};
    bool hover = false, hoverClose = false;
    int x = 0, y = 0;             // resting position
    Tween alpha, rise;            // opacity 0..1; pixels below the resting position
    Tween hoverT, closeT;         // hover highlight of the notice / its close button
    bool closing = false;         // fading out; destroyed when the fade ends
    bool dismissed = false;
    bool hoverAnimating = false;
    int lastAlpha = -1, lastY = INT_MIN;
};
Toast* g_toast = nullptr;

void StartClosing(Toast& t, bool dismissed = false) {
    t.dismissed |= dismissed;
    if (t.closing) return;
    t.closing = true;
    t.alpha.Go(0, 160);
    if (!t.alpha.Running()) { DestroyWindow(t.hwnd); return; }  // animations off
    KeepAnimating(t.hwnd, true);
}

float D(const Toast& t, float v) { return v * t.dpi; }

void Layout(Toast& t) {
    const float pad = D(t, 14), inner = D(t, 340);
    float y = pad;
    if (t.thumbBmp) {
        const float s = std::min({1.f, inner / t.thumb.w, D(t, 190) / t.thumb.h});
        const float tw = std::max(1.f, t.thumb.w * s), th = std::max(1.f, t.thumb.h * s);
        t.thumbRect = RectF(pad + (inner - tw) / 2, y, tw, th);
        y += th + D(t, 12);
    } else if (t.swatch) {
        t.thumbRect = RectF(pad, y, inner, D(t, 44));
        y += D(t, 44) + D(t, 12);
    }
    y += D(t, 22) + D(t, 2) + D(t, 20);  // title + body
    t.w = int(inner + 2 * pad);
    t.h = int(y + pad);
    const int b = int(D(t, 26));
    t.closeBtn = {t.w - b - int(D(t, 6)), int(D(t, 6)), t.w - int(D(t, 6)), b + int(D(t, 6))};
}

void MakePreview(Toast& t) {
    if (!t.thumbBmp) return;
    const int w = std::max(1, int(std::ceil(t.thumbRect.Width)));
    const int h = std::max(1, int(std::ceil(t.thumbRect.Height)));
    Bitmap* preview = new Bitmap(w, h, PixelFormat32bppPARGB);
    if (preview->GetLastStatus() != Ok) { delete preview; return; }
    Status status;
    {
        Graphics g(preview);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        status = g.DrawImage(t.thumbBmp, Rect(0, 0, w, h), 0, 0, t.thumb.w, t.thumb.h, UnitPixel);
    }
    if (status != Ok) {
        delete preview;
        return;
    }
    t.preview = preview;
}

void Paint(Toast& t, HDC dc) {
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, t.w, t.h);
    HGDIOBJ old = SelectObject(mem, bmp);
    {
        Graphics g(mem);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        const Palette& th = Theme();
        const float hv = t.hoverT.Value();
        g.Clear(Mix(th.toast, th.toastHover, hv));
        if (t.thumbBmp) {
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);
            Bitmap* image = t.preview ? t.preview : t.thumbBmp;
            g.DrawImage(image, t.thumbRect, 0, 0, REAL(image->GetWidth()), REAL(image->GetHeight()), UnitPixel);
            g.SetPixelOffsetMode(PixelOffsetModeDefault);
            Pen edge(th.imageBorder, 1);  // keeps a white snip visible on a light notice
            g.DrawRectangle(&edge, t.thumbRect.X - 0.5f, t.thumbRect.Y - 0.5f, t.thumbRect.Width, t.thumbRect.Height);
        } else if (t.swatch) {
            SolidBrush b(Color(255, GetRValue(t.color), GetGValue(t.color), GetBValue(t.color)));
            FillRoundRect(g, t.thumbRect, D(t, 6), b);
            Pen edge(th.imageBorder, 1);
            StrokeRoundRect(g, t.thumbRect, D(t, 6), edge);
        }
        const float pad = D(t, 14);
        const float y = (t.thumbBmp || t.swatch) ? t.thumbRect.GetBottom() + D(t, 12) : pad;
        Font title(Tr(L"Microsoft YaHei UI", L"Segoe UI"), D(t, 15), FontStyleBold, UnitPixel);
        Font body(Tr(L"Microsoft YaHei UI", L"Segoe UI"), D(t, 13), FontStyleRegular, UnitPixel);
        SolidBrush fg(th.text), muted(th.muted);
        StringFormat fmt;
        fmt.SetTrimming(StringTrimmingEllipsisCharacter);
        fmt.SetFormatFlags(StringFormatFlagsNoWrap);
        g.DrawString(t.title.c_str(), -1, &title, RectF(pad, y, REAL(t.w) - 2 * pad, D(t, 24)), &fmt, &fg);
        g.DrawString(t.body.c_str(), -1, &body, RectF(pad, y + D(t, 24), REAL(t.w) - 2 * pad, D(t, 22)), &fmt, &muted);
        if (hv > 0.01f) {  // close button fades in with the hover
            const RECT& c = t.closeBtn;
            const float cv = t.closeT.Value();
            if (cv > 0.01f) {
                SolidBrush hb(Mix(Color(0, th.hover.GetR(), th.hover.GetG(), th.hover.GetB()), th.hover, cv));
                FillRoundRect(g, RectF(REAL(c.left), REAL(c.top), REAL(c.right - c.left), REAL(c.bottom - c.top)),
                              D(t, 4), hb);
            }
            RECT ir = c;
            InflateRect(&ir, -int(D(t, 5)), -int(D(t, 5)));
            DrawIcon(g, Icon::Close, ir, Mix(th.toast, th.text, hv));
        }
    }
    BitBlt(dc, 0, 0, t.w, t.h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* t = reinterpret_cast<Toast*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        t = static_cast<Toast*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        t->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(t));
    }
    if (!t) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        Paint(*t, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, t->thumbBmp ? IDC_HAND : IDC_ARROW));
        return TRUE;
    case WM_MOUSEMOVE: {
        if (t->dismissed) return 0;
        const POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const bool overClose = PtInRect(&t->closeBtn, p) != 0;
        const bool changed = !t->hover || t->hoverClose != overClose || t->closing;
        if (t->closing) {  // pointer came back while it was fading out: stay
            t->closing = false;
            t->alpha.Go(1, 120);
        }
        if (!t->hover) {
            t->hover = true;
            KillTimer(hwnd, kCloseTimer);  // stay while the pointer is on it
            TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
        }
        t->hoverClose = overClose;
        if (!changed) return 0;
        t->hoverT.Go(1, 120);
        t->closeT.Go(overClose ? 1.f : 0.f, 100);
        InvalidateRect(hwnd, nullptr, FALSE);
        KeepAnimating(hwnd, true);
        return 0;
    }
    case WM_MOUSELEAVE:
        t->hover = t->hoverClose = false;
        t->hoverT.Go(0, 160);
        t->closeT.Go(0, 100);
        SetTimer(hwnd, kCloseTimer, 3000, nullptr);
        InvalidateRect(hwnd, nullptr, FALSE);
        KeepAnimating(hwnd, true);
        return 0;
    case WM_LBUTTONUP: {
        const POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (PtInRect(&t->closeBtn, p)) { StartClosing(*t, true); return 0; }
        if (t->thumbBmp) OpenEditor(t->thumb, AppIcon());
        DestroyWindow(hwnd);  // the editor takes over at once
        return 0;
    }
    case WM_RBUTTONUP:
        StartClosing(*t, true);
        return 0;
    case WM_APP_ANIMFRAME:
        SendMessageW(hwnd, WM_TIMER, kAnimTimer, 0);
        if (IsWindow(hwnd)) UpdateWindow(hwnd);
        AnimationFrameDelivered(hwnd);
        return 0;
    case WM_TIMER:
        if (wp == kCloseTimer) {
            KillTimer(hwnd, kCloseTimer);
            StartClosing(*t);
        } else if (wp == kAnimTimer) {
            const int alpha = int(255 * t->alpha.Value() + 0.5f), y = t->y + int(t->rise.Value());
            if (alpha != t->lastAlpha) { SetLayeredWindowAttributes(hwnd, 0, BYTE(alpha), LWA_ALPHA); t->lastAlpha = alpha; }
            if (y != t->lastY) { SetWindowPos(hwnd, nullptr, t->x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); t->lastY = y; }
            const bool hoverMoving = t->hoverT.Running() || t->closeT.Running();
            if (hoverMoving || t->hoverAnimating) InvalidateRect(hwnd, nullptr, FALSE);
            t->hoverAnimating = hoverMoving;
            const bool running = t->alpha.Running() || t->rise.Running() || hoverMoving;
            if (!running && t->closing) { DestroyWindow(hwnd); return 0; }
            KeepAnimating(hwnd, running);
        }
        return 0;
    case WM_APP_THEMECHANGED:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_NCDESTROY:
        KeepAnimating(hwnd, false);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (g_toast == t) g_toast = nullptr;
        if (!t->creating) { delete t->thumbBmp; delete t->preview; delete t; }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}  // namespace

void ShowToast(const ToastInfo& info) {
    if (g_toast) DestroyWindow(g_toast->hwnd);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof wc};
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0;
        if (!registered) return;
    }
    auto* t = new Toast;
    t->title = info.title;
    t->body = info.body;
    t->swatch = info.swatch;
    t->color = info.color;
    if (info.thumb && info.thumb->w > 0) {
        t->thumb = *info.thumb;
        t->thumbBmp = ToGdiplus(t->thumb);
    }
    HMONITOR hm = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(hm, &mi);
    t->dpi = MonitorScale(hm);
    Layout(*t);
    MakePreview(*t);
    const int m = int(D(*t, 12));
    t->x = mi.rcWork.right - t->w - m;
    t->y = mi.rcWork.bottom - t->h - m;
    // Entrance: rises a little and fades in.
    t->alpha.Jump(0);
    t->rise.Jump(D(*t, 16));
    t->hoverT.Jump(0);
    t->closeT.Jump(0);
    t->alpha.Go(1, 200);
    t->rise.Go(0, 260);
    if (!t->alpha.Running()) { t->alpha.Jump(1); t->rise.Jump(0); }
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kClass, L"Screenshot",
                                WS_POPUP, t->x, t->y + int(t->rise.Value()), t->w, t->h, nullptr, nullptr,
                                GetModuleHandleW(nullptr), t);
    if (!hwnd) { delete t->thumbBmp; delete t->preview; delete t; return; }
    t->creating = false;
    SetLayeredWindowAttributes(hwnd, 0, BYTE(255 * t->alpha.Value()), LWA_ALPHA);
    const DWORD round = 2;  // DWMWA_WINDOW_CORNER_PREFERENCE (33) = DWMWCP_ROUND, Windows 11
    DwmSetWindowAttribute(hwnd, 33, &round, sizeof round);
    g_toast = t;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetTimer(hwnd, kCloseTimer, kLifetimeMs, nullptr);
    KeepAnimating(hwnd, true);
}
