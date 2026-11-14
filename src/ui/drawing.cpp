// SPDX-License-Identifier: GPL-3.0-only
// GDI+ helpers, the light/dark palettes and line icons drawn in code.
#include "app/application.h"
#include "ui/ui_common.h"
#include <dwmapi.h>
#include <cstring>
#include <cmath>
#include <map>
#include <mutex>
#include <system_error>
#include <thread>

using namespace Gdiplus;

Bitmap* ToGdiplus(const ::Image& img) {
    auto* bmp = new Bitmap(img.w, img.h, PixelFormat32bppARGB);
    BitmapData d{};
    Gdiplus::Rect r(0, 0, img.w, img.h);
    if (bmp->LockBits(&r, ImageLockModeWrite, PixelFormat32bppARGB, &d) != Ok) { delete bmp; return nullptr; }
    for (int y = 0; y < img.h; ++y)
        memcpy(static_cast<BYTE*>(d.Scan0) + size_t(y) * d.Stride, &img.px[size_t(y) * img.w], size_t(img.w) * 4);
    bmp->UnlockBits(&d);
    return bmp;
}

bool FromGdiplus(Bitmap& bmp, ::Image& out) {
    const int w = int(bmp.GetWidth()), h = int(bmp.GetHeight());
    BitmapData d{};
    Gdiplus::Rect r(0, 0, w, h);
    if (bmp.LockBits(&r, ImageLockModeRead, PixelFormat32bppARGB, &d) != Ok) return false;
    out = ::Image{};
    out.w = w;
    out.h = h;
    out.px.resize(size_t(w) * h);
    for (int y = 0; y < h; ++y)
        memcpy(&out.px[size_t(y) * w], static_cast<const BYTE*>(d.Scan0) + size_t(y) * d.Stride, size_t(w) * 4);
    bmp.UnlockBits(&d);
    return true;
}

float MonitorScale(HMONITOR m) {
    UINT dx = 96, dy = 96;
    using Fn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static Fn f = [] {
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        return shcore ? reinterpret_cast<Fn>(GetProcAddress(shcore, "GetDpiForMonitor")) : nullptr;
    }();
    if (!f || FAILED(f(m, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy))) dy = 96;
    return dy / 96.f;
}

static void RoundPath(GraphicsPath& p, const RectF& r, float rad) {
    const float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

void FillRoundRect(Graphics& g, const RectF& r, float rad, const Brush& b) {
    GraphicsPath p;
    RoundPath(p, r, rad);
    g.FillPath(&b, &p);
}

void StrokeRoundRect(Graphics& g, const RectF& r, float rad, const Pen& pen) {
    GraphicsPath p;
    RoundPath(p, r, rad);
    const SmoothingMode old = g.GetSmoothingMode();
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.DrawPath(&pen, &p);
    g.SetSmoothingMode(old);
}

// ---- animation ----
bool AnimationsOn() {
    BOOL on = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0);
    return on != FALSE;
}

double NowMs() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / double(freq.QuadPart);
}

float Tween::Value() const {
    if (dur <= 0) return to;
    const double p = (NowMs() - t0) / dur;
    if (p >= 1) return to;
    const double q = 1 - p;
    return float(from + (to - from) * (1 - q * q * q));
}

bool Tween::Running() const { return dur > 0 && NowMs() - t0 < dur; }

void Tween::Go(float v, double ms) {
    if (!set) { Jump(v); return; }
    if (v == to) return;
    from = Value();
    to = v;
    t0 = NowMs();
    dur = AnimationsOn() ? ms : 0;
}

Color Mix(Color a, Color b, float t) {
    t = std::clamp(t, 0.f, 1.f);
    auto m = [t](BYTE x, BYTE y) { return BYTE(x + (y - x) * t + 0.5f); };
    return Color(m(a.GetA(), b.GetA()), m(a.GetR(), b.GetR()), m(a.GetG(), b.GetG()), m(a.GetB(), b.GetB()));
}

