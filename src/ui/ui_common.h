// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// GDI+ drawing shared by the snipping bar, the toast and the editor.
#include "core/common.h"
#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}
#include <objidl.h>
#include <gdiplus.h>

struct GdiplusSession {
    ULONG_PTR token = 0;
    GdiplusSession() {
        Gdiplus::GdiplusStartupInput in;
        Gdiplus::GdiplusStartup(&token, &in, nullptr);
    }
    ~GdiplusSession() { if (token) Gdiplus::GdiplusShutdown(token); }
};

// Image <-> GDI+ bitmap (32bpp ARGB, straight alpha; same byte layout).
Gdiplus::Bitmap* ToGdiplus(const Image& img);
bool FromGdiplus(Gdiplus::Bitmap& bmp, Image& out);

enum class Icon {
    Rect, Window, Full, Free, Text, Color, Close,  // snipping bar
    Pen, Highlighter, Eraser, Crop, Undo, Redo, Copy, Save, Check,  // editor
};
// Draws `icon` centred in `r` with stroke colour `c`.
void DrawIcon(Gdiplus::Graphics& g, Icon icon, const RECT& r, Gdiplus::Color c);
void FillRoundRect(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius, const Gdiplus::Brush& b);
void StrokeRoundRect(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius, const Gdiplus::Pen& p);

// ---- animation (icons.cpp) ----
// Every animation is skipped (jumps to its end) when Windows' "Animation
// effects" setting is off.
bool AnimationsOn();
double NowMs();
// A value that eases (cubic ease-out) from where it is to a target.
struct Tween {
    float from = 0, to = 0;
    double t0 = 0, dur = 0;
    bool set = false;
    float Value() const;
    bool Running() const;
    void Jump(float v) { from = to = v; dur = 0; set = true; }
    void Go(float v, double ms);  // no-op if already heading to v; the first call jumps
};
Gdiplus::Color Mix(Gdiplus::Color a, Gdiplus::Color b, float t);  // t = 0 -> a, 1 -> b
// Frame timer: windows call this after starting tweens and on each tick.
constexpr UINT_PTR kAnimTimer = 0xA111;
constexpr UINT WM_APP_ANIMFRAME = WM_APP + 21;
void KeepAnimating(HWND hwnd, bool running);
void AnimationFrameDelivered(HWND hwnd);

// Light / dark colours for every window. The setting (app.cpp) is
// 0 = follow Windows, 1 = light, 2 = dark.
struct Palette {
    bool dark = false;
    Gdiplus::Color bar, hover, selected, accent, text, muted, separator, disabled;
    Gdiplus::Color canvas, imageBorder, tooltip, toast, toastHover, check1, check2;
    Gdiplus::Color border;  // outline for boxes drawn over screenshots (alpha 0 = none)
};
const Palette& Theme();
void RefreshTheme();               // re-reads the setting and the Windows theme
void ApplyWindowTheme(HWND hwnd);  // light or dark title bar
// Sent to every window of the app after the theme changed.
constexpr UINT WM_APP_THEMECHANGED = WM_APP + 20;
constexpr UINT WM_APP_LANGUAGECHANGED = WM_APP + 22;

// toast.cpp: the notice shown after a capture, bottom right of the screen.
struct ToastInfo {
    std::wstring title, body;
    const Image* thumb = nullptr;  // copied; clicking opens it in the editor
    bool swatch = false;           // show a colour sample instead of a thumbnail
    COLORREF color = 0;
};
void ShowToast(const ToastInfo& info);

// main.cpp
HICON AppIcon();
// icons.cpp: DPI of a monitor / 96.
float MonitorScale(HMONITOR m);

// editor.cpp: markup window (pen, highlighter, eraser, crop, undo, save).
void OpenEditor(const Image& img, HICON icon);
bool CloseAllEditors();  // asks every editor first; cancellation keeps all windows open
bool CanEndEditorSession();  // blocks normal shutdown while edits remain; does not close windows
