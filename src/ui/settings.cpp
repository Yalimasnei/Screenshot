// SPDX-License-Identifier: GPL-3.0-only
// Settings window: capture mode, save folder, shortcuts, theme, startup.
// Drawn with GDI+ in the app's light/dark palette; every change applies at once.
#include "app/application.h"
#include "platform/windows_paths.h"
#include "ui/ui_common.h"
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <winver.h>
#include <cmath>
#include <map>

using namespace Gdiplus;

namespace {
const wchar_t kClass[] = L"ScreenshotSettings";
enum Id { MODE, AUTOSAVE, BROWSE, RESETDIR, PRTSCN, WINSHIFTS, HOTKEY, CLEARKEY, THEME, STARTUP, FORMAT, TOAST, HOMEPAGE, BLOCKWIN, CHROMA, LANGUAGE, SELECTIONSIZE };
const wchar_t kHomepage[] = L"https://github.com/Yalimasnei/Screenshot";
const wchar_t kAuthor[] = L"Yalimasnei";

struct Hit {
    Id id;
    int sub;  // segment index
    RECT r;
};
struct Settings {
    bool creating = true;
    HWND hwnd = nullptr;
    float dpi = 1;
    int cw = 0, ch = 0;
    std::vector<Hit> hits;
    int hotId = -1, hotSub = -1;
    bool recording = false;
    std::wstring status;  // hotkey error / hint
    std::wstring lastLayout;
    std::wstring saveFolder;
    bool saveFolderValid = false;
    int contentH = 0;     // client height the rows need, from the last paint
    std::map<int, Tween> anim;   // per-control tweens, keyed by Key(id, slot)
    std::map<int, int> segSel;   // last drawn selection of each segmented control
    std::map<std::pair<int, std::wstring>, float> textWidths;
    int rowIndex = 0;            // row counter while painting
    std::map<int, std::wstring> rowNote;  // note each row shows (kept while it fades out)
    std::map<int, std::wstring> previousRowNote;
    HBITMAP lastFrame = nullptr; // previous frame, kept for the theme cross-fade
    HBITMAP fromFrame = nullptr; // frame from before the theme changed
    HBITMAP dragContent = nullptr;
    std::vector<Hit> dragHits;
    bool hoverDirty = false;
    Tween themeFade;             // 1 -> 0: how much of fromFrame still shows
    // Scrolling. Content is laid out `lw` wide, shifted by (ox, -scroll).
    Tween scroll;                // scroll position in px (eases to its target)
    float lw = 0, ox = 0, oy = 0;
    bool fitted = false;         // initial height set from the content
    bool sbDrag = false, sbPress = false;
    float sbGrab = 0;            // pointer offset inside the thumb while dragging
    RECT thumb{};                // scrollbar thumb, client coords
    Tween sbWide;                // 0 thin .. 1 widened while the pointer is near it
};
constexpr UINT WM_FIT = WM_APP + 1;
Settings* g_set = nullptr;

int Key(int id, int slot) { return id * 64 + slot + 8; }

// Eases the tween for (id, slot) towards `target` and returns its value.
float Anim(Settings& s, int id, int slot, float target, double ms) {
    Tween& t = s.anim[Key(id, slot)];
    t.Go(target, ms);
    return t.Value();
}

bool AnyRunning(const Settings& s) {
    if (s.themeFade.Running() || s.scroll.Running() || s.sbWide.Running()) return true;
    for (const auto& kv : s.anim)
        if (kv.second.Running()) return true;
    return false;
}

float D(const Settings& s, float v) { return v * s.dpi; }

std::wstring KeyName(UINT vk) {
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::wstring(1, wchar_t(vk));
    switch (vk) {
    case VK_SNAPSHOT: return L"PrtScn";
    case VK_SPACE: return L"Space";
    case VK_INSERT: return L"Insert";
    case VK_DELETE: return L"Delete";
    case VK_HOME: return L"Home";
    case VK_END: return L"End";
    case VK_PRIOR: return L"PageUp";
    case VK_NEXT: return L"PageDown";
    case VK_PAUSE: return L"Pause";
    case VK_LEFT: return L"←";
    case VK_RIGHT: return L"→";
    case VK_UP: return L"↑";
    case VK_DOWN: return L"↓";
    }
    wchar_t name[64] = L"";
    const UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (sc && GetKeyNameTextW(LONG(sc << 16), name, 64) > 0) return name;
    return L"0x" + std::to_wstring(vk);
}

std::wstring ComboText(DWORD combo) {
    if (!combo) return Tr(L"无", L"None");
    const UINT mods = combo >> 16, vk = combo & 0xFFFF;
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl + ";
    if (mods & MOD_ALT) s += L"Alt + ";
    if (mods & MOD_SHIFT) s += L"Shift + ";
    if (mods & MOD_WIN) s += L"Win + ";
    return s + KeyName(vk);
}

struct Ctx {
    Settings& s;
    Graphics& g;
    const Palette& th;
    Font& body;
    Font& small;
    Font& head;
    bool Hot(Id id, int sub = -1) const { return s.hotId == id && (sub < 0 || s.hotSub == sub); }
    float HotT(Id id, int sub = -1) const { return Anim(s, id, sub < 0 ? 0 : 1 + sub, Hot(id, sub) ? 1.f : 0.f, 110); }
    void Add(Id id, int sub, RectF r) {  // stored in client coordinates (scroll applied)
        r.Offset(s.ox, s.oy);
        s.hits.push_back({id, sub, {LONG(r.X), LONG(r.Y), LONG(r.GetRight()), LONG(r.GetBottom())}});
    }
    float TextW(const wchar_t* t, Font& f) const {
        const int font = &f == &body ? 0 : &f == &small ? 1 : 2;
        const auto key = std::make_pair(font, std::wstring(t));
        auto found = s.textWidths.find(key);
        if (found != s.textWidths.end()) return found->second;
        RectF m;
        g.MeasureString(t, -1, &f, PointF(0, 0), &m);
        s.textWidths.emplace(key, m.Width);
        return m.Width;
    }
    void Text(const wchar_t* t, Font& f, Color c, RectF r, bool center = false) const {
        if (r.GetBottom() + s.oy <= 0 || r.Y + s.oy >= s.ch) return;
        StringFormat fmt;
        fmt.SetLineAlignment(StringAlignmentCenter);
        if (center) fmt.SetAlignment(StringAlignmentCenter);
        fmt.SetTrimming(StringTrimmingEllipsisCharacter);
        fmt.SetFormatFlags(StringFormatFlagsNoWrap);
        SolidBrush b(c);
        g.DrawString(t, -1, &f, r, &fmt, &b);
    }
};

// Each control draws right-aligned, ending at `right`, centred on `cy`;
// it returns its left edge. Hover, knob and selection changes are tweened.
float Toggle(Ctx& c, Id id, bool on, float right, float cy) {
    const float w = D(c.s, 40), h = D(c.s, 20), x = right - w, y = cy - h / 2;
    const RectF r(x, y, w, h);
    const float t = Anim(c.s, id, 40, on ? 1.f : 0.f, 170);  // 0 off .. 1 on
    const float hv = c.HotT(id);
    c.g.SetSmoothingMode(SmoothingModeAntiAlias);
    const Color clearAccent(0, c.th.accent.GetR(), c.th.accent.GetG(), c.th.accent.GetB());
    if (t > 0.01f) { SolidBrush fill(Mix(clearAccent, c.th.accent, t)); FillRoundRect(c.g, r, h / 2, fill); }
    if (t < 0.99f) {
        const Color edgeC = Mix(c.th.muted, c.th.text, hv);
        Pen edge(Mix(edgeC, Color(0, edgeC.GetR(), edgeC.GetG(), edgeC.GetB()), t), D(c.s, 1));
        StrokeRoundRect(c.g, r, h / 2, edge);
    }
    const float k = D(c.s, 12 + 2 * t);
    const Color knobOn = c.th.dark ? Color(255, 0, 0, 0) : Color(255, 255, 255, 255);
    SolidBrush knob(Mix(c.th.muted, knobOn, t));
    const float kx0 = x + h / 2 - k / 2, kx1 = x + w - h / 2 - k / 2;
    c.g.FillEllipse(&knob, kx0 + (kx1 - kx0) * t, cy - k / 2, k, k);
    c.g.SetSmoothingMode(SmoothingModeDefault);
    const float tw = D(c.s, English() ? 30 : 22);
    c.Text(on ? Tr(L"开", L"On") : Tr(L"关", L"Off"), c.body, c.th.text, RectF(x - tw - D(c.s, 8), cy - D(c.s, 12), tw, D(c.s, 24)));
    RectF hit(x - tw - D(c.s, 10), cy - D(c.s, 16), w + tw + D(c.s, 12), D(c.s, 32));
    c.Add(id, -1, hit);
    return hit.X;
}

float Button(Ctx& c, Id id, const wchar_t* label, float right, float cy, float appear = 1) {
    const float w = c.TextW(label, c.body) + D(c.s, 26), h = D(c.s, 30);
    const RectF r(right - w, cy - h / 2, w, h);
    auto fadeIn = [appear](Color col) { return Color(BYTE(col.GetA() * appear), col.GetR(), col.GetG(), col.GetB()); };
    SolidBrush fill(fadeIn(Mix(c.th.bar, c.th.hover, c.HotT(id))));
    FillRoundRect(c.g, r, D(c.s, 4), fill);
    Pen edge(fadeIn(c.th.separator), 1);
    StrokeRoundRect(c.g, r, D(c.s, 4), edge);
    c.Text(label, c.body, fadeIn(c.th.text), r, true);
    c.Add(id, -1, r);
    return r.X;
}

// Plain text on the right of a row (About page values).
float Value(Ctx& c, const wchar_t* text, float right, float cy) {
    const float w = c.TextW(text, c.body) + D(c.s, 2);
    c.Text(text, c.body, c.th.muted, RectF(right - w, cy - D(c.s, 12), w, D(c.s, 24)));
    return right - w;
}

// A web link on the right of a row; underlined while the pointer is on it.
float Link(Ctx& c, Id id, const wchar_t* text, float right, float cy) {
    const float w = c.TextW(text, c.body) + D(c.s, 2), h = D(c.s, 24);
    const RectF r(right - w, cy - h / 2, w, h);
    const float hv = c.HotT(id);
    const Color col = Mix(c.th.accent, c.th.text, hv * 0.35f);
    c.Text(text, c.body, col, r);
    if (hv > 0.01f) {
        Pen line(Color(BYTE(255 * hv), col.GetR(), col.GetG(), col.GetB()), 1);
        const float ly = cy + D(c.s, 9);
        c.g.DrawLine(&line, r.X + D(c.s, 1), ly, r.GetRight() - D(c.s, 2), ly);
    }
    c.Add(id, -1, r);
    return r.X;
}

// Version from the exe's version resource ("1.0.0"), so it never drifts.
std::wstring AppVersion() {
    const std::wstring exe = ModulePath();
    if (exe.empty()) return L"";
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &dummy);
    std::vector<BYTE> buf(size);
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (!size || !GetFileVersionInfoW(exe.c_str(), 0, size, buf.data()) ||
        !VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&fi), &len) || !fi)
        return L"";
    return std::to_wstring(HIWORD(fi->dwFileVersionMS)) + L"." + std::to_wstring(LOWORD(fi->dwFileVersionMS)) + L"." +
           std::to_wstring(HIWORD(fi->dwFileVersionLS));
}