namespace {
// One high-resolution timer serves all animated windows. At most one frame is
// queued per window, so a slow paint cannot build up an input-delaying backlog.
class FrameClock {
    HANDLE wake_ = nullptr, timer_ = nullptr;
    std::thread thread_;
    std::mutex mutex_;
    struct WindowFrame { bool pending = false; double next = 0, interval = 1000.0 / 60, checked = 0; };
    std::map<HWND, WindowFrame> windows_;
    bool stopping_ = false;

    static double Interval(HWND hwnd) {
        MONITORINFOEXW info{}; info.cbSize = sizeof info;
        DEVMODEW mode{}; mode.dmSize = sizeof mode;
        const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (GetMonitorInfoW(monitor, &info) && EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
            mode.dmDisplayFrequency >= 24 && mode.dmDisplayFrequency <= 1000)
            return 1000.0 / mode.dmDisplayFrequency;
        return 1000.0 / 60;
    }

    void Run() {
        const HANDLE handles[] = {wake_, timer_};
        bool useTimeout = false;
        DWORD timeout = INFINITE;
        for (;;) {
            const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, useTimeout ? timeout : INFINITE);
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) break;
            if (wait != WAIT_OBJECT_0 && wait != WAIT_OBJECT_0 + 1 && wait != WAIT_TIMEOUT) break;
            if (windows_.empty()) { CancelWaitableTimer(timer_); useTimeout = false; continue; }
            const double now = NowMs();
            double delay = 1000;
            for (auto& window : windows_) {
                WindowFrame& frame = window.second;
                if (!frame.checked || now - frame.checked >= 1000) {
                    frame.interval = Interval(window.first);
                    frame.checked = now;
                }
                if (frame.next <= now) {
                    if (!frame.pending && PostMessageW(window.first, WM_APP_ANIMFRAME, 0, 0)) frame.pending = true;
                    frame.next = frame.next ? frame.next + frame.interval : now + frame.interval;
                    if (frame.next <= now) frame.next = now + frame.interval;
                }
                delay = std::min(delay, frame.next - now);
            }
            LARGE_INTEGER due; due.QuadPart = -std::max<LONGLONG>(1, LONGLONG(delay * 10000));
            useTimeout = !SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
            timeout = DWORD(std::max(1.0, std::ceil(delay)));
        }
    }
  public:
    FrameClock() {
        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        constexpr DWORD highResolution = 0x00000002;  // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, highResolution, TIMER_ALL_ACCESS);
        if (wake_ && timer_) {
            try { thread_ = std::thread(&FrameClock::Run, this); }
            catch (const std::system_error&) { }
        }
    }
    ~FrameClock() {
        if (thread_.joinable()) {
            { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
            SetEvent(wake_);
            thread_.join();
        }
        if (timer_) CloseHandle(timer_);
        if (wake_) CloseHandle(wake_);
    }
    bool Available() const { return thread_.joinable(); }
    void Set(HWND hwnd, bool running) {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool changed = running ? windows_.emplace(hwnd, WindowFrame{}).second : windows_.erase(hwnd) != 0;
        if (changed) SetEvent(wake_);
    }
    void Delivered(HWND hwnd) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = windows_.find(hwnd);
        if (it != windows_.end()) it->second.pending = false;
    }
};
FrameClock& Clock() { static FrameClock clock; return clock; }
}

void KeepAnimating(HWND hwnd, bool running) {
    if (!hwnd) return;
    FrameClock& clock = Clock();
    if (clock.Available()) clock.Set(hwnd, running);
    else if (running) SetTimer(hwnd, kAnimTimer, 10, nullptr);
    else KillTimer(hwnd, kAnimTimer);
}

void AnimationFrameDelivered(HWND hwnd) { Clock().Delivered(hwnd); }

