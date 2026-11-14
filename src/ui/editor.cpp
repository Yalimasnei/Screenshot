// SPDX-License-Identifier: GPL-3.0-only
// Markup editor, like Snipping Tool's: pen, highlighter, eraser, crop, undo /
// redo, copy, save. Edits are strokes kept over the untouched capture, so
// crop and undo never lose pixels. Every edit is copied to the clipboard.
#include "app/application.h"
#include "ui/ui_common.h"
#include <commdlg.h>
#include <dwmapi.h>
#include <knownfolders.h>
#include <windowsx.h>
#include <cmath>
#include <cwctype>
#include <map>

using namespace Gdiplus;

namespace {
const wchar_t kClass[] = L"ScreenshotEditor";
constexpr UINT_PTR kCopyTimer = 1;
bool g_closingEditors = false;

enum class Tool { Pen, Highlighter, Eraser };
struct Stroke {
    Tool tool = Tool::Pen;
    Color color;
    float width = 4;
    std::vector<PointF> pts;  // image pixels
};
struct Doc {
    std::vector<Stroke> strokes;
    RECT crop{};
};

const Color kPenColors[] = {Color(255, 0, 0, 0), Color(255, 232, 39, 39), Color(255, 255, 196, 0),
                            Color(255, 34, 177, 76), Color(255, 0, 120, 215), Color(255, 255, 255, 255)};
const Color kHiColors[] = {Color(255, 255, 238, 0), Color(255, 102, 230, 60), Color(255, 0, 214, 255),
                           Color(255, 255, 84, 200), Color(255, 255, 150, 0)};
const float kPenSizes[] = {2, 4, 8}, kHiSizes[] = {12, 20, 32};
constexpr int kPenColorCount = 6, kHiColorCount = 5, kSizeCount = 3;

enum { B_PEN, B_HI, B_ERASER, B_COLOR0 = 10, B_SIZE0 = 30, B_CROP = 40, B_UNDO, B_REDO, B_COPY, B_SAVE, B_DONE,
       B_CANCEL };
struct Button {
    int id;
    RECT r;
};

struct Editor {
    HWND hwnd = nullptr;
    bool creating = true;
    bool copyPending = false;
    uint64_t copyOperation = 0;
    ::Image base;
    Bitmap* baseBmp = nullptr;
    bool hasAlpha = false;
    Doc doc;
    std::vector<Doc> undo, redo;
    Tool tool = Tool::Pen;
    int penColor = 1, hiColor = 0, penSize = 1, hiSize = 1;
    bool cropping = false, dirty = false;
    RECT cropEdit{};
    int cropHandle = -1;  // 0..7 = corners/edges, 8 = move
    POINT cropStart{};
    RECT cropAtStart{};
    bool drawing = false, erasing = false, erased = false;
    Stroke cur;
    std::vector<Button> buttons;
    int hot = -1, pressed = -1;
    mutable std::map<int, Tween> hoverT;  // toolbar hover fade, by button id
    mutable std::map<int, Tween> selectedT;
    float dpi = 1;
    int cw = 0, ch = 0;
    RECT view{};        // image rect on screen
    double scale = 1;
    RECT shown{};       // image pixels displayed (crop, or everything while cropping)
    Bitmap* cache = nullptr;
    RECT cacheShown{};
    double cacheScale = 0;
    std::wstring savedPath;
};

bool HasUnsavedChanges(const Editor& e) {
    return e.dirty || (e.drawing && !e.cur.pts.empty()) ||
           (e.cropping && !EqualRect(&e.cropEdit, &e.doc.crop));
}

void UpdateShutdownReason(const Editor& e) {
    if (HasUnsavedChanges(e))
        ShutdownBlockReasonCreate(e.hwnd, Tr(L"截图有未保存的更改", L"A screenshot has unsaved changes"));
    else
        ShutdownBlockReasonDestroy(e.hwnd);
}

float D(const Editor& e, float v) { return v * e.dpi; }
int Di(const Editor& e, float v) { return int(v * e.dpi + 0.5f); }
int BarH(const Editor& e) { return Di(e, 52); }
int ToolbarMinWidth(const Editor& e) { return Di(e, 760); }

void Layout(Editor& e) {
    e.buttons.clear();
    const int s = Di(e, 36), gap = Di(e, 4), y = (BarH(e) - s) / 2;
    int x = Di(e, 8);
    auto add = [&](int id, int w) { e.buttons.push_back({id, {x, y, x + w, y + s}}); x += w + gap; };
    if (e.cropping) {
        x = e.cw / 2 - s - gap;
        add(B_DONE, s);
        add(B_CANCEL, s);
        return;
    }
    add(B_PEN, s);
    add(B_HI, s);
    add(B_ERASER, s);
    x += Di(e, 10);
    if (e.tool != Tool::Eraser) {
        const int n = e.tool == Tool::Pen ? kPenColorCount : kHiColorCount;
        for (int i = 0; i < n; ++i) add(B_COLOR0 + i, Di(e, 26));
        x += Di(e, 8);
        for (int i = 0; i < kSizeCount; ++i) add(B_SIZE0 + i, Di(e, 26));
        x += Di(e, 10);
    }
    add(B_CROP, s);
    x += Di(e, 10);
    add(B_UNDO, s);
    add(B_REDO, s);
    x = std::max(x + Di(e, 10), e.cw - 2 * s - gap - Di(e, 8));
    add(B_COPY, s);
    add(B_SAVE, s);
}

void ComputeView(Editor& e) {
    e.shown = e.cropping ? RECT{0, 0, e.base.w, e.base.h} : e.doc.crop;
    const int iw = e.shown.right - e.shown.left, ih = e.shown.bottom - e.shown.top;
    const int m = Di(e, 16), availW = std::max(1, e.cw - 2 * m), availH = std::max(1, e.ch - BarH(e) - 2 * m);
    e.scale = std::min({1.0, double(availW) / iw, double(availH) / ih});
    const int vw = std::max(1, int(iw * e.scale + 0.5)), vh = std::max(1, int(ih * e.scale + 0.5));
    const int vx = (e.cw - vw) / 2, vy = BarH(e) + (e.ch - BarH(e) - vh) / 2;
    e.view = {vx, vy, vx + vw, vy + vh};
}

PointF ToImage(const Editor& e, POINT p) {
    return PointF(REAL(e.shown.left + (p.x - e.view.left) / e.scale), REAL(e.shown.top + (p.y - e.view.top) / e.scale));
}
POINT ToScreen(const Editor& e, double x, double y) {
    return {LONG(std::lround(e.view.left + (x - e.shown.left) * e.scale)),
            LONG(std::lround(e.view.top + (y - e.shown.top) * e.scale))};
}

void RebuildCache(Editor& e) {
    if (e.cache && EqualRect(&e.cacheShown, &e.shown) && e.cacheScale == e.scale) return;
    delete e.cache;
    const int vw = e.view.right - e.view.left, vh = e.view.bottom - e.view.top;
    e.cache = new Bitmap(vw, vh, PixelFormat32bppPARGB);
    Graphics g(e.cache);
    g.SetCompositingMode(CompositingModeSourceCopy);
    g.SetInterpolationMode(e.scale == 1 ? InterpolationModeNearestNeighbor : InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    ImageAttributes attr;
    attr.SetWrapMode(WrapModeTileFlipXY);  // no dark fringe at the edges
    g.DrawImage(e.baseBmp, Gdiplus::Rect(0, 0, vw, vh), e.shown.left, e.shown.top, e.shown.right - e.shown.left,
                e.shown.bottom - e.shown.top, UnitPixel, &attr);
    e.cacheShown = e.shown;
    e.cacheScale = e.scale;
}

void DrawStroke(Graphics& g, const Stroke& s) {
    const bool hi = s.tool == Tool::Highlighter;
    const Color c = hi ? Color(110, s.color.GetR(), s.color.GetG(), s.color.GetB()) : s.color;
    if (s.pts.size() == 1) {  // a dot
        SolidBrush b(c);
        const float r = s.width / 2;
        if (hi) g.FillRectangle(&b, s.pts[0].X - r, s.pts[0].Y - r, s.width, s.width);
        else g.FillEllipse(&b, s.pts[0].X - r, s.pts[0].Y - r, s.width, s.width);
        return;
    }
    Pen pen(c, s.width);
    pen.SetLineJoin(LineJoinRound);
    pen.SetStartCap(hi ? LineCapSquare : LineCapRound);
    pen.SetEndCap(hi ? LineCapSquare : LineCapRound);
    g.DrawLines(&pen, s.pts.data(), INT(s.pts.size()));  // one path: a highlighter never darkens itself
}

// Crop handles: 0 TL, 1 T, 2 TR, 3 R, 4 BR, 5 B, 6 BL, 7 L (screen rects).
RECT HandleRect(const Editor& e, int i) {
    const POINT a = ToScreen(e, e.cropEdit.left, e.cropEdit.top), b = ToScreen(e, e.cropEdit.right, e.cropEdit.bottom);
    const LONG xs[] = {a.x, (a.x + b.x) / 2, b.x, b.x, b.x, (a.x + b.x) / 2, a.x, a.x};
    const LONG ys[] = {a.y, a.y, a.y, (a.y + b.y) / 2, b.y, b.y, b.y, (a.y + b.y) / 2};
    const int r = Di(e, 7);
    return {xs[i] - r, ys[i] - r, xs[i] + r, ys[i] + r};
}

void DrawCropUi(Graphics& g, const Editor& e) {
    const POINT a = ToScreen(e, e.cropEdit.left, e.cropEdit.top), b = ToScreen(e, e.cropEdit.right, e.cropEdit.bottom);
    Region outside(Gdiplus::Rect(e.view.left, e.view.top, e.view.right - e.view.left, e.view.bottom - e.view.top));
    outside.Exclude(Gdiplus::Rect(a.x, a.y, b.x - a.x, b.y - a.y));
    SolidBrush shade(Color(150, 0, 0, 0)), white(Color(255, 255, 255, 255));
    g.FillRegion(&shade, &outside);
    Pen frame(Color(255, 255, 255, 255), D(e, 1.5f));
    g.DrawRectangle(&frame, REAL(a.x), REAL(a.y), REAL(b.x - a.x), REAL(b.y - a.y));
    Pen edge(Color(255, 60, 60, 60), 1);
    for (int i = 0; i < 8; ++i) {
        const RECT h = HandleRect(e, i);
        g.FillEllipse(&white, REAL(h.left), REAL(h.top), REAL(h.right - h.left), REAL(h.bottom - h.top));
        g.DrawEllipse(&edge, REAL(h.left), REAL(h.top), REAL(h.right - h.left), REAL(h.bottom - h.top));
    }
}

bool CanUndo(const Editor& e) { return !e.undo.empty(); }
bool CanRedo(const Editor& e) { return !e.redo.empty(); }

void DrawBar(Graphics& g, const Editor& e) {
    const Palette& th = Theme();
    SolidBrush bar(Color(255, th.bar.GetR(), th.bar.GetG(), th.bar.GetB()));
    g.FillRectangle(&bar, 0, 0, e.cw, BarH(e));
    Pen line(th.separator, 1);
    g.DrawLine(&line, 0, BarH(e) - 1, e.cw, BarH(e) - 1);
    Font font(Tr(L"Microsoft YaHei UI", L"Segoe UI"), D(e, 13), FontStyleRegular, UnitPixel);
    SolidBrush text(th.muted);
    if (e.cropping) {
        StringFormat f;
        f.SetAlignment(StringAlignmentFar);
        f.SetLineAlignment(StringAlignmentCenter);
        const Button* first = e.buttons.empty() ? nullptr : &e.buttons.front();
        const REAL right = first ? REAL(first->r.left - Di(e, 16)) : REAL(e.cw / 2);
        g.DrawString(Tr(L"拖动边角调整裁剪范围 · Enter 完成 · Esc 取消", L"Drag the edges to crop · Enter to apply · Esc to cancel"), -1, &font,
                     RectF(0, 0, right, REAL(BarH(e))), &f, &text);
    }
    for (int i = 0; i < int(e.buttons.size()); ++i) {
        const Button& b = e.buttons[i];
        const RectF rf(REAL(b.r.left), REAL(b.r.top), REAL(b.r.right - b.r.left), REAL(b.r.bottom - b.r.top));
        const bool toolOn = (b.id == B_PEN && e.tool == Tool::Pen) || (b.id == B_HI && e.tool == Tool::Highlighter) ||
                            (b.id == B_ERASER && e.tool == Tool::Eraser);
        const bool colorOn = b.id >= B_COLOR0 && b.id < B_COLOR0 + 10 &&
                             b.id - B_COLOR0 == (e.tool == Tool::Pen ? e.penColor : e.hiColor);
        const bool sizeOn = b.id >= B_SIZE0 && b.id < B_SIZE0 + kSizeCount &&
                            b.id - B_SIZE0 == (e.tool == Tool::Pen ? e.penSize : e.hiSize);
        Tween& st = e.selectedT[b.id];
        st.Go(toolOn || colorOn || sizeOn ? 1.f : 0.f, 150);
        const float selected = st.Value();
        const bool disabled = (b.id == B_UNDO && !CanUndo(e)) || (b.id == B_REDO && !CanRedo(e));
        Tween& ht = e.hoverT[b.id];
        ht.Go(i == e.hot && !disabled ? 1.f : 0.f, 110);
        const float hv = ht.Value();
        const Color base = Mix(Color(0, th.selected.GetR(), th.selected.GetG(), th.selected.GetB()), th.selected,
                               b.id <= B_ERASER ? selected : 0.f);
        const Color fill = Mix(base, th.hover, hv);
        if (fill.GetA()) { SolidBrush fb(fill); FillRoundRect(g, rf, D(e, 5), fb); }
        if (b.id <= B_ERASER && selected > 0) {
            SolidBrush mark(Mix(Color(0, th.accent.GetR(), th.accent.GetG(), th.accent.GetB()), th.accent, selected));
            FillRoundRect(g, RectF(rf.X + rf.Width * 0.3f, rf.GetBottom() - D(e, 3), rf.Width * 0.4f, D(e, 3)),
                          D(e, 1.5f), mark);
        }
        if (b.id >= B_COLOR0 && b.id < B_COLOR0 + 10) {
            const int ci = b.id - B_COLOR0;
            const Color c = e.tool == Tool::Pen ? kPenColors[ci] : kHiColors[ci];
            const float d = D(e, 15 + 3 * selected);
            SolidBrush cb(c);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.FillEllipse(&cb, rf.X + (rf.Width - d) / 2, rf.Y + (rf.Height - d) / 2, d, d);
            Pen ring(Mix(th.disabled, th.accent, selected), D(e, 1 + selected));
            g.DrawEllipse(&ring, rf.X + (rf.Width - d) / 2, rf.Y + (rf.Height - d) / 2, d, d);
            g.SetSmoothingMode(SmoothingModeDefault);
        } else if (b.id >= B_SIZE0 && b.id < B_SIZE0 + kSizeCount) {
            const int si = b.id - B_SIZE0;
            const float d = D(e, 5.f + 4 * si);
            SolidBrush db(Mix(th.text, th.accent, selected));
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.FillEllipse(&db, rf.X + (rf.Width - d) / 2, rf.Y + (rf.Height - d) / 2, d, d);
            g.SetSmoothingMode(SmoothingModeDefault);
        } else {
            static const struct { int id; Icon icon; } icons[] = {
                {B_PEN, Icon::Pen}, {B_HI, Icon::Highlighter}, {B_ERASER, Icon::Eraser}, {B_CROP, Icon::Crop},
                {B_UNDO, Icon::Undo}, {B_REDO, Icon::Redo}, {B_COPY, Icon::Copy}, {B_SAVE, Icon::Save},
                {B_DONE, Icon::Check}, {B_CANCEL, Icon::Close}};
            for (const auto& ic : icons) {
                if (ic.id != b.id) continue;
                RECT ir = b.r;
                InflateRect(&ir, -Di(e, 7), -Di(e, 7));
                DrawIcon(g, ic.icon, ir, disabled ? th.disabled : Mix(th.text, th.accent, selected));
            }
        }
    }
}

const wchar_t* Tip(int id) {
    switch (id) {
    case B_PEN: return Tr(L"笔", L"Pen");
    case B_HI: return Tr(L"荧光笔", L"Highlighter");
    case B_ERASER: return Tr(L"橡皮擦：擦掉整条笔画", L"Eraser: remove an entire stroke");
    case B_CROP: return Tr(L"裁剪", L"Crop");
    case B_UNDO: return Tr(L"撤销 (Ctrl+Z)", L"Undo (Ctrl+Z)");
    case B_REDO: return Tr(L"重做 (Ctrl+Y)", L"Redo (Ctrl+Y)");
    case B_COPY: return Tr(L"复制 (Ctrl+C)", L"Copy (Ctrl+C)");
    case B_SAVE: return Tr(L"另存为 (Ctrl+S)", L"Save as (Ctrl+S)");
    case B_DONE: return Tr(L"完成裁剪 (Enter)", L"Apply crop (Enter)");
    case B_CANCEL: return Tr(L"取消裁剪 (Esc)", L"Cancel crop (Esc)");
    }
    if (id >= B_COLOR0 && id < B_COLOR0 + 10) return Tr(L"颜色", L"Color");
    if (id >= B_SIZE0 && id < B_SIZE0 + kSizeCount) return Tr(L"粗细", L"Thickness");
    return nullptr;
}

void Paint(Editor& e, HDC dc, int paintBottom) {
    HDC mem = CreateCompatibleDC(dc);
    const int bufferHeight = std::max(1, std::min(e.ch, paintBottom));
    HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1, e.cw), bufferHeight);
    HGDIOBJ old = SelectObject(mem, bmp);
    {
        Graphics g(mem);
        const Palette& th = Theme();
        g.Clear(th.canvas);
        RebuildCache(e);
        const int vw = e.view.right - e.view.left, vh = e.view.bottom - e.view.top;
        {   // thin outline so a white snip stands out from a light canvas
            Pen edge(th.imageBorder, 1);
            g.DrawRectangle(&edge, e.view.left - 1, e.view.top - 1, vw + 1, vh + 1);
        }
        if (e.hasAlpha) {  // checkerboard behind transparent free-form corners
            SolidBrush c1(th.check1), c2(th.check2);
            g.FillRectangle(&c1, e.view.left, e.view.top, vw, vh);
            const int t = Di(e, 8);
            for (int y = 0; y < vh; y += t)
                for (int x = (y / t) % 2 * t; x < vw; x += 2 * t)
                    g.FillRectangle(&c2, e.view.left + x, e.view.top + y, std::min(t, vw - x), std::min(t, vh - y));
        }
        g.SetInterpolationMode(InterpolationModeNearestNeighbor);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.DrawImage(e.cache, Gdiplus::Rect(e.view.left, e.view.top, vw, vh), 0, 0, vw, vh, UnitPixel);
        // Strokes, clipped to what is shown, in image coordinates.
        g.SetClip(Gdiplus::Rect(e.view.left, e.view.top, vw, vh));
        g.TranslateTransform(REAL(e.view.left), REAL(e.view.top));
        g.ScaleTransform(REAL(e.scale), REAL(e.scale));
        g.TranslateTransform(REAL(-e.shown.left), REAL(-e.shown.top));
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        for (const Stroke& s : e.doc.strokes) DrawStroke(g, s);
        if (e.drawing) DrawStroke(g, e.cur);
        g.ResetTransform();
        g.ResetClip();
        if (e.cropping) DrawCropUi(g, e);
        g.SetSmoothingMode(SmoothingModeDefault);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        DrawBar(g, e);
        if (e.hot >= 0 && e.hot < int(e.buttons.size()) && !e.drawing) {
            if (const wchar_t* tip = Tip(e.buttons[e.hot].id)) {
                Font font(Tr(L"Microsoft YaHei UI", L"Segoe UI"), D(e, 13), FontStyleRegular, UnitPixel);
                RectF m;
                g.MeasureString(tip, -1, &font, PointF(0, 0), &m);
                const RECT& br = e.buttons[e.hot].r;
                const float pad = D(e, 7), w = m.Width + 2 * pad;
                const float x = std::clamp((br.left + br.right) / 2.f - w / 2, D(e, 4), e.cw - w - D(e, 4));
                const Palette& th = Theme();
                SolidBrush bg(th.tooltip), fg(th.text);
                const RectF tr(x, REAL(BarH(e) + Di(e, 4)), w, m.Height + pad);
                FillRoundRect(g, tr, D(e, 4), bg);
                Pen edge(th.separator, 1);
                StrokeRoundRect(g, tr, D(e, 4), edge);
                g.DrawString(tip, -1, &font, PointF(x + pad, BarH(e) + Di(e, 4) + pad / 2), &fg);
            }
        }
    }
    BitBlt(dc, 0, 0, e.cw, bufferHeight, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// Composite for the destination: only NVENC consumes the retained high-depth pixels.
::Image Composite(const Editor& e, SaveFormat format) {
    const RECT c = e.doc.crop;
    ::Image image;
    if (format == SaveFormat::HeicNvenc && e.base.HighPrecision()) image = Crop(e.base, c);
    if (image.HighPrecision()) {
        if (e.doc.strokes.empty()) return image;
        Bitmap annotations(image.w, image.h, PixelFormat32bppARGB);
        {
            Graphics g(&annotations);
            g.Clear(Color(0, 0, 0, 0));
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);
            g.TranslateTransform(REAL(-c.left), REAL(-c.top));
            for (const Stroke& s : e.doc.strokes) DrawStroke(g, s);
        }
        ::Image layer;
        if (FromGdiplus(annotations, layer)) CompositeAnnotations(image, layer);
        return image;
    }
    Bitmap out(c.right - c.left, c.bottom - c.top, PixelFormat32bppARGB);
    {
        Graphics g(&out);
        g.SetCompositingMode(CompositingModeSourceCopy);
        g.DrawImage(e.baseBmp, Gdiplus::Rect(0, 0, c.right - c.left, c.bottom - c.top), c.left, c.top,
                    c.right - c.left, c.bottom - c.top, UnitPixel);
        g.SetCompositingMode(CompositingModeSourceOver);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.TranslateTransform(REAL(-c.left), REAL(-c.top));
        for (const Stroke& s : e.doc.strokes) DrawStroke(g, s);
    }
    ::Image img;
    FromGdiplus(out, img);
    return img;
}

bool CopyResult(Editor& e) {
    KillTimer(e.hwnd, kCopyTimer);
    e.copyPending = false;
    e.copyOperation = BeginClipboardOperation();
    const ::Image img = Composite(e, SaveFormat::Png);  // clipboard PNG and CF_DIB both use BGRA8
    std::vector<BYTE> png;
    return EncodePng(img, png) && CopyToClipboard(e.hwnd, img, png);
}

bool CopyPendingResult(Editor& e) {
    KillTimer(e.hwnd, kCopyTimer);
    if (!e.copyPending) return true;
    e.copyPending = false;
    if (!ClipboardOperationCurrent(e.copyOperation)) return true;
    return CopyResult(e);
}

bool EndsWithNoCase(const std::wstring& s, const wchar_t* suffix) {
    const size_t n = wcslen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (towlower(s[s.size() - n + i]) != towlower(suffix[i])) return false;
    return true;
}

SaveFormat SaveFormatForPath(const std::wstring& path, DWORD filterIndex, int preference) {
    if (EndsWithNoCase(path, L".heic") || EndsWithNoCase(path, L".heif"))
        return filterIndex == 2 || (filterIndex != 3 && preference == 1)
                   ? SaveFormat::HeicNvenc : SaveFormat::HeicWindows;
    return SaveFormat::Png;  // PNG and JPEG both consume 8-bit RGB/alpha samples
}

bool SaveAs(Editor& e) {
    // Retain canonical filter IDs while omitting NVENC on unsupported devices.
    std::wstring filter;
    std::vector<int> filterIds;
    auto addFilter = [&](int id, const wchar_t* title, const wchar_t* pattern) {
        filter.append(title); filter.push_back(0);
        filter.append(pattern); filter.push_back(0);
        filterIds.push_back(id);
    };
    addFilter(1, L"PNG (*.png)", L"*.png");
    if (NvencAvailable()) addFilter(2, L"NVENC (*.heic)", L"*.heic");
    addFilter(3, L"HEIC (*.heic)", L"*.heic");
    addFilter(4, L"JPEG (*.jpg)", L"*.jpg;*.jpeg");
    filter.push_back(0);
    const int pref = GetSaveFormat();
    const wchar_t* prefExt = pref ? L".heic" : L".png";
    const std::wstring def = e.savedPath.empty() ? ScreenshotFileName(prefExt) : e.savedPath;
    if (def.size() >= SIZE_MAX) return false;
    const size_t fileCapacity = std::max<size_t>(MAX_PATH, def.size() + 1);
    if (fileCapacity > UINT_MAX) return false;
    std::vector<wchar_t> file(fileCapacity, L'\0');
    std::copy(def.begin(), def.end(), file.begin());
    const std::wstring dir = SaveFolder();
    OPENFILENAMEW ofn = {sizeof ofn};
    ofn.hwndOwner = e.hwnd;
    ofn.lpstrFilter = filter.c_str();
    const auto selected = std::find(filterIds.begin(), filterIds.end(), pref + 1);
    ofn.nFilterIndex = selected == filterIds.end() ? 1 : DWORD(selected - filterIds.begin() + 1);
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = DWORD(file.size());
    ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    ofn.lpstrDefExt = prefExt + 1;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return false;
    const std::wstring path = file.data();
    if (!ofn.nFilterIndex || ofn.nFilterIndex > filterIds.size()) return false;
    const SaveFormat format = SaveFormatForPath(path, DWORD(filterIds[ofn.nFilterIndex - 1]), pref);
    const ::Image img = Composite(e, format);
    std::vector<BYTE> data;
    bool ok = false;
    std::wstring why;
    if (EndsWithNoCase(path, L".jpg") || EndsWithNoCase(path, L".jpeg")) {
        ok = EncodeJpeg(img, data);
    } else if (format != SaveFormat::Png) {
        ok = EncodeHeic(img, format, data, why, nullptr,
                        format == SaveFormat::HeicNvenc && GetNvenc444());
    } else {
        ok = EncodePng(img, data);
    }
    if (!ok || !WriteBytes(path, data)) {
        const std::wstring msg = why.empty() ? std::wstring(Tr(L"保存失败。", L"Could not save the image."))
                                             : Tr(L"无法存成 HEIC：", L"Could not save as HEIC: ") + why + Tr(L"。\n请换成 PNG 再保存。", L".\nChoose PNG and try again.");
        MessageBoxW(e.hwnd, msg.c_str(), Tr(L"截图编辑", L"Screenshot editor"), MB_ICONERROR);
        return false;
    }
    e.savedPath = path;
    e.dirty = false;
    UpdateShutdownReason(e);
    Log(L"editor saved %ls (%zu bytes)", path.c_str(), data.size());
    return true;
}

void PushUndo(Editor& e) {
    e.undo.push_back(e.doc);
    e.redo.clear();
}

void Changed(Editor& e) {
    e.dirty = true;
    UpdateShutdownReason(e);
    e.copyPending = true;
    e.copyOperation = BeginClipboardOperation();
    ComputeView(e);
    SetTimer(e.hwnd, kCopyTimer, 300, nullptr);  // copy the edited snip once edits pause
    InvalidateRect(e.hwnd, nullptr, FALSE);
}

void Undo(Editor& e, bool back) {
    auto& from = back ? e.undo : e.redo;
    auto& to = back ? e.redo : e.undo;
    if (from.empty()) return;
    to.push_back(e.doc);
    e.doc = std::move(from.back());
    from.pop_back();
    Changed(e);
}

// Removes every stroke within reach of image point `p`.
void EraseAt(Editor& e, PointF p) {
    const float reach = float(6 / e.scale);
    for (size_t i = e.doc.strokes.size(); i-- > 0;) {
        const Stroke& s = e.doc.strokes[i];
        const float r = reach + s.width / 2;
        bool hit = false;
        for (size_t k = 0; k < s.pts.size() && !hit; ++k) {
            const PointF a = s.pts[k], b = s.pts[k + 1 < s.pts.size() ? k + 1 : k];
            const float dx = b.X - a.X, dy = b.Y - a.Y, len2 = dx * dx + dy * dy;
            const float t = len2 > 0 ? std::clamp(((p.X - a.X) * dx + (p.Y - a.Y) * dy) / len2, 0.f, 1.f) : 0.f;
            const float ex = a.X + t * dx - p.X, ey = a.Y + t * dy - p.Y;
            hit = ex * ex + ey * ey <= r * r;
        }
        if (!hit) continue;
        if (!e.erased) { PushUndo(e); e.erased = true; }
        e.doc.strokes.erase(e.doc.strokes.begin() + long(i));
        Changed(e);
    }
}

void StartCrop(Editor& e) {
    e.cropping = true;
    e.cropEdit = e.doc.crop;
    Layout(e);
    ComputeView(e);
    InvalidateRect(e.hwnd, nullptr, FALSE);
}

void EndCrop(Editor& e, bool apply) {
    e.cropping = false;
    e.cropHandle = -1;
    UpdateShutdownReason(e);
    if (apply && !EqualRect(&e.cropEdit, &e.doc.crop)) {
        PushUndo(e);
        e.doc.crop = e.cropEdit;
        Layout(e);
        Changed(e);
        return;
    }
    Layout(e);
    ComputeView(e);
    InvalidateRect(e.hwnd, nullptr, FALSE);
}

void DragCrop(Editor& e, POINT p) {
    const double dx = (p.x - e.cropStart.x) / e.scale, dy = (p.y - e.cropStart.y) / e.scale;
    RECT r = e.cropAtStart;
    const LONG W = e.base.w, H = e.base.h;
    const LONG minW = std::min<LONG>(8, W), minH = std::min<LONG>(8, H);
    const int h = e.cropHandle;
    if (h == 8) {  // move
        const LONG w = r.right - r.left, ht = r.bottom - r.top;
        r.left = std::clamp<LONG>(LONG(r.left + dx), 0, W - w);
        r.top = std::clamp<LONG>(LONG(r.top + dy), 0, H - ht);
        r.right = r.left + w;
        r.bottom = r.top + ht;
    } else {
        if (h == 0 || h == 6 || h == 7) r.left = std::clamp<LONG>(LONG(r.left + dx), 0, r.right - minW);
        if (h == 2 || h == 3 || h == 4) r.right = std::clamp<LONG>(LONG(r.right + dx), r.left + minW, W);
        if (h == 0 || h == 1 || h == 2) r.top = std::clamp<LONG>(LONG(r.top + dy), 0, r.bottom - minH);
        if (h == 4 || h == 5 || h == 6) r.bottom = std::clamp<LONG>(LONG(r.bottom + dy), r.top + minH, H);
    }
    e.cropEdit = r;
}

void Press(Editor& e, int id) {
    if (id == B_PEN) e.tool = Tool::Pen;
    else if (id == B_HI) e.tool = Tool::Highlighter;
    else if (id == B_ERASER) e.tool = Tool::Eraser;
    else if (id >= B_COLOR0 && id < B_COLOR0 + 10) (e.tool == Tool::Pen ? e.penColor : e.hiColor) = id - B_COLOR0;
    else if (id >= B_SIZE0 && id < B_SIZE0 + kSizeCount) (e.tool == Tool::Pen ? e.penSize : e.hiSize) = id - B_SIZE0;
    else if (id == B_CROP) { StartCrop(e); return; }
    else if (id == B_UNDO) { Undo(e, true); return; }
    else if (id == B_REDO) { Undo(e, false); return; }
    else if (id == B_COPY) { CopyResult(e); return; }
    else if (id == B_SAVE) { SaveAs(e); return; }
    else if (id == B_DONE) { EndCrop(e, true); return; }
    else if (id == B_CANCEL) { EndCrop(e, false); return; }
    Layout(e);
    InvalidateRect(e.hwnd, nullptr, FALSE);
    KeepAnimating(e.hwnd, true);
}

int ButtonAt(const Editor& e, POINT p) {
    for (int i = 0; i < int(e.buttons.size()); ++i)
        if (PtInRect(&e.buttons[i].r, p)) return i;
    return -1;
}

int CropHandleAt(const Editor& e, POINT p) {
    for (int i = 0; i < 8; ++i) {
        RECT h = HandleRect(e, i);
        InflateRect(&h, Di(e, 4), Di(e, 4));
        if (PtInRect(&h, p)) return i;
    }
    const POINT a = ToScreen(e, e.cropEdit.left, e.cropEdit.top), b = ToScreen(e, e.cropEdit.right, e.cropEdit.bottom);
    const RECT inside{a.x, a.y, b.x, b.y};
    return PtInRect(&inside, p) ? 8 : -1;
}

void OnDown(Editor& e, POINT p) {
    SetCapture(e.hwnd);
    if (p.y < BarH(e)) {
        e.pressed = ButtonAt(e, p);
        return;
    }
    if (e.cropping) {
        e.cropHandle = CropHandleAt(e, p);
        e.cropStart = p;
        e.cropAtStart = e.cropEdit;
        return;
    }
    if (e.tool == Tool::Eraser) {
        e.erasing = true;
        e.erased = false;
        EraseAt(e, ToImage(e, p));
        return;
    }
    e.drawing = true;
    e.cur = {};
    e.cur.tool = e.tool;
    const bool pen = e.tool == Tool::Pen;
    e.cur.color = pen ? kPenColors[e.penColor] : kHiColors[e.hiColor];
    // Sizes are in screen pixels at the current zoom, like Snipping Tool.
    e.cur.width = float((pen ? kPenSizes[e.penSize] : kHiSizes[e.hiSize]) * e.dpi / e.scale);
    e.cur.pts.push_back(ToImage(e, p));
    InvalidateRect(e.hwnd, nullptr, FALSE);
}

void OnMove(Editor& e, POINT p) {
    const int hot = GetCapture() == e.hwnd ? e.hot : (p.y < BarH(e) ? ButtonAt(e, p) : -1);
    if (hot != e.hot) {
        e.hot = hot;
        if (hot >= 0) { TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, e.hwnd, 0}; TrackMouseEvent(&t); }
        InvalidateRect(e.hwnd, nullptr, FALSE);
        KeepAnimating(e.hwnd, true);
    }
    if (e.drawing) {
        const PointF q = ToImage(e, p), last = e.cur.pts.back();
        const float min = float(1.5 / e.scale);
        if (fabsf(q.X - last.X) + fabsf(q.Y - last.Y) >= min) {
            e.cur.pts.push_back(q);
            InvalidateRect(e.hwnd, nullptr, FALSE);
        }
    } else if (e.erasing) {
        EraseAt(e, ToImage(e, p));
    } else if (e.cropping && e.cropHandle >= 0) {
        DragCrop(e, p);
        InvalidateRect(e.hwnd, nullptr, FALSE);
    }
}