// Fade-in for controls that only sometimes exist: 0 -> 1 when one appears.
// Call it every paint with whether the control is drawn this time.
float Appear(Settings& s, Id id, bool shown) {
    Tween& t = s.anim[Key(id, 60)];
    if (!shown) { t.Jump(0); return 0; }
    if (!t.set) { t.Jump(1); return 1; }  // already there when the window opened
    t.Go(1, 180);
    return t.Value();
}

float Segmented(Ctx& c, Id id, const std::vector<const wchar_t*>& items, int sel, float right, float cy,
                unsigned disabled = 0) {
    const float pad = D(c.s, 12), h = D(c.s, 30), gap = D(c.s, 2);
    float total = gap;
    for (auto t : items) total += c.TextW(t, c.body) + 2 * pad + gap;
    float x = right - total;
    const float left = x;
    SolidBrush track(c.th.hover);
    FillRoundRect(c.g, RectF(x, cy - h / 2, total, h), D(c.s, 5), track);
    x += gap;
    std::vector<RectF> rs;
    for (auto t : items) {
        const float w = c.TextW(t, c.body) + 2 * pad;
        rs.emplace_back(x, cy - h / 2 + gap, w, h - 2 * gap);
        x += w + gap;
    }
    // Hover fades, then the selected pill, which slides when the selection changes.
    const Color clearSel(0, c.th.selected.GetR(), c.th.selected.GetG(), c.th.selected.GetB());
    for (int i = 0; i < int(rs.size()); ++i) {
        const float hv = (disabled & (1u << i)) ? 0.f : c.HotT(id, i);
        if (i != sel && hv > 0.01f) { SolidBrush b(Mix(clearSel, c.th.selected, hv)); FillRoundRect(c.g, rs[i], D(c.s, 4), b); }
    }
    if (sel >= 0 && sel < int(rs.size())) {
        Tween &px = c.s.anim[Key(id, 50)], &pw = c.s.anim[Key(id, 51)];
        const float tx = rs[sel].X - left, tw = rs[sel].Width;
        auto last = c.s.segSel.find(id);
        if (last == c.s.segSel.end()) {
            px.Jump(tx);  // first paint
            pw.Jump(tw);
        } else if (last->second != sel) {
            px.Go(tx, 200);  // selection changed: slide
            pw.Go(tw, 200);
        } else if (!px.Running() && (px.to != tx || pw.to != tw)) {
            px.Jump(tx);  // same selection, layout moved (resize, DPI): follow at once
            pw.Jump(tw);
        }
        c.s.segSel[id] = sel;
        SolidBrush on(c.th.accent);
        FillRoundRect(c.g, RectF(left + px.Value(), rs[sel].Y, pw.Value(), rs[sel].Height), D(c.s, 4), on);
    }
    for (int i = 0; i < int(rs.size()); ++i) {
        // Text turns light as the sliding pill covers it.
        float cover = 0;
        if (sel >= 0 && sel < int(rs.size())) {
            const float pl = left + c.s.anim[Key(id, 50)].Value(), pr = pl + c.s.anim[Key(id, 51)].Value();
            cover = std::clamp((std::min(pr, rs[i].GetRight()) - std::max(pl, rs[i].X)) / rs[i].Width, 0.f, 1.f);
        }
        const Color onText = c.th.dark ? Color(255, 0, 0, 0) : Color(255, 255, 255, 255);
        const bool enabled = !(disabled & (1u << i));
        c.Text(items[i], c.body, enabled ? Mix(c.th.text, onText, cover) : c.th.disabled, rs[i], true);
        if (enabled) c.Add(id, i, rs[i]);
    }
    return left;
}

