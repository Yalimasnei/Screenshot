// SPDX-License-Identifier: GPL-3.0-only
// The capture screen: frozen desktop plus Snipping Tool's bar at the top.
//   Rectangle  drag a rectangle            Window  click a window
//   Full       click to take every screen  Free    draw an outline
//   Text       drag (or click a window) to extract text
//   Color      click a pixel; a magnifier follows the cursor
// Esc or right-click cancels.
#include "ui/ui_common.h"
#include "app/application.h"
#include <windowsx.h>
#include <cstring>

using namespace Gdiplus;

namespace {
enum { BRIGHT, DIM, BACK, INK, LAYERS };
const wchar_t kClass[] = L"ScreenshotOverlay";
constexpr int kDragThreshold = 4;

struct Button {
    Icon icon;
    SnipMode mode;  // the mode it selects; Close uses Rect and is special-cased
    bool close, sepBefore;
    const wchar_t* tip;
    const wchar_t* tipEnglish;
};
const Button kButtons[] = {
    {Icon::Rect, SnipMode::Rect, false, false, L"矩形", L"Rectangle"},
    {Icon::Window, SnipMode::Window, false, false, L"窗口", L"Window"},
    {Icon::Full, SnipMode::Full, false, false, L"全屏", L"Full screen"},
    {Icon::Free, SnipMode::Free, false, false, L"任意形状", L"Freeform"},
    {Icon::Text, SnipMode::Text, false, true, L"提取文字：拖动框选，或单击窗口", L"Extract text: drag an area or click a window"},
    {Icon::Color, SnipMode::Color, false, false, L"取色器：单击复制颜色值", L"Color picker: click to copy a color"},
    {Icon::Close, SnipMode::Rect, true, true, L"关闭 (Esc)", L"Close (Esc)"},
};
constexpr int kButtonCount = int(sizeof kButtons / sizeof kButtons[0]);

struct Overlay {
    bool creating = true;
    const ::Image* img = nullptr;
    const ::Image* exact = nullptr;  // colour picker samples
    SnipDone done = nullptr;
    SnipMode mode = SnipMode::Rect;
    int w = 0, h = 0;
    std::vector<RECT> targets;  // client coords; windows topmost first, then monitors
    RECT mon{};                 // monitor the bar sits on
    HDC dc[LAYERS] = {};
    HBITMAP bmp[LAYERS] = {};
    HGDIOBJ old[LAYERS] = {};
    float dpi = 1;              // DPI / 96
    RECT bar{}, btn[kButtonCount] = {};
    int hotBtn = -1;
    Tween btnHot[kButtonCount], btnSel[kButtonCount];  // 0..1 hover / selected backgrounds
    Tween fade, slide;  // window opacity 0..1; bar drop-in offset in px
    bool layered = false;  // WS_EX_LAYERED while fading in
    bool down = false, dragging = false, finished = false;
    bool showSelectionSize = false;
    POINT a{}, b{}, cur{};
    RECT hover{};
    std::vector<POINT> path;    // free-form outline while drawing
    RECT pathBox{};
};
Overlay* g_ov = nullptr;

uint32_t* NewLayer(Overlay& o, int i) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = o.w;
    bi.bmiHeader.biHeight = -o.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    o.bmp[i] = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!o.bmp[i]) return nullptr;
    o.dc[i] = CreateCompatibleDC(nullptr);
    o.old[i] = SelectObject(o.dc[i], o.bmp[i]);
    return static_cast<uint32_t*>(bits);
}

bool BuildLayers(Overlay& o) {
    uint32_t* bright = NewLayer(o, BRIGHT);
    uint32_t* dim = NewLayer(o, DIM);
    if (!bright || !dim || !NewLayer(o, BACK) || !NewLayer(o, INK)) return false;
    const size_t n = size_t(o.w) * o.h;
    memcpy(bright, o.img->px.data(), n * 4);
    for (size_t i = 0; i < n; ++i) dim[i] = ((bright[i] >> 1) & 0x7F7F7F7Fu) | 0xFF000000u;
    return true;
}