void FinishStroke(Editor& e) {
    if (!e.drawing) return;
    e.drawing = false;
    PushUndo(e);
    e.doc.strokes.push_back(std::move(e.cur));
    e.cur = {};
    Changed(e);
}

void OnUp(Editor& e, POINT p) {
    FinishStroke(e);
    e.erasing = false;
    e.cropHandle = -1;
    const int i = e.pressed;
    e.pressed = -1;
    if (GetCapture() == e.hwnd) ReleaseCapture();
    if (i >= 0 && i == ButtonAt(e, p)) Press(e, e.buttons[i].id);
}

LPCWSTR CursorFor(const Editor& e, POINT p) {
    if (p.y < BarH(e)) return IDC_ARROW;
    if (!e.cropping) return IDC_CROSS;
    const int h = e.cropHandle >= 0 ? e.cropHandle : CropHandleAt(e, p);
    static const LPCWSTR byHandle[] = {IDC_SIZENWSE, IDC_SIZENS, IDC_SIZENESW, IDC_SIZEWE,
                                       IDC_SIZENWSE, IDC_SIZENS, IDC_SIZENESW, IDC_SIZEWE, IDC_SIZEALL};
    return h >= 0 ? byHandle[h] : IDC_ARROW;
}

bool ConfirmClose(Editor& e) {
    if (!CopyPendingResult(e)) Log(L"editor: clipboard copy failed");
    if (!e.dirty) return true;
    const int answer = MessageBoxW(e.hwnd, Tr(L"是否保存对截图的更改？", L"Save changes to this screenshot?"),
                                  Tr(L"截图编辑", L"Screenshot editor"), MB_YESNOCANCEL | MB_ICONQUESTION);
    return answer == IDNO || (answer == IDYES && SaveAs(e));
}