std::wstring HeldModifiers() {
    std::wstring s;
    if (GetKeyState(VK_CONTROL) < 0) s += L"Ctrl + ";
    if (GetKeyState(VK_MENU) < 0) s += L"Alt + ";
    if (GetKeyState(VK_SHIFT) < 0) s += L"Shift + ";
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) s += L"Win + ";
    return s;
}

float HotkeyBox(Ctx& c, float right, float cy) {
    const float w = D(c.s, 190), h = D(c.s, 30);
    const RectF r(right - w, cy - h / 2, w, h);
    const float rec = Anim(c.s, HOTKEY, 41, c.s.recording ? 1.f : 0.f, 150);  // recording highlight
    SolidBrush fill(Mix(Mix(c.th.bar, c.th.hover, c.HotT(HOTKEY)), c.th.bar, rec));
    FillRoundRect(c.g, r, D(c.s, 4), fill);
    Pen edge(Mix(c.th.separator, c.th.accent, rec), D(c.s, 1 + rec));
    StrokeRoundRect(c.g, r, D(c.s, 4), edge);
    const std::wstring text = c.s.recording ? (HeldModifiers().empty() ? Tr(L"请按下快捷键…", L"Press a shortcut…") : HeldModifiers() + L"…")
                                            : ComboText(GetCustomHotkey());
    c.Text(text.c_str(), c.body, c.s.recording || GetCustomHotkey() ? c.th.text : c.th.muted, r, true);
    c.Add(HOTKEY, -1, r);
    return r.X;
}

// A card row: title (and optional note) on the left; controls drawn by
// `controls` on the right, which returns where they start. When the note
// appears, changes or goes, the row grows / shrinks and the text fades.
template <class F> void Row(Ctx& c, float& y, const wchar_t* title, const std::wstring& note, F controls,
                            Color noteColor = Color(0, 0, 0, 0), bool visible = true, bool stackControl = false) {
    const int row = c.s.rowIndex++;
    Tween& visibility = c.s.anim[Key(2000 + row, 0)];
    visibility.Go(visible ? 1.f : 0.f, 180);
    const float reveal = visibility.Value();
    if (reveal <= 0 && !visibility.Running()) return;
    std::wstring& shown = c.s.rowNote[row];  // what the note area shows (kept while it fades out)
    std::wstring& previous = c.s.previousRowNote[row];
    Tween& grow = c.s.anim[Key(1000 + row, 0)];
    Tween& ink = c.s.anim[Key(1000 + row, 1)];
    const bool first = !grow.set;
    if (!note.empty() && note != shown) {
        const bool wasEmpty = shown.empty() || grow.to == 0;
        previous = wasEmpty ? L"" : shown;
        shown = note;
        if (first) ink.Jump(1);
        else { if (!wasEmpty) ink.Jump(0); ink.Go(1, 180); }
    }
    grow.Go(note.empty() ? 0.f : 1.f, 180);
    if (note.empty()) ink.Go(0, 120);
    const float gv = grow.Value(), iv = ink.Value();
    if (!ink.Running()) previous.clear();
    if (note.empty() && !grow.Running() && !ink.Running()) shown.clear();

    const float extra = stackControl ? D(c.s, 40) : 0;
    const float m = D(c.s, 20), h = D(c.s, 52 + 10 * gv) + extra;
    if (y + h * reveal + c.s.oy <= 0 || y + c.s.oy >= c.s.ch) {
        y += (h + D(c.s, 4)) * reveal;
        return;
    }
    const RectF card(m, y, c.s.lw - 2 * m, h);
    const GraphicsState rowState = c.g.Save();
    c.g.SetClip(RectF(0, y, c.s.lw, h * reveal), CombineModeIntersect);
    const size_t firstHit = c.s.hits.size();
    SolidBrush bg(c.th.dark ? Color(255, 43, 43, 43) : Color(255, 251, 251, 251));
    FillRoundRect(c.g, card, D(c.s, 6), bg);
    Pen edge(c.th.dark ? Color(255, 29, 29, 29) : Color(255, 229, 229, 229), 1);
    StrokeRoundRect(c.g, card, D(c.s, 6), edge);
    const float cy = y + (h - extra) / 2 + extra;
    const float left = controls(card.GetRight() - D(c.s, 16), cy);
    const float tx = card.X + D(c.s, 16);
    const float tw = stackControl ? card.Width - D(c.s, 32) : std::max(D(c.s, 40), left - tx - D(c.s, 12));
    // Title moves from the vertical centre up to its place above the note.
    const float titleY = y + (h - extra - D(c.s, 22)) / 2 * (1 - gv) + D(c.s, 9) * gv;
    c.Text(title, c.body, c.th.text, RectF(tx, titleY, tw, D(c.s, 22)));
    if (!shown.empty() || !previous.empty()) {
        const Color base = noteColor.GetA() ? noteColor : c.th.muted;
        const GraphicsState noteState = c.g.Save();
        c.g.SetClip(card, CombineModeIntersect);
        const RectF noteRect(tx, titleY + D(c.s, 22), tw, D(c.s, 20));
        if (!previous.empty() && iv < 0.99f)
            c.Text(previous.c_str(), c.small, Color(BYTE(base.GetA() * (1 - iv)), base.GetR(), base.GetG(), base.GetB()), noteRect);
        if (iv > 0.01f)
            c.Text(shown.c_str(), c.small, Color(BYTE(base.GetA() * iv), base.GetR(), base.GetG(), base.GetB()), noteRect);
        c.g.Restore(noteState);
    }
    c.g.Restore(rowState);
    const LONG bottom = LONG(std::ceil(y + h * reveal + c.s.oy));
    for (size_t i = firstHit; i < c.s.hits.size(); ++i) c.s.hits[i].r.bottom = std::min(c.s.hits[i].r.bottom, bottom);
    if (!visible) c.s.hits.resize(firstHit);
    y += (h + D(c.s, 4)) * reveal;
}