void FreeOverlay(Overlay* o) {
    for (int i = 0; i < LAYERS; ++i) {
        if (o->dc[i]) { SelectObject(o->dc[i], o->old[i]); DeleteDC(o->dc[i]); }
        if (o->bmp[i]) DeleteObject(o->bmp[i]);
    }
    delete o;
}

int Px(const Overlay& o, float v) { return int(v * o.dpi + 0.5f); }

void LayoutBar(Overlay& o) {
    const int size = Px(o, 40), gap = Px(o, 4), sep = Px(o, 13), pad = Px(o, 6);
    int x = pad;
    for (int i = 0; i < kButtonCount; ++i) {
        if (kButtons[i].sepBefore) x += sep - gap;
        o.btn[i] = {x, pad, x + size, pad + size};
        x += size + gap;
    }
    const int barW = x - gap + pad, barH = size + 2 * pad;
    const int left = (o.mon.left + o.mon.right - barW) / 2, top = o.mon.top + Px(o, 16);
    o.bar = {left, top, left + barW, top + barH};
    for (RECT& r : o.btn) OffsetRect(&r, left, top);
}

RECT TipRect(const Overlay& o) {  // generous box below the bar for the tooltip
    return {o.bar.left - Px(o, 120), o.bar.bottom, o.bar.right + Px(o, 120), o.bar.bottom + Px(o, 44)};
}

RECT DragRect(const Overlay& o) {
    return {std::min(o.a.x, o.b.x), std::min(o.a.y, o.b.y), std::max(o.a.x, o.b.x) + 1, std::max(o.a.y, o.b.y) + 1};
}

RECT HitTest(const Overlay& o, POINT p) {
    for (const RECT& r : o.targets)
        if (PtInRect(&r, p)) return r;
    return {};
}

bool OverBar(const Overlay& o, POINT p) { return !o.down && PtInRect(&o.bar, p); }

// The area a release now would capture (empty if none).
RECT Highlight(const Overlay& o) {
    if (OverBar(o, o.cur)) return {};
    switch (o.mode) {
    case SnipMode::Rect: return o.dragging ? DragRect(o) : RECT{};
    case SnipMode::Window: return o.hover;
    case SnipMode::Full: return {0, 0, o.w, o.h};
    case SnipMode::Text: return o.dragging ? DragRect(o) : o.hover;
    default: return {};
    }
}

bool ShowsDimensions(const Overlay& o) {
    return o.showSelectionSize &&
           (o.mode == SnipMode::Rect || o.mode == SnipMode::Window || o.mode == SnipMode::Text);
}

constexpr int kLoupeCells = 15;
RECT LoupeRect(const Overlay& o) {
    if (o.mode != SnipMode::Color || OverBar(o, o.cur)) return {};
    const int cell = Px(o, 8), side = kLoupeCells * cell, label = Px(o, 36), off = Px(o, 24);
    int x = o.cur.x + off, y = o.cur.y + off;
    if (x + side > o.w) x = o.cur.x - off - side;
    if (y + side + label > o.h) y = o.cur.y - off - side - label;
    return {x, y, x + side, y + side + label};
}

// Everything drawn on top of the dim layer that depends on the pointer.
RECT Decorations(const Overlay& o) {
    RECT r = Highlight(o);
    if (o.mode == SnipMode::Full) r = {};  // static; never needs a partial repaint
    if (!IsRectEmpty(&r)) {
        if (ShowsDimensions(o)) r.right = std::max<LONG>(r.right, r.left + Px(o, 120));
        InflateRect(&r, Px(o, 4), Px(o, ShowsDimensions(o) ? 36 : 4));
    }
    RECT l = LoupeRect(o);
    // Grid endpoints and the label outline can cover pixels outside the box.
    if (!IsRectEmpty(&l)) InflateRect(&l, 2, 2);
    UnionRect(&r, &r, &l);
    return r;
}