// ---- theme ----
static Palette MakePalette(bool dark) {
    Palette p;
    p.dark = dark;
    if (dark) {  // Windows 11 dark, as Snipping Tool draws it
        p.bar = Color(245, 32, 32, 32);
        p.hover = Color(255, 61, 61, 61);
        p.selected = Color(255, 70, 70, 70);
        p.accent = Color(255, 76, 194, 255);
        p.text = Color(255, 240, 240, 240);
        p.muted = Color(255, 160, 160, 160);
        p.separator = Color(255, 80, 80, 80);
        p.disabled = Color(255, 90, 90, 90);
        p.canvas = Color(255, 24, 24, 24);
        p.imageBorder = Color(255, 64, 64, 64);
        p.tooltip = Color(255, 50, 50, 50);
        p.toast = Color(255, 36, 36, 36);
        p.toastHover = Color(255, 44, 44, 44);
        p.check1 = Color(255, 60, 60, 60);
        p.check2 = Color(255, 80, 80, 80);
        p.border = Color(0, 0, 0, 0);
    } else {     // Windows 11 light
        p.bar = Color(248, 249, 249, 249);
        p.hover = Color(255, 232, 232, 232);
        p.selected = Color(255, 224, 224, 224);
        p.accent = Color(255, 0, 95, 184);
        p.text = Color(255, 27, 27, 27);
        p.muted = Color(255, 96, 96, 96);
        p.separator = Color(255, 210, 210, 210);
        p.disabled = Color(255, 180, 180, 180);
        p.canvas = Color(255, 238, 238, 238);
        p.imageBorder = Color(255, 200, 200, 200);
        p.tooltip = Color(255, 252, 252, 252);
        p.toast = Color(255, 249, 249, 249);
        p.toastHover = Color(255, 240, 240, 240);
        p.check1 = Color(255, 255, 255, 255);
        p.check2 = Color(255, 220, 220, 220);
        p.border = Color(255, 205, 205, 205);
    }
    return p;
}

static Palette g_palette = MakePalette(false);
const Palette& Theme() { return g_palette; }

static bool WindowsUsesLightApps() {
    DWORD v = 1, size = sizeof v;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v != 0;
}

// Popup menus (the tray menu) only turn dark through uxtheme's unnamed
// SetPreferredAppMode (#135) and FlushMenuThemes (#136), Windows 10 1903+.
static void ApplyMenuTheme(bool dark) {
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetMode = int(WINAPI*)(int);
    using Flush = void(WINAPI*)();
    auto set = reinterpret_cast<SetMode>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
    auto flush = reinterpret_cast<Flush>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
    if (set) set(dark ? 2 /* ForceDark */ : 3 /* ForceLight */);
    if (flush) flush();
}

void RefreshTheme() {
    const int setting = GetThemeSetting();
    const bool dark = setting == 2 || (setting == 0 && !WindowsUsesLightApps());
    g_palette = MakePalette(dark);
    ApplyMenuTheme(dark);
}

void ApplyWindowTheme(HWND hwnd) {
    const BOOL dark = g_palette.dark;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
}