void Section(Ctx& c, float& y, const wchar_t* title) {
    y += D(c.s, 14);
    c.Text(title, c.head, c.th.text, RectF(D(c.s, 22), y, c.s.lw - D(c.s, 44), D(c.s, 26)));
    y += D(c.s, 32);
}

const Hit* HitAt(const Settings& s, POINT p) {
    for (const Hit& h : s.hits)
        if (PtInRect(&h.r, p)) return &h;
    return nullptr;
}

// ---- scrolling ----
float MaxScroll(const Settings& s) { return std::max(0.f, float(s.contentH - s.ch)); }

void ScrollTo(Settings& s, float target, bool animate = true) {
    target = std::clamp(target, 0.f, MaxScroll(s));
    if (target == s.scroll.to && !s.scroll.Running()) return;
    if (animate) s.scroll.Go(target, 160);
    else s.scroll.Jump(target);
    InvalidateRect(s.hwnd, nullptr, FALSE);
    if (animate) KeepAnimating(s.hwnd, true);
}

// Strip on the right edge the scrollbar lives in (also its hit area).
RECT ScrollStrip(const Settings& s) { return {s.cw - int(D(s, 14)), 0, s.cw, s.ch}; }

RECT ThumbRect(const Settings& s, float width) {
    const float view = float(s.ch), total = float(std::max(s.contentH, 1));
    const float pad = D(s, 3), track = view - 2 * pad;
    const float th = std::max(D(s, 32), track * view / total);
    const float maxs = MaxScroll(s), pos = maxs > 0 ? s.scroll.Value() / maxs : 0;
    const float top = pad + (track - th) * pos;
    const float right = s.cw - D(s, 3);
    return {LONG(right - width), LONG(top), LONG(right), LONG(top + th)};
}

// Thin bar while idle; widens with a track behind it when the pointer is near.
void DrawScrollbar(Settings& s, Graphics& g, const Palette& th) {
    if (MaxScroll(s) <= 0) { s.thumb = {}; return; }
    const float wv = s.sbWide.Value();
    const float width = D(s, 3 + 5 * wv);
    s.thumb = ThumbRect(s, width);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    if (wv > 0.01f) {
        const Color tr = th.dark ? Color(255, 255, 255, 255) : Color(255, 0, 0, 0);
        SolidBrush track(Color(BYTE((th.dark ? 30 : 14) * wv), tr.GetR(), tr.GetG(), tr.GetB()));
        FillRoundRect(g, RectF(s.cw - D(s, 3) - width, D(s, 3), width, s.ch - D(s, 6)), width / 2, track);
    }
    const Color base = th.muted;
    SolidBrush thumb(Color(BYTE(150 + 80 * (s.sbDrag ? 1.f : wv)), base.GetR(), base.GetG(), base.GetB()));
    const RECT& t = s.thumb;
    FillRoundRect(g, RectF(REAL(t.left), REAL(t.top), REAL(t.right - t.left), REAL(t.bottom - t.top)), width / 2, thumb);
    g.SetSmoothingMode(SmoothingModeDefault);
}