void Repaint(HWND hwnd, const RECT& a, const RECT& b) {
    if (!IsRectEmpty(&a)) InvalidateRect(hwnd, &a, FALSE);
    if (!IsRectEmpty(&b)) InvalidateRect(hwnd, &b, FALSE);
}

// White text in a rounded dark box; `x` is the left edge, or the centre.
void Label(Graphics& g, const Overlay& o, const wchar_t* text, float x, float y, bool center) {
    Font font(Tr(L"Microsoft YaHei UI", L"Segoe UI"), 13.f * o.dpi, FontStyleRegular, UnitPixel);
    RectF box;
    g.MeasureString(text, -1, &font, PointF(0, 0), &box);
    const float pad = 7 * o.dpi;
    RectF r(center ? x - box.Width / 2 - pad : x, y, box.Width + 2 * pad, box.Height + pad);
    const Palette& th = Theme();
    SolidBrush bg(th.bar), fg(th.text);
    FillRoundRect(g, r, 4 * o.dpi, bg);
    if (th.border.GetA()) { Pen edge(th.border, 1); StrokeRoundRect(g, r, 4 * o.dpi, edge); }
    g.DrawString(text, -1, &font, PointF(r.X + pad, r.Y + pad / 2), &fg);
}

// GDI part of the magnifier (drawn before any GDI+ work on the back buffer).
void LoupePixels(const Overlay& o, HDC back) {
    const RECT lr = LoupeRect(o);
    if (IsRectEmpty(&lr)) return;
    const int cell = Px(o, 8), side = kLoupeCells * cell, half = kLoupeCells / 2;
    RECT zoom{lr.left, lr.top, lr.left + side, lr.top + side};
    FillRect(back, &zoom, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    // Source pixels around the cursor, clipped to the capture.
    const int sx0 = std::max<int>(0, o.cur.x - half), sy0 = std::max<int>(0, o.cur.y - half);
    const int sx1 = std::min<int>(o.w, o.cur.x + half + 1), sy1 = std::min<int>(o.h, o.cur.y + half + 1);
    if (sx1 > sx0 && sy1 > sy0) {
        SetStretchBltMode(back, COLORONCOLOR);
        StretchBlt(back, zoom.left + (sx0 - (o.cur.x - half)) * cell, zoom.top + (sy0 - (o.cur.y - half)) * cell,
                   (sx1 - sx0) * cell, (sy1 - sy0) * cell, o.dc[BRIGHT], sx0, sy0, sx1 - sx0, sy1 - sy0, SRCCOPY);
    }
}

void DrawLoupe(Graphics& g, const Overlay& o) {
    const RECT lr = LoupeRect(o);
    if (IsRectEmpty(&lr)) return;
    const int cell = Px(o, 8), side = kLoupeCells * cell, half = kLoupeCells / 2;
    RECT zoom{lr.left, lr.top, lr.left + side, lr.top + side};
    Pen grid(Color(40, 255, 255, 255), 1);
    for (int i = 1; i < kLoupeCells; ++i) {
        g.DrawLine(&grid, INT(zoom.left + i * cell), INT(zoom.top), INT(zoom.left + i * cell), INT(zoom.bottom));
        g.DrawLine(&grid, INT(zoom.left), INT(zoom.top + i * cell), INT(zoom.right), INT(zoom.top + i * cell));
    }
    Pen centre(Color(255, 255, 255, 255), 2), frame(Theme().accent, 2);
    g.DrawRectangle(&centre, zoom.left + half * cell, zoom.top + half * cell, cell, cell);
    g.DrawRectangle(&frame, zoom.left + 1, zoom.top + 1, side - 2, side - 2);
    const uint32_t p = (o.cur.x >= 0 && o.cur.y >= 0 && o.cur.x < o.w && o.cur.y < o.h)
                           ? o.exact->px[size_t(o.cur.y) * o.w + o.cur.x] : 0;
    wchar_t t[48];
    swprintf(t, 48, L"#%02X%02X%02X", (p >> 16) & 255, (p >> 8) & 255, p & 255);
    SolidBrush sw(Color(255, BYTE(p >> 16), BYTE(p >> 8), BYTE(p)));
    const float y = float(zoom.bottom + Px(o, 4));
    Label(g, o, t, float(zoom.left) + 18 * o.dpi, y, false);
    g.FillRectangle(&sw, float(zoom.left), y + 4 * o.dpi, 14 * o.dpi, 14 * o.dpi);
}

void DrawBar(Graphics& g, const Overlay& o) {
    const Palette& th = Theme();
    const float dy = o.slide.Value();  // drop-in offset while the capture screen appears
    g.TranslateTransform(0, dy);
    SolidBrush bg(th.bar), accent(th.accent);
    const RectF barF(REAL(o.bar.left), REAL(o.bar.top), REAL(o.bar.right - o.bar.left), REAL(o.bar.bottom - o.bar.top));
    FillRoundRect(g, barF, 8 * o.dpi, bg);
    if (th.border.GetA()) { Pen edge(th.border, 1); StrokeRoundRect(g, barF, 8 * o.dpi, edge); }
    Pen sep(th.separator, 1);
    const Color clear(0, th.selected.GetR(), th.selected.GetG(), th.selected.GetB());
    for (int i = 0; i < kButtonCount; ++i) {
        const RECT& r = o.btn[i];
        const RectF rf(REAL(r.left), REAL(r.top), REAL(r.right - r.left), REAL(r.bottom - r.top));
        // Selected and hover backgrounds fade in and out.
        const float sel = o.btnSel[i].Value(), hot = o.btnHot[i].Value();
        const Color base = Mix(clear, th.selected, sel);
        const Color fill = Mix(base, th.hover, hot);
        if (fill.GetA()) { SolidBrush b(fill); FillRoundRect(g, rf, 5 * o.dpi, b); }
        if (sel > 0.01f) {
            const float wid = rf.Width * 0.4f * sel;  // underline grows from the centre
            SolidBrush u(Mix(Color(0, th.accent.GetR(), th.accent.GetG(), th.accent.GetB()), th.accent, sel));
            FillRoundRect(g, RectF(rf.X + (rf.Width - wid) / 2, rf.GetBottom() - 3 * o.dpi, wid, 3 * o.dpi), 1.5f * o.dpi, u);
        }
        if (kButtons[i].sepBefore) {
            const float x = REAL(r.left) - 6.5f * o.dpi;
            g.DrawLine(&sep, x, REAL(r.top) + 8 * o.dpi, x, REAL(r.bottom) - 8 * o.dpi);
        }
        RECT ir = r;
        InflateRect(&ir, -Px(o, 9), -Px(o, 9));
        DrawIcon(g, kButtons[i].icon, ir, Mix(th.text, th.accent, sel));
    }
    if (o.hotBtn >= 0)
        Label(g, o, Tr(kButtons[o.hotBtn].tip, kButtons[o.hotBtn].tipEnglish), (o.btn[o.hotBtn].left + o.btn[o.hotBtn].right) / 2.f,
              REAL(o.bar.bottom + Px(o, 6)), true);
    g.ResetTransform();
}

// Starts the per-button tweens for the current hover / mode.
void UpdateBarTweens(HWND hwnd, Overlay& o) {
    for (int i = 0; i < kButtonCount; ++i) {
        o.btnHot[i].Go(i == o.hotBtn ? 1.f : 0.f, 110);
        o.btnSel[i].Go(!kButtons[i].close && kButtons[i].mode == o.mode ? 1.f : 0.f, 160);
    }
    KeepAnimating(hwnd, true);
}

bool BarAnimating(const Overlay& o) {
    if (o.slide.Running()) return true;
    for (int i = 0; i < kButtonCount; ++i)
        if (o.btnHot[i].Running() || o.btnSel[i].Running()) return true;
    return false;
}

RECT BarArea(const Overlay& o) {  // bar, tooltip and the drop-in distance above
    RECT r = o.bar, tip = TipRect(o);
    UnionRect(&r, &r, &tip);
    r.top -= Px(o, 16);
    return r;
}

void Paint(const Overlay& o, HDC dc, const RECT& clip) {
    HDC back = o.dc[BACK];
    const int cw = clip.right - clip.left, ch = clip.bottom - clip.top;
    const bool bright = o.mode == SnipMode::Color || o.mode == SnipMode::Full;
    // Raw GDI first ...
    const int layer = bright ? BRIGHT : (o.mode == SnipMode::Free && o.down ? INK : DIM);
    BitBlt(back, clip.left, clip.top, cw, ch, o.dc[layer], clip.left, clip.top, SRCCOPY);
    const RECT s = Highlight(o);
    RECT vis;
    if (!bright && !IsRectEmpty(&s) && IntersectRect(&vis, &s, &clip))
        BitBlt(back, vis.left, vis.top, vis.right - vis.left, vis.bottom - vis.top, o.dc[BRIGHT], vis.left, vis.top,
               SRCCOPY);
    LoupePixels(o, back);
    // ... then GDI+ on top.
    {
        Graphics g(back);
        g.SetClip(Gdiplus::Rect(clip.left, clip.top, cw, ch));
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        if (!IsRectEmpty(&s)) {
            const RectF box(REAL(s.left), REAL(s.top), REAL(std::max<LONG>(0, s.right - s.left - 1)),
                            REAL(std::max<LONG>(0, s.bottom - s.top - 1)));
            if (o.mode == SnipMode::Rect || o.mode == SnipMode::Text) {
                // Local Snipping Tool SelectionRectangle.xbf: white, 2 DIP,
                // StrokeDashArray="2 1.5"; each edge is a separate Line.
                Pen frame(Color(255, 255, 255, 255), 2 * o.dpi);
                const REAL dashes[] = {2.f, 1.5f};
                frame.SetDashPattern(dashes, 2);
                frame.SetDashCap(DashCapFlat);
                g.DrawLine(&frame, box.X, box.Y, box.GetRight(), box.Y);
                g.DrawLine(&frame, box.X, box.GetBottom(), box.GetRight(), box.GetBottom());
                g.DrawLine(&frame, box.X, box.Y, box.X, box.GetBottom());
                g.DrawLine(&frame, box.GetRight(), box.Y, box.GetRight(), box.GetBottom());
            } else if (o.mode == SnipMode::Window) {
                // WindowPlaceholder.xbf: Margin=-1, BorderThickness=1,
                // SelectionBorderColor; the thin border lies outside the window.
                const REAL width = o.dpi;
                SolidBrush frame(Color(255, 255, 255, 255));
                g.FillRectangle(&frame, REAL(s.left) - width, REAL(s.top) - width,
                                REAL(s.right - s.left) + 2 * width, width);
                g.FillRectangle(&frame, REAL(s.left) - width, REAL(s.bottom),
                                REAL(s.right - s.left) + 2 * width, width);
                g.FillRectangle(&frame, REAL(s.left) - width, REAL(s.top), width, REAL(s.bottom - s.top));
                g.FillRectangle(&frame, REAL(s.right), REAL(s.top), width, REAL(s.bottom - s.top));
            }
            if (ShowsDimensions(o)) {
                wchar_t t[48];
                swprintf(t, 48, L"%ld × %ld", s.right - s.left, s.bottom - s.top);
                const float lh = 30 * o.dpi;
                Label(g, o, t, REAL(s.left), s.top - lh >= 0 ? s.top - lh : s.top + 4 * o.dpi, false);
            }
        }
        DrawLoupe(g, o);
        const RECT barArea = BarArea(o);
        if (IntersectRect(&vis, &barArea, &clip)) DrawBar(g, o);
    }
    BitBlt(dc, clip.left, clip.top, cw, ch, back, clip.left, clip.top, SRCCOPY);
}

void Finish(HWND hwnd, SnipResult r) {
    Overlay* o = g_ov;
    if (!o || o->finished) return;
    o->finished = true;
    if (GetCapture() == hwnd) ReleaseCapture();
    ShowWindow(hwnd, SW_HIDE);
    const RECT all{0, 0, o->w, o->h};
    IntersectRect(&r.rect, &r.rect, &all);  // client coords == image pixels
    if (r.kind != SnipResult::Color && IsRectEmpty(&r.rect)) r.kind = SnipResult::Cancel;
    r.mode = o->mode;
    o->done(r);
    DestroyWindow(hwnd);
}

void SetMode(HWND hwnd, Overlay& o, SnipMode m) {
    o.mode = m;
    o.down = o.dragging = false;
    o.path.clear();
    o.hover = (m == SnipMode::Window || m == SnipMode::Text) ? HitTest(o, o.cur) : RECT{};
    UpdateBarTweens(hwnd, o);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void OnMove(HWND hwnd, Overlay& o, POINT pt) {
    const RECT before = Decorations(o);
    const int hot = o.hotBtn;
    o.cur = pt;
    o.hotBtn = -1;
    if (!o.down)
        for (int i = 0; i < kButtonCount; ++i)
            if (PtInRect(&o.btn[i], pt)) o.hotBtn = i;
    if (o.down && !o.dragging && (abs(pt.x - o.a.x) > kDragThreshold || abs(pt.y - o.a.y) > kDragThreshold))
        o.dragging = o.mode == SnipMode::Rect || o.mode == SnipMode::Text || o.mode == SnipMode::Free;
    const POINT c{std::clamp<LONG>(pt.x, 0, o.w - 1), std::clamp<LONG>(pt.y, 0, o.h - 1)};
    if (o.dragging) {
        o.b = c;
        if (o.mode == SnipMode::Free) {
            const POINT previous = o.path.back();
            o.path.push_back(c);
            {
                Graphics g(o.dc[INK]);
                g.SetSmoothingMode(SmoothingModeAntiAlias);
                // FreeformSelection.xbf: transparent fill, white 1-DIP outline.
                Pen pen(Color(255, 255, 255, 255), o.dpi);
                pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound);
                g.DrawLine(&pen, REAL(previous.x), REAL(previous.y), REAL(c.x), REAL(c.y));
            }
            RECT dirty{std::min(previous.x, c.x), std::min(previous.y, c.y), std::max(previous.x, c.x) + 1, std::max(previous.y, c.y) + 1};
            InflateRect(&dirty, Px(o, 4), Px(o, 4));
            InvalidateRect(hwnd, &dirty, FALSE);
            RECT seg{c.x, c.y, c.x + 1, c.y + 1};
            UnionRect(&o.pathBox, &o.pathBox, &seg);
        }
    } else if (!o.down && (o.mode == SnipMode::Window || o.mode == SnipMode::Text)) {
        o.hover = OverBar(o, pt) ? RECT{} : HitTest(o, pt);
    }
    const RECT after = Decorations(o);
    if (!EqualRect(&before, &after) || o.mode == SnipMode::Color) Repaint(hwnd, before, after);
    if (hot != o.hotBtn) {
        const RECT bar = BarArea(o);
        InvalidateRect(hwnd, &bar, FALSE);
        UpdateBarTweens(hwnd, o);
    }
}

void OnDown(HWND hwnd, Overlay& o, POINT pt) {
    if (OverBar(o, pt)) {
        for (int i = 0; i < kButtonCount; ++i) {
            if (!PtInRect(&o.btn[i], pt)) continue;
            if (kButtons[i].close) Finish(hwnd, {});
            else SetMode(hwnd, o, kButtons[i].mode);
        }
        return;
    }
    if (o.mode == SnipMode::Color) {
        SnipResult r;
        r.kind = SnipResult::Color;
        if (pt.x >= 0 && pt.y >= 0 && pt.x < o.w && pt.y < o.h) r.color = o.exact->px[size_t(pt.y) * o.w + pt.x];
        Finish(hwnd, r);
        return;
    }
    o.down = true;
    o.dragging = false;
    o.a = o.b = pt;
    o.path.assign(1, pt);
    o.pathBox = {pt.x, pt.y, pt.x + 1, pt.y + 1};
    if (o.mode == SnipMode::Free) BitBlt(o.dc[INK], 0, 0, o.w, o.h, o.dc[DIM], 0, 0, SRCCOPY);
    SetCapture(hwnd);
}

void OnUp(HWND hwnd, Overlay& o) {
    if (!o.down) return;
    o.down = false;
    const bool dragged = o.dragging;
    o.dragging = false;
    SnipResult r;
    r.kind = o.mode == SnipMode::Text ? SnipResult::Text : SnipResult::Picture;
    if (o.mode == SnipMode::Free) {
        const RECT box = o.pathBox;
        if (dragged && o.path.size() >= 3 && box.right - box.left >= 3 && box.bottom - box.top >= 3) {
            r.rect = box;
            r.poly = std::move(o.path);
            Finish(hwnd, r);
            return;
        }
    } else if (dragged) {
        const RECT d = DragRect(o);
        if (d.right - d.left >= 2 && d.bottom - d.top >= 2) { r.rect = d; Finish(hwnd, r); return; }
    } else if (o.mode != SnipMode::Rect) {  // a click: Window, Full, Text take what is highlighted
        r.rect = o.mode == SnipMode::Full ? RECT{0, 0, o.w, o.h} : HitTest(o, o.cur);
        Finish(hwnd, r);
        return;
    }
    o.path.clear();  // too small: start over
    if (GetCapture() == hwnd) ReleaseCapture();
    InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Overlay* o = g_ov;
    if (!o || o->finished) {
        if (msg == WM_NCDESTROY && o) { KeepAnimating(hwnd, false); g_ov = nullptr; if (!o->creating) FreeOverlay(o); }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        Paint(*o, dc, ps.rcPaint);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, OverBar(*o, o->cur) ? IDC_ARROW : IDC_CROSS));
        return TRUE;
    case WM_MOUSEMOVE:
        OnMove(hwnd, *o, pt);
        return 0;
    case WM_LBUTTONDOWN:
        OnDown(hwnd, *o, pt);
        return 0;
    case WM_LBUTTONUP:
        OnUp(hwnd, *o);
        return 0;
    case WM_CAPTURECHANGED:
        if (o->down) { o->down = o->dragging = false; o->path.clear(); InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    case WM_RBUTTONUP:
        Finish(hwnd, {});
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            Finish(hwnd, {});
        } else if (wp == VK_RETURN && !o->down) {
            SnipResult r;
            r.kind = o->mode == SnipMode::Text ? SnipResult::Text : SnipResult::Picture;
            if (o->mode == SnipMode::Full) r.rect = {0, 0, o->w, o->h};
            else if (o->mode == SnipMode::Window || o->mode == SnipMode::Text) r.rect = o->hover;
            if (!IsRectEmpty(&r.rect)) Finish(hwnd, r);
        }
        return 0;
    case WM_APP_LANGUAGECHANGED:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_APP_ANIMFRAME:
        SendMessageW(hwnd, WM_TIMER, kAnimTimer, 0);
        if (IsWindow(hwnd)) UpdateWindow(hwnd);
        AnimationFrameDelivered(hwnd);
        return 0;
    case WM_TIMER:
        if (wp == kAnimTimer) {
            if (o->layered) {
                const float a = o->fade.Value();
                SetLayeredWindowAttributes(hwnd, 0, BYTE(255 * a + 0.5f), LWA_ALPHA);
                if (!o->fade.Running()) {  // fully shown: drop the layered style (cheaper to repaint)
                    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & ~WS_EX_LAYERED);
                    o->layered = false;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            const RECT bar = BarArea(*o);
            InvalidateRect(hwnd, &bar, FALSE);
            KeepAnimating(hwnd, o->layered || BarAnimating(*o));
        }
        return 0;
    case WM_DPICHANGED:
        return 0;  // keep covering the whole virtual screen
    case WM_NCDESTROY:
        KeepAnimating(hwnd, false);
        g_ov = nullptr;
        if (!o->creating) FreeOverlay(o);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ForceForeground(HWND hwnd) {
    HWND fg = GetForegroundWindow();
    const DWORD me = GetCurrentThreadId(), other = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const bool attach = other && other != me && AttachThreadInput(me, other, TRUE);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
    if (attach) AttachThreadInput(me, other, FALSE);
}
}  // namespace

bool ShowOverlay(const ::Image& img, const ::Image& exact, const RECT& screen, const std::vector<RECT>& windows,
                 const std::vector<RECT>& monitors, SnipMode mode, SnipDone done) {
    if (g_ov || img.w != screen.right - screen.left || img.h != screen.bottom - screen.top) return false;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof wc};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0;
        if (!registered) return false;
    }
    auto* o = new Overlay;
    o->img = &img;
    o->exact = exact.px.size() == img.px.size() ? &exact : &img;
    o->done = done;
    o->mode = mode;
    o->showSelectionSize = GetShowSelectionSize();
    o->w = img.w;
    o->h = img.h;
    auto toClient = [&](RECT r) { OffsetRect(&r, -screen.left, -screen.top); return r; };
    const RECT all{0, 0, o->w, o->h};
    for (const RECT& r : windows) { RECT c = toClient(r); if (IntersectRect(&c, &c, &all)) o->targets.push_back(c); }
    for (const RECT& r : monitors) o->targets.push_back(toClient(r));
    POINT cur{};
    GetCursorPos(&cur);
    HMONITOR hm = MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(hm, &mi);
    o->mon = toClient(mi.rcMonitor);
    o->dpi = MonitorScale(hm);
    LayoutBar(*o);
    {   // button centres in screen coordinates, for scripted tests
        wchar_t centres[256] = L"";
        for (int i = 0; i < kButtonCount; ++i) {
            wchar_t one[32];
            swprintf(one, 32, L" %ld,%ld", (o->btn[i].left + o->btn[i].right) / 2 + screen.left,
                     (o->btn[i].top + o->btn[i].bottom) / 2 + screen.top);
            wcscat_s(centres, one);
        }
        Log(L"overlay mode %d, bar buttons:%ls", int(mode), centres);
    }
    o->cur = {cur.x - screen.left, cur.y - screen.top};
    if (mode == SnipMode::Window || mode == SnipMode::Text) o->hover = HitTest(*o, o->cur);
    if (!BuildLayers(*o)) { FreeOverlay(o); return false; }
    g_ov = o;

    // Entrance: the dimmed capture fades in while the bar drops into place.
    for (int i = 0; i < kButtonCount; ++i) {
        o->btnHot[i].Jump(0);
        o->btnSel[i].Jump(!kButtons[i].close && kButtons[i].mode == mode ? 1.f : 0.f);
    }
    o->layered = AnimationsOn();
    o->fade.Jump(o->layered ? 0.f : 1.f);
    o->slide.Jump(o->layered ? -14 * o->dpi : 0.f);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | (o->layered ? WS_EX_LAYERED : 0), kClass, L"Screenshot",
                                WS_POPUP, screen.left, screen.top, o->w, o->h, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) { g_ov = nullptr; FreeOverlay(o); return false; }
    o->creating = false;
    if (o->layered) {
        SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
        o->fade.Go(1, 140);
        o->slide.Go(0, 220);
        KeepAnimating(hwnd, true);
    }
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    ForceForeground(hwnd);
    return true;
}