void DrawIcon(Graphics& g, Icon icon, const RECT& r, Color c) {
    const float u = float(std::min(r.right - r.left, r.bottom - r.top)) / 24.f;
    const float ox = (r.left + r.right) / 2.f - 10 * u, oy = (r.top + r.bottom) / 2.f - 10 * u;
    const bool mirror = icon == Icon::Redo;
    auto P = [&](float x, float y) { return PointF(ox + (mirror ? 20 - x : x) * u, oy + y * u); };
    auto R = [&](float x0, float y0, float x1, float y1) { return RectF(ox + x0 * u, oy + y0 * u, (x1 - x0) * u, (y1 - y0) * u); };
    Pen pen(c, 1.5f * u);
    pen.SetLineJoin(LineJoinRound);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    SolidBrush br(c);
    const SmoothingMode oldMode = g.GetSmoothingMode();
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    auto Lines = [&](std::initializer_list<PointF> pts, bool closed = false) {
        std::vector<PointF> v(pts);
        if (closed) g.DrawPolygon(&pen, v.data(), INT(v.size()));
        else g.DrawLines(&pen, v.data(), INT(v.size()));
    };
    switch (icon) {
    case Icon::Rect: {
        pen.SetDashStyle(DashStyleDash);
        g.DrawRectangle(&pen, R(3, 4, 17, 16));
        break;
    }
    case Icon::Window:
        g.DrawRectangle(&pen, R(2, 3, 18, 17));
        g.FillRectangle(&br, R(2, 3, 18, 6.5f));
        break;
    case Icon::Full:
        g.DrawRectangle(&pen, R(2, 3, 18, 14));
        Lines({P(10, 14), P(10, 17)});
        Lines({P(6, 17.5f), P(14, 17.5f)});
        break;
    case Icon::Free: {
        pen.SetDashStyle(DashStyleDash);
        PointF pts[] = {P(4, 10), P(6, 4), P(12, 3), P(17, 7), P(15, 13), P(9, 16), P(5, 15)};
        g.DrawClosedCurve(&pen, pts, 7);
        break;
    }
    case Icon::Text:
        Lines({P(2, 6), P(2, 2), P(6, 2)});
        Lines({P(14, 2), P(18, 2), P(18, 6)});
        Lines({P(18, 14), P(18, 18), P(14, 18)});
        Lines({P(6, 18), P(2, 18), P(2, 14)});
        Lines({P(6, 6), P(14, 6)});
        Lines({P(10, 6), P(10, 15)});
        break;
    case Icon::Color: {
        Pen thick(c, 2.4f * u);
        thick.SetStartCap(LineCapRound);
        thick.SetEndCap(LineCapRound);
        g.DrawLine(&thick, P(4, 16), P(10.5f, 9.5f));
        g.FillEllipse(&br, R(11, 3, 17, 9));
        Lines({P(9, 6.5f), P(13.5f, 11)});
        break;
    }
    case Icon::Close:
        Lines({P(5, 5), P(15, 15)});
        Lines({P(15, 5), P(5, 15)});
        break;
    case Icon::Pen:
        Lines({P(4, 16), P(5, 12.5f), P(13.5f, 4), P(16, 6.5f), P(7.5f, 15)}, true);
        Lines({P(11.5f, 6), P(14, 8.5f)});
        break;
    case Icon::Highlighter:
        Lines({P(5, 14), P(6, 10.5f), P(13, 3.5f), P(16.5f, 7), P(9.5f, 14)}, true);
        g.FillRectangle(&br, R(3, 16.5f, 17, 18.5f));
        break;
    case Icon::Eraser:
        Lines({P(3, 12), P(10, 5), P(16, 11), P(10, 17)}, true);
        Lines({P(6.5f, 8.5f), P(12.5f, 14.5f)});
        Lines({P(10, 17.5f), P(17, 17.5f)});
        break;
    case Icon::Crop:
        Lines({P(6, 2), P(6, 14), P(18, 14)});
        Lines({P(2, 6), P(14, 6), P(14, 18)});
        break;
    case Icon::Undo:
    case Icon::Redo: {
        GraphicsPath p;
        p.AddLine(P(5, 8), P(12, 8));
        p.AddBezier(P(12, 8), P(17.5f, 8), P(17.5f, 15.5f), P(12, 15.5f));
        p.AddLine(P(12, 15.5f), P(8, 15.5f));
        g.DrawPath(&pen, &p);
        Lines({P(8, 4.5f), P(4.5f, 8), P(8, 11.5f)});
        break;
    }
    case Icon::Copy:
        Lines({P(7, 7), P(7, 3), P(17, 3), P(17, 13), P(13, 13)});
        g.DrawRectangle(&pen, R(3, 7, 13, 17));
        break;
    case Icon::Save:
        Lines({P(3, 3), P(14, 3), P(17, 6), P(17, 17), P(3, 17)}, true);
        g.DrawRectangle(&pen, R(6.5f, 3, 13, 7));
        g.DrawRectangle(&pen, R(6, 11, 14, 17));
        break;
    case Icon::Check:
        Lines({P(4, 10.5f), P(8, 14.5f), P(16, 5.5f)});
        break;
    }
    g.SetSmoothingMode(oldMode);
}