// Re-reads which control is under the pointer (content can scroll under a still mouse).
void UpdateHover(Settings& s) {
    POINT p;
    GetCursorPos(&p);
    if (WindowFromPoint(p) != s.hwnd) return;
    ScreenToClient(s.hwnd, &p);
    const RECT strip = ScrollStrip(s);
    const Hit* h = (PtInRect(&strip, p) && MaxScroll(s) > 0) || s.sbDrag ? nullptr : HitAt(s, p);
    const int id = h ? int(h->id) : -1, sub = h ? h->sub : -1;
    if (id != s.hotId || sub != s.hotSub) {
        s.hotId = id;
        s.hotSub = sub;
        s.hoverDirty = true;
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
}

void PaintFull(Settings& s, HDC dc) {
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1, s.cw), std::max(1, s.ch));
    HGDIOBJ old = SelectObject(mem, bmp);
    {
        Graphics g(mem);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        const Palette& th = Theme();
        g.Clear(th.dark ? Color(255, 32, 32, 32) : Color(255, 243, 243, 243));
        Font body(Tr(L"Microsoft YaHei UI", L"Segoe UI"), D(s, 14), FontStyleRegular, UnitPixel);
        Font small(Tr(L"Microsoft YaHei UI", L"Segoe UI"), D(s, 12), FontStyleRegular, UnitPixel);
        Font head(Tr(L"Microsoft YaHei UI", L"Segoe UI"), D(s, 15), FontStyleBold, UnitPixel);
        Ctx c{s, g, th, body, small, head};
        s.hits.clear();
        s.rowIndex = 0;
        // Rows follow the window width up to a comfortable maximum, then centre.
        s.lw = std::min(float(s.cw), D(s, 1000));
        s.ox = std::floor((s.cw - s.lw) / 2);
        s.oy = -std::round(s.scroll.Value());
        g.TranslateTransform(s.ox, s.oy);
        float y = D(s, 4);

        Section(c, y, Tr(L"截图", L"Capture"));
        Row(c, y, Tr(L"截图栏默认模式", L"Default capture mode"), Tr(L"按快捷键后截图栏先选中的模式", L"Mode selected when capture starts"), [&](float r, float cy) {
            return Segmented(c, MODE, {Tr(L"上次使用", L"Last used"), Tr(L"矩形", L"Rectangle"), Tr(L"窗口", L"Window"), Tr(L"全屏", L"Full screen"), Tr(L"任意形状", L"Freeform")}, GetDefaultMode(), r, cy);
        }, Color(0, 0, 0, 0), true, English());
        Row(c, y, Tr(L"显示选区尺寸", L"Show selection size"), Tr(L"显示选区的宽 × 高（像素）", L"Show selection width × height in pixels"), [&](float r, float cy) {
            return Toggle(c, SELECTIONSIZE, GetShowSelectionSize(), r, cy);
        });
        Row(c, y, Tr(L"截图后弹出通知", L"Show capture notifications"), Tr(L"右下角显示缩略图，单击可打开编辑器；出错时总会提示", L"Click the thumbnail to edit. Errors are always shown."), [&](float r, float cy) {
            return Toggle(c, TOAST, GetShowToast(), r, cy);
        });

        Section(c, y, Tr(L"保存", L"Save"));
        Row(c, y, Tr(L"自动保存截图", L"Save screenshots automatically"), Tr(L"截图后同时保存到下面的文件夹", L"Also save captures to the folder below"), [&](float r, float cy) {
            return Toggle(c, AUTOSAVE, GetAutoSave(), r, cy);
        });
        const int fmt = GetSaveFormat();
        const auto& nvenc = GetNvencCapabilities();
        const int displayBits = DisplayBitDepth() >= 10 ? 10 : 8;
        int nvencBits = displayBits;
        bool nvenc444 = GetNvenc444();
        if (!nvenc.Supports(nvencBits, nvenc444)) { nvencBits = 8; nvenc444 = false; }
        std::wstring fmtNote;  // spec of the selected HEIC encoder
        if (fmt == 1) {
            fmtNote = nvenc444 ? L"HEVC · 4:4:4 · " : L"HEVC · 4:2:0 · ";
            fmtNote += nvencBits == 10 ? Tr(L"10 位", L"10-bit") : Tr(L"8 位", L"8-bit");
        } else if (fmt == 2) {
            fmtNote = HeicWindowsAvailable() ? Tr(L"HEVC · 4:2:0 · 8 位", L"HEVC · 4:2:0 · 8-bit")
                                             : Tr(L"未安装 HEIF 图像扩展，会存成 PNG", L"HEIF Image Extensions is missing; saves as PNG");
        }
        Row(c, y, Tr(L"保存格式", L"File format"), fmtNote, [&](float r, float cy) {
            return Segmented(c, FORMAT, {L"PNG", L"NVENC", L"HEIC"}, fmt, r, cy, nvenc.available ? 0 : 1u << 1);
        });
        Row(c, y, Tr(L"色度采样", L"Chroma sampling"), Tr(L"4:4:4 保留更多彩色细节，文件通常更大", L"More color detail; larger files"), [&](float r, float cy) {
            return Segmented(c, CHROMA, {L"4:2:0", L"4:4:4"}, nvenc444 ? 1 : 0, r, cy,
                             nvenc.Supports(displayBits, true) ? 0 : 1u << 1);
        }, Color(0, 0, 0, 0), fmt == 1);
        const bool custom = !GetCustomSaveDir().empty();
        const float resetIn = Appear(s, RESETDIR, custom);
        if (!s.saveFolderValid) { s.saveFolder = SaveFolder(); s.saveFolderValid = true; }
        Row(c, y, Tr(L"保存位置", L"Save location"), s.saveFolder + (custom ? L"" : Tr(L"（默认）", L" (default)")), [&](float r, float cy) {
            float x = Button(c, BROWSE, Tr(L"更改…", L"Change…"), r, cy);
            if (custom) x = Button(c, RESETDIR, Tr(L"恢复默认", L"Reset"), x - D(s, 8), cy, resetIn);
            return x;
        });

        Section(c, y, Tr(L"快捷键", L"Shortcuts"));
        const bool block = GetBlockWinShots();
        Row(c, y, Tr(L"运行时屏蔽 Windows 截图快捷键", L"Block Windows capture shortcuts"), Tr(L"Win+Shift+S、Win+Shift+R、PrtScn、Win+PrtScn、Alt+PrtScn 不会呼出 Windows 截图；退出后恢复", L"Blocks Windows capture shortcuts while Screenshot is running"),
            [&](float r, float cy) { return Toggle(c, BLOCKWIN, block, r, cy); });
        Row(c, y, Tr(L"用 PrtScn 键截图", L"Capture with PrtScn"), block ? Tr(L"关闭后按 PrtScn 不做任何事", L"When off, PrtScn does nothing") : Tr(L"关闭后 PrtScn 恢复 Windows 原来的功能", L"When off, PrtScn uses the Windows action"), [&](float r, float cy) {
            return Toggle(c, PRTSCN, GetTakePrtScn(), r, cy);
        });
        Row(c, y, Tr(L"用 Win + Shift + S 截图", L"Capture with Win + Shift + S"), block ? Tr(L"关闭后这个组合键不做任何事", L"When off, this shortcut does nothing") : Tr(L"关闭后这个组合键交还给 Windows 截图工具", L"When off, this shortcut opens Windows Snipping Tool"), [&](float r, float cy) {
            return Toggle(c, WINSHIFTS, GetTakeWinShiftS(), r, cy);
        });
        const bool err = !s.status.empty();
        const bool clearShown = GetCustomHotkey() && !s.recording;
        const float clearIn = Appear(s, CLEARKEY, clearShown);
        Row(c, y, Tr(L"自定义快捷键", L"Custom shortcut"),
            err ? s.status : s.recording ? std::wstring(Tr(L"按下组合键（Esc 取消，Backspace 清除）；没有反应说明已被其他程序占用", L"Press a shortcut. Esc cancels; Backspace clears. Another app may be using it."))
                                         : std::wstring(Tr(L"单击右侧方框后按下组合键", L"Click the box and press a shortcut")),
            [&](float r, float cy) {
                float x = r;
                if (clearShown) x = Button(c, CLEARKEY, Tr(L"清除", L"Clear"), x, cy, clearIn) - D(s, 8);
                return HotkeyBox(c, x, cy);
            },
            err ? Color(255, 196, 43, 28) : Color(0, 0, 0, 0));

        Section(c, y, Tr(L"外观", L"Appearance"));
        Row(c, y, Tr(L"语言", L"Language"), L"", [&](float r, float cy) {
            return Segmented(c, LANGUAGE, {Tr(L"跟随系统", L"System"), L"中文", L"English"}, GetLanguageSetting(), r, cy);
        });
        Row(c, y, Tr(L"主题", L"Theme"), L"", [&](float r, float cy) {
            return Segmented(c, THEME, {Tr(L"跟随系统", L"System"), Tr(L"浅色", L"Light"), Tr(L"深色", L"Dark")}, GetThemeSetting(), r, cy);
        });

        Section(c, y, Tr(L"其他", L"Other"));
        Row(c, y, Tr(L"开机启动", L"Start with Windows"), L"", [&](float r, float cy) { return Toggle(c, STARTUP, GetStartup(), r, cy); });

        Section(c, y, Tr(L"关于", L"About"));
        static const std::wstring version = AppVersion();
        Row(c, y, L"Screenshot", L"", [&](float r, float cy) {
            return Value(c, (Tr(L"版本 ", L"Version ") + version).c_str(), r, cy);
        });
        Row(c, y, Tr(L"作者", L"Author"), L"", [&](float r, float cy) { return Value(c, kAuthor, r, cy); });
        Row(c, y, Tr(L"项目地址", L"Project URL"), L"", [&](float r, float cy) { return Link(c, HOMEPAGE, kHomepage, r, cy); });
        s.contentH = int(y + D(s, 16));
        g.ResetTransform();
        // Keep the scroll position valid as rows grow / shrink or the window resizes.
        const float maxScroll = MaxScroll(s);
        if (s.scroll.to > maxScroll) {
            if (s.scroll.Running()) s.scroll.to = maxScroll;
            else s.scroll.Jump(maxScroll);
        }
        DrawScrollbar(s, g, th);
    }
    // Theme switch: the old frame fades out over the new one.
    const float fromA = s.fromFrame ? s.themeFade.Value() : 0.f;
    if (fromA > 0.01f) {
        HDC from = CreateCompatibleDC(dc);
        HGDIOBJ oldFrom = SelectObject(from, s.fromFrame);
        BITMAP bm{};
        GetObjectW(s.fromFrame, sizeof bm, &bm);
        const BLENDFUNCTION bf = {AC_SRC_OVER, 0, BYTE(255 * fromA), 0};
        const int w = std::min<int>(s.cw, bm.bmWidth), h = std::min<int>(s.ch, bm.bmHeight);
        AlphaBlend(mem, 0, 0, w, h, from, 0, 0, w, h, bf);
        SelectObject(from, oldFrom);
        DeleteDC(from);
    } else if (s.fromFrame && !s.themeFade.Running()) {
        DeleteObject(s.fromFrame);
        s.fromFrame = nullptr;
    }
    BitBlt(dc, 0, 0, s.cw, s.ch, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    if (s.lastFrame) DeleteObject(s.lastFrame);
    s.lastFrame = bmp;  // kept as the starting picture of a theme cross-fade
    if (!s.fitted && s.contentH) PostMessageW(s.hwnd, WM_FIT, 0, 0);  // first paint: size the window to the content
    const bool moving = AnyRunning(s);
    KeepAnimating(s.hwnd, moving);
    UpdateHover(s);  // things may have scrolled under a still pointer
    if (!moving && !s.sbDrag && s.hwnd) {   // control centres in screen coordinates, for scripted tests
        std::wstring line;
        for (const Hit& h : s.hits) {
            if (h.r.bottom <= 0 || h.r.top >= s.ch) continue;  // scrolled out of view
            POINT p{(h.r.left + h.r.right) / 2, (h.r.top + h.r.bottom) / 2};
            ClientToScreen(s.hwnd, &p);
            line += L" " + std::to_wstring(int(h.id)) + L"." + std::to_wstring(h.sub) + L"@" + std::to_wstring(p.x) +
                    L"," + std::to_wstring(p.y);
        }
        if (line != s.lastLayout) {
            s.lastLayout = line;
            Log(L"settings controls:%ls", line.c_str());
        }
    }
}