void FreeEditor(Editor* e) {
    delete e->cache;
    delete e->baseBmp;
    delete e;
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* e = reinterpret_cast<Editor*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        e = static_cast<Editor*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        e->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(e));
    }
    if (!e) return DefWindowProcW(hwnd, msg, wp, lp);
    const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    switch (msg) {
    case WM_SIZE:
        e->cw = LOWORD(lp);
        e->ch = HIWORD(lp);
        Layout(*e);
        ComputeView(*e);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        Paint(*e, dc, ps.rcPaint.bottom);
        EndPaint(hwnd, &ps);
        bool running = false;
        for (const auto& kv : e->hoverT) running |= kv.second.Running();
        for (const auto& kv : e->selectedT) running |= kv.second.Running();
        if (running) KeepAnimating(hwnd, true);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_APP_LANGUAGECHANGED:
        SetWindowTextW(hwnd, Tr(L"截图编辑 - Screenshot", L"Editor - Screenshot"));
        UpdateShutdownReason(*e);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_APP_THEMECHANGED:
        ApplyWindowTheme(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        e->dpi = HIWORD(wp) / 96.f;
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {ToolbarMinWidth(*e), Di(*e, 300)};
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            SetCursor(LoadCursorW(nullptr, CursorFor(*e, p)));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        OnDown(*e, pt);
        return 0;
    case WM_MOUSEMOVE:
        OnMove(*e, pt);
        return 0;
    case WM_LBUTTONUP:
        OnUp(*e, pt);
        return 0;
    case WM_MOUSELEAVE:
        if (e->hot >= 0 && GetCapture() != hwnd) { e->hot = -1; InvalidateRect(hwnd, nullptr, FALSE); KeepAnimating(hwnd, true); }
        return 0;
    case WM_CAPTURECHANGED:
        FinishStroke(*e);
        e->erasing = false;
        e->cropHandle = -1;
        e->pressed = -1;
        return 0;
    case WM_KEYDOWN: {
        const bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if (ctrl && wp == 'Z') Undo(*e, true);
        else if (ctrl && wp == 'Y') Undo(*e, false);
        else if (ctrl && wp == 'C') CopyResult(*e);
        else if (ctrl && wp == 'S') SaveAs(*e);
        else if (e->cropping && wp == VK_ESCAPE) EndCrop(*e, false);
        else if (e->cropping && wp == VK_RETURN) EndCrop(*e, true);
        return 0;
    }
    case WM_APP_ANIMFRAME:
        SendMessageW(hwnd, WM_TIMER, kAnimTimer, 0);
        if (IsWindow(hwnd)) UpdateWindow(hwnd);
        AnimationFrameDelivered(hwnd);
        return 0;
    case WM_TIMER:
        if (wp == kCopyTimer) {
            if (!CopyPendingResult(*e)) Log(L"editor: clipboard copy failed");
        } else if (wp == kAnimTimer) {
            const RECT bar{0, 0, e->cw, BarH(*e) + Di(*e, 44)};  // toolbar and its tooltip
            InvalidateRect(hwnd, &bar, FALSE);
            bool running = false;
            for (const auto& kv : e->hoverT) running |= kv.second.Running();
            for (const auto& kv : e->selectedT) running |= kv.second.Running();
            KeepAnimating(hwnd, running);
        }
        return 0;
    case WM_QUERYENDSESSION:
        return CanEndEditorSession() ? TRUE : FALSE;
    case WM_ENDSESSION:
        // Querying/canceling a shutdown must not close editors or copy their images.
        if (!wp) UpdateShutdownReason(*e);
        return 0;
    case WM_CLOSE:
        if (!ConfirmClose(*e)) return 0;
        DestroyWindow(hwnd);
        return 0;
    case WM_NCDESTROY:
        ShutdownBlockReasonDestroy(hwnd);
        KeepAnimating(hwnd, false);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (!e->creating) FreeEditor(e);  // the caller owns it until CreateWindowEx succeeds
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}  // namespace

void OpenEditor(const ::Image& img, HICON icon) {
    if (g_closingEditors) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof wc};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hIcon = wc.hIconSm = icon;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0;
        if (!registered) return;
    }
    auto* e = new Editor;
    e->base = img;
    e->baseBmp = ToGdiplus(e->base);
    if (!e->baseBmp) { delete e; return; }
    e->doc.crop = {0, 0, img.w, img.h};
    for (uint32_t p : img.px)
        if ((p >> 24) != 255) { e->hasAlpha = true; break; }
    POINT cur;
    GetCursorPos(&cur);
    HMONITOR hm = MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(hm, &mi);
    e->dpi = MonitorScale(hm);
    const RECT& wa = mi.rcWork;
    const int maxW = (wa.right - wa.left) * 9 / 10, maxH = (wa.bottom - wa.top) * 9 / 10;
    RECT r{0, 0, std::max(img.w + Di(*e, 32), ToolbarMinWidth(*e)), img.h + BarH(*e) + Di(*e, 32)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, UINT(e->dpi * 96));
    const int w = std::min<int>(r.right - r.left, maxW), h = std::min<int>(r.bottom - r.top, maxH);
    HWND hwnd = CreateWindowExW(0, kClass, Tr(L"截图编辑 - Screenshot", L"Editor - Screenshot"), WS_OVERLAPPEDWINDOW, wa.left + (wa.right - wa.left - w) / 2,
                                wa.top + (wa.bottom - wa.top - h) / 2, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), e);
    if (!hwnd) { FreeEditor(e); return; }
    e->creating = false;
    ApplyWindowTheme(hwnd);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
}

bool CloseAllEditors() {
    if (g_closingEditors) return false;
    g_closingEditors = true;
    std::vector<HWND> windows;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM value) -> BOOL {
        wchar_t name[64]{};
        if (GetClassNameW(hwnd, name, _countof(name)) && !lstrcmpW(name, kClass))
            reinterpret_cast<std::vector<HWND>*>(value)->push_back(hwnd);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));
    for (HWND hwnd : windows) {
        auto* e = reinterpret_cast<Editor*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (e && !ConfirmClose(*e)) { g_closingEditors = false; return false; }
    }
    for (HWND hwnd : windows) if (IsWindow(hwnd)) DestroyWindow(hwnd);
    g_closingEditors = false;
    return true;
}

bool CanEndEditorSession() {
    bool accepted = true;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM value) -> BOOL {
        wchar_t name[64]{};
        if (GetClassNameW(hwnd, name, _countof(name)) && !lstrcmpW(name, kClass)) {
            const auto* e = reinterpret_cast<Editor*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (e) {
                UpdateShutdownReason(*e);
                if (HasUnsavedChanges(*e)) *reinterpret_cast<bool*>(value) = false;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&accepted));
    return accepted;
}