void DropDragCache(Settings& s) {
    if (s.dragContent) DeleteObject(s.dragContent);
    s.dragContent = nullptr;
    s.dragHits.clear();
}

bool ContentMoving(const Settings& s) {
    if (s.themeFade.Running()) return true;
    for (const auto& kv : s.anim) if (kv.second.Running()) return true;
    return false;
}

bool BuildDragCache(Settings& s, HDC dc) {
    if (s.cw <= 0 || s.ch <= 0 || s.contentH <= s.ch || ContentMoving(s)) return false;
    Settings page = s;
    page.hwnd = nullptr;
    page.ch = s.contentH;
    page.scroll.Jump(0);
    page.lastFrame = page.fromFrame = page.dragContent = nullptr;
    page.dragHits.clear();
    page.anim.clear();
    page.segSel.clear();
    page.rowNote.clear();
    page.previousRowNote.clear();
    page.sbDrag = false;
    page.fitted = true;
    HDC scratch = CreateCompatibleDC(dc);
    HBITMAP surface = scratch ? CreateCompatibleBitmap(dc, s.cw, s.contentH) : nullptr;
    if (!surface) { if (scratch) DeleteDC(scratch); return false; }
    HGDIOBJ old = SelectObject(scratch, surface);
    PaintFull(page, scratch);
    SelectObject(scratch, old);
    DeleteObject(surface);
    DeleteDC(scratch);
    if (!page.lastFrame || page.contentH != s.contentH) {
        if (page.lastFrame) DeleteObject(page.lastFrame);
        return false;
    }
    s.dragContent = page.lastFrame;
    s.dragHits = std::move(page.hits);
    return true;
}

void Paint(Settings& s, HDC dc) {
    if (s.hoverDirty || (!s.sbDrag && !s.scroll.Running()) || ContentMoving(s) || (!s.dragContent && !BuildDragCache(s, dc))) {
        if (s.hoverDirty) { DropDragCache(s); s.hoverDirty = false; }
        if (s.dragContent && !s.sbDrag && !s.scroll.Running()) DropDragCache(s);
        if (s.dragContent && ContentMoving(s)) DropDragCache(s);
        PaintFull(s, dc);
        return;
    }
    HDC mem = CreateCompatibleDC(dc), source = CreateCompatibleDC(dc);
    HBITMAP frame = mem ? CreateCompatibleBitmap(dc, s.cw, s.ch) : nullptr;
    if (!mem || !source || !frame) {
        if (frame) DeleteObject(frame);
        if (mem) DeleteDC(mem);
        if (source) DeleteDC(source);
        PaintFull(s, dc);
        return;
    }
    HGDIOBJ oldFrame = SelectObject(mem, frame), oldSource = SelectObject(source, s.dragContent);
    const int offset = int(std::round(s.scroll.Value()));
    BitBlt(mem, 0, 0, s.cw, s.ch, source, 0, offset, SRCCOPY);
    {
        Graphics g(mem);
        DrawScrollbar(s, g, Theme());
    }
    BitBlt(dc, 0, 0, s.cw, s.ch, mem, 0, 0, SRCCOPY);
    SelectObject(source, oldSource);
    SelectObject(mem, oldFrame);
    DeleteDC(source);
    DeleteDC(mem);
    if (s.lastFrame) DeleteObject(s.lastFrame);
    s.lastFrame = frame;
    s.oy = float(-offset);
    s.hits = s.dragHits;
    for (Hit& hit : s.hits) { hit.r.top -= offset; hit.r.bottom -= offset; }
    KeepAnimating(s.hwnd, AnyRunning(s));
    if (!s.sbDrag) UpdateHover(s);
}

bool PickFolder(HWND owner, std::wstring& out) {
    Com<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                reinterpret_cast<void**>(&dlg))))
        return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(Tr(L"选择截图保存位置", L"Choose screenshot folder"));
    Com<IShellItem> start;
    if (SUCCEEDED(SHCreateItemFromParsingName(SaveFolder().c_str(), nullptr, IID_IShellItem,
                                              reinterpret_cast<void**>(&start))))
        dlg->SetFolder(start.p);
    Com<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->Show(owner)) || FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return false;
    out = path;
    CoTaskMemFree(path);
    return true;
}

void SetRecording(Settings& s, bool on) {
    if (s.recording == on) return;
    s.recording = on;
    SuspendCustomHotkey(on);  // so the current shortcut can be pressed again while recording
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// A key pressed while recording.
void OnRecordKey(Settings& s, UINT vk) {
    switch (vk) {
    case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_LWIN: case VK_RWIN:
    case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL: case VK_LMENU: case VK_RMENU:
        InvalidateRect(s.hwnd, nullptr, FALSE);  // show the held modifiers
        return;
    case VK_ESCAPE:
        s.status.clear();
        SetRecording(s, false);
        return;
    case VK_BACK:
    case VK_DELETE: {
        std::wstring err;
        SetRecording(s, false);
        SetCustomHotkey(0, err);
        s.status.clear();
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    }
    UINT mods = 0;
    if (GetKeyState(VK_CONTROL) < 0) mods |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) < 0) mods |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) < 0) mods |= MOD_SHIFT;
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) mods |= MOD_WIN;
    const DWORD combo = DWORD(mods) << 16 | vk;
    if (!mods && !(vk >= VK_F1 && vk <= VK_F24) && vk != VK_SNAPSHOT && vk != VK_PAUSE) {
        s.status = Tr(L"快捷键需要包含 Ctrl、Alt、Shift 或 Win（F1–F24 可以单独使用）", L"Include Ctrl, Alt, Shift or Win (F1–F24 can be used alone)");
    } else if (combo == VK_SNAPSHOT || combo == (DWORD(MOD_WIN | MOD_SHIFT) << 16 | 'S')) {
        s.status = Tr(L"这个组合键由上面的开关控制，请换一个", L"This shortcut is controlled by the switches above. Choose another.");
    } else {
        std::wstring err;
        SetRecording(s, false);
        if (SetCustomHotkey(combo, err)) s.status.clear();
        else s.status = err;
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void Act(Settings& s, const Hit& h) {
    DropDragCache(s);
    switch (h.id) {
    case MODE: SetDefaultMode(h.sub); break;
    case AUTOSAVE: SetAutoSave(!GetAutoSave()); break;
    case BROWSE: {
        std::wstring dir;
        if (PickFolder(s.hwnd, dir)) { SetCustomSaveDir(dir); s.saveFolderValid = false; Log(L"save folder set to %ls", dir.c_str()); }
        break;
    }
    case RESETDIR: SetCustomSaveDir(L""); s.saveFolderValid = false; Log(L"save folder reset"); break;
    case PRTSCN: SetTakePrtScn(!GetTakePrtScn()); ApplyHookSettings(); break;
    case WINSHIFTS: SetTakeWinShiftS(!GetTakeWinShiftS()); ApplyHookSettings(); break;
    case BLOCKWIN: SetBlockWinShots(!GetBlockWinShots()); ApplyHookSettings(); break;
    case HOTKEY: s.status.clear(); SetRecording(s, true); SetFocus(s.hwnd); break;
    case CLEARKEY: { std::wstring err; SetCustomHotkey(0, err); s.status.clear(); break; }
    case THEME: SetThemeSetting(h.sub); ThemeChanged(); break;  // repaints this window too
    case LANGUAGE: SetLanguageSetting(h.sub); LanguageChanged(); break;
    case STARTUP: SetStartup(!GetStartup()); break;
    case TOAST: SetShowToast(!GetShowToast()); Log(L"show notice %d", int(GetShowToast())); break;
    case SELECTIONSIZE: SetShowSelectionSize(!GetShowSelectionSize()); break;
    case HOMEPAGE: OpenUnelevated(kHomepage); break;
    case FORMAT: SetSaveFormat(h.sub); Log(L"save format %d", GetSaveFormat()); break;
    case CHROMA:
        if (NvencAvailable() && (h.sub == 0 || (h.sub == 1 && GetNvencCapabilities().Supports(DisplayBitDepth(), true)))) {
            SetNvenc444(h.sub == 1);
            Log(L"NVENC chroma %ls", GetNvenc444() ? L"4:4:4" : L"4:2:0");
        }
        break;
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* s = reinterpret_cast<Settings*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        s = static_cast<Settings*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        s->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
    }
    if (!s) return DefWindowProcW(hwnd, msg, wp, lp);
    const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    switch (msg) {
    case WM_SIZE:
        DropDragCache(*s);
        s->cw = LOWORD(lp);
        s->ch = HIWORD(lp);
        if (s->scroll.to > MaxScroll(*s)) s->scroll.Jump(MaxScroll(*s));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {  // smallest size that still fits the widest row
        RECT r{0, 0, int(D(*s, 560)), int(D(*s, 320))};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, UINT(s->dpi * 96));
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
        return 0;
    }
    case WM_MOUSEWHEEL: {
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        const float step = lines == WHEEL_PAGESCROLL ? float(s->ch) * 0.9f : D(*s, 32) * float(lines);
        ScrollTo(*s, s->scroll.to - GET_WHEEL_DELTA_WPARAM(wp) / float(WHEEL_DELTA) * step);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        Paint(*s, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_FIT: {  // once, when first shown: as tall as the content, at most 90% of the screen
        s->fitted = true;
        RECT wr, cr;
        GetWindowRect(hwnd, &wr);
        GetClientRect(hwnd, &cr);
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        const int frame = (wr.bottom - wr.top) - cr.bottom;
        const int h = std::min<int>(s->contentH + frame, (mi.rcWork.bottom - mi.rcWork.top) * 9 / 10);
        const int top = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
        if (h != wr.bottom - wr.top) SetWindowPos(hwnd, nullptr, wr.left, top, wr.right - wr.left, h, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_APP_LANGUAGECHANGED:
        DropDragCache(*s);
        s->textWidths.clear();
        s->rowNote.clear();
        s->previousRowNote.clear();
        s->status.clear();
        SetWindowTextW(hwnd, Tr(L"设置 - Screenshot", L"Settings - Screenshot"));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_APP_THEMECHANGED:
        DropDragCache(*s);
        // The last frame (old colours) fades out over the newly coloured window.
        if (s->fromFrame) DeleteObject(s->fromFrame);
        s->fromFrame = s->lastFrame;
        s->lastFrame = nullptr;
        s->themeFade.Jump(1);
        s->themeFade.Go(0, 240);
        ApplyWindowTheme(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        DropDragCache(*s);
        const float nd = HIWORD(wp) / 96.f;
        s->scroll.Jump(s->scroll.to * nd / s->dpi);  // keep the same place in the content
        s->dpi = nd;
        s->textWidths.clear();
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (s->sbDrag) {  // dragging the scrollbar thumb
            const float pad = D(*s, 3), track = s->ch - 2 * pad, th = float(s->thumb.bottom - s->thumb.top);
            const float frac = std::clamp((pt.y - s->sbGrab - pad) / std::max(1.f, track - th), 0.f, 1.f);
            ScrollTo(*s, frac * MaxScroll(*s), false);
            return 0;
        }
        const RECT strip = ScrollStrip(*s);
        const bool nearBar = MaxScroll(*s) > 0 && PtInRect(&strip, pt);
        s->sbWide.Go(nearBar ? 1.f : 0.f, 120);
        const Hit* h = nearBar ? nullptr : HitAt(*s, pt);
        const int id = h ? int(h->id) : -1, sub = h ? h->sub : -1;
        if (id != s->hotId || sub != s->hotSub || s->sbWide.Running()) {
            if (id != s->hotId || sub != s->hotSub) { DropDragCache(*s); s->hoverDirty = true; }
            s->hotId = id;
            s->hotSub = sub;
            TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&t);
            InvalidateRect(hwnd, nullptr, FALSE);
            KeepAnimating(hwnd, true);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const RECT strip = ScrollStrip(*s);
        if (MaxScroll(*s) > 0 && PtInRect(&strip, pt)) {
            s->sbPress = true;
            SetCapture(hwnd);
            if (pt.y >= s->thumb.top && pt.y < s->thumb.bottom) {
                DropDragCache(*s);
                s->sbDrag = true;
                s->sbGrab = float(pt.y - s->thumb.top);
            } else {  // click on the track: page towards the pointer
                ScrollTo(*s, s->scroll.to + (pt.y < s->thumb.top ? -1.f : 1.f) * s->ch * 0.9f);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        DropDragCache(*s);
        s->sbDrag = s->sbPress = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSELEAVE:
        s->hotId = s->hotSub = -1;
        if (!s->sbDrag) s->sbWide.Go(0, 160);
        InvalidateRect(hwnd, nullptr, FALSE);
        KeepAnimating(hwnd, true);
        return 0;
    case WM_APP_ANIMFRAME:
        SendMessageW(hwnd, WM_TIMER, kAnimTimer, 0);
        if (IsWindow(hwnd)) UpdateWindow(hwnd);
        AnimationFrameDelivered(hwnd);
        return 0;
    case WM_TIMER:
        if (wp == kAnimTimer) {
            InvalidateRect(hwnd, nullptr, FALSE);  // Paint stops the clock once everything settles.
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            const RECT strip = ScrollStrip(*s);
            const bool onBar = MaxScroll(*s) > 0 && PtInRect(&strip, p);
            SetCursor(LoadCursorW(nullptr, !onBar && HitAt(*s, p) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONUP: {
        if (s->sbPress) {  // end of a scrollbar press: no control click
            DropDragCache(*s);
            s->sbDrag = s->sbPress = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        const Hit* h = HitAt(*s, pt);
        if (s->recording && !(h && h->id == HOTKEY)) { s->status.clear(); SetRecording(*s, false); }
        if (h) { const Hit copy = *h; Act(*s, copy); }
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (s->recording) { OnRecordKey(*s, UINT(wp)); return 0; }
        if (wp == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
        if (msg == WM_KEYDOWN) {
            const float line = D(*s, 48), page = s->ch * 0.9f;
            switch (wp) {
            case VK_DOWN: ScrollTo(*s, s->scroll.to + line); return 0;
            case VK_UP: ScrollTo(*s, s->scroll.to - line); return 0;
            case VK_NEXT: case VK_SPACE: ScrollTo(*s, s->scroll.to + page); return 0;
            case VK_PRIOR: ScrollTo(*s, s->scroll.to - page); return 0;
            case VK_HOME: ScrollTo(*s, 0); return 0;
            case VK_END: ScrollTo(*s, MaxScroll(*s)); return 0;
            }
        }
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (s->recording) { InvalidateRect(hwnd, nullptr, FALSE); return 0; }  // also keeps Alt from opening the menu
        break;
    case WM_SYSCHAR:
        if (s->recording) return 0;
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && s->recording) SetRecording(*s, false);
        break;
    case WM_NCDESTROY:
        KeepAnimating(hwnd, false);
        DropDragCache(*s);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (s->recording) SuspendCustomHotkey(false);
        if (g_set == s) g_set = nullptr;
        if (s->lastFrame) DeleteObject(s->lastFrame);
        if (s->fromFrame) DeleteObject(s->fromFrame);
        if (!s->creating) delete s;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}  // namespace

void RefreshSettingsWindow() {
    if (g_set) { DropDragCache(*g_set); g_set->saveFolderValid = false; InvalidateRect(g_set->hwnd, nullptr, FALSE); }
}

void OpenSettings() {
    if (g_set) {
        ShowWindow(g_set->hwnd, SW_RESTORE);
        SetForegroundWindow(g_set->hwnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof wc};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hIcon = wc.hIconSm = AppIcon();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0;
        if (!registered) return;
    }
    auto* s = new Settings;
    POINT cur;
    GetCursorPos(&cur);
    HMONITOR hm = MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(hm, &mi);
    s->dpi = MonitorScale(hm);
    // Resizable; the first paint sets the height from the content (WM_FIT).
    RECT r{0, 0, int(D(*s, 700)), int(D(*s, 690))};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, UINT(s->dpi * 96));
    const RECT& wa = mi.rcWork;
    const int w = r.right - r.left, h = std::min<int>(r.bottom - r.top, wa.bottom - wa.top);
    HWND hwnd = CreateWindowExW(0, kClass, Tr(L"设置 - Screenshot", L"Settings - Screenshot"), WS_OVERLAPPEDWINDOW,
                                wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h,
                                nullptr, nullptr, GetModuleHandleW(nullptr), s);
    if (!hwnd) { delete s; return; }
    s->creating = false;
    g_set = s;
    ApplyWindowTheme(hwnd);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
}
