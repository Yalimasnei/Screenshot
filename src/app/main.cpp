// SPDX-License-Identifier: GPL-3.0-only
// Screenshot: a Snipping Tool style screenshot tool with HDR -> sRGB SDR
// conversion applied to the whole desktop instead of one window.
#include "app/application.h"
#include "ui/ui_common.h"
#include <knownfolders.h>
#include <shellapi.h>
#include <cstdio>
#include <memory>

namespace {
constexpr UINT kTrayId = 1;
enum { ID_CAPTURE = 100, ID_AUTOSAVE, ID_STARTUP, ID_OPENFOLDER, ID_EXIT, ID_THEME_SYSTEM, ID_THEME_LIGHT, ID_THEME_DARK,
       ID_SETTINGS };
constexpr int kHotkeyId = 1;
constexpr UINT_PTR kTrayCaptureTimer = 1;

HWND g_wnd = nullptr;
HICON g_icon = nullptr;
UINT g_taskbarCreated = 0;
bool g_busy = false;
bool g_exitPromptActive = false, g_exiting = false;
Desktop g_desk;

HICON MakeIcon() {
    const int s = GetSystemMetrics(SM_CXSMICON);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = s;
    bi.bmiHeader.biHeight = -s;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color) return LoadIconW(nullptr, IDI_APPLICATION);
    auto* px = static_cast<uint32_t*>(bits);
    const int m = s / 8, t = s >= 24 ? 2 : 1;  // margin, line thickness
    for (int y = 0; y < s; ++y)
        for (int x = 0; x < s; ++x) {
            const bool inside = x >= m && x < s - m && y >= m && y < s - m;
            const bool edge = inside && (x < m + t || x >= s - m - t || y < m + t || y >= s - m - t);
            const bool dash = ((x + y) / (s / 6 + 1)) % 2 == 0;
            px[y * s + x] = edge && dash ? 0xFF2D9CFFu : inside && !edge ? 0x30FFFFFFu : 0;
        }
    HBITMAP mask = CreateBitmap(s, s, 1, 1, nullptr);
    ICONINFO ii = {TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

void Tray(DWORD op) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof nid;
    nid.hWnd = g_wnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_icon;
    lstrcpynW(nid.szTip, L"Screenshot", ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(op, &nid);
}

void Notify(const wchar_t* text) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof nid;
    nid.hWnd = g_wnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_WARNING;
    lstrcpynW(nid.szInfoTitle, L"Screenshot", ARRAYSIZE(nid.szInfoTitle));
    lstrcpynW(nid.szInfo, text, ARRAYSIZE(nid.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

struct OcrJob {
    Image img;
    HWND reply;
    uint64_t operation;
};
struct OcrResult {
    HRESULT hr = E_FAIL;
    std::wstring text;
    uint64_t operation = 0;
};

DWORD WINAPI OcrThread(void* p) {
    std::unique_ptr<OcrJob> job(static_cast<OcrJob*>(p));
    auto result = std::make_unique<OcrResult>();
    result->operation = job->operation;
    result->hr = RecognizeText(job->img, result->text);
    if (PostMessageW(job->reply, WM_APP_OCRDONE, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
    return 0;
}

void OnOcrDone(OcrResult* value) {
    std::unique_ptr<OcrResult> result(value);
    if (!result || g_exiting || !ClipboardOperationCurrent(result->operation)) return;
    const HRESULT hr = result->hr;
    const std::wstring& text = result->text;
    if (FAILED(hr)) {
        Log(L"OCR failed 0x%08X", unsigned(hr));
        ShowToast({Tr(L"无法提取文字", L"Could not extract text"), hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)
                                        ? Tr(L"系统没有安装 OCR 语言包（设置 > 时间和语言 > 语言）", L"Install an OCR language pack in Windows Settings > Time & language > Language")
                                        : Tr(L"Windows 文字识别出错", L"Windows text recognition failed")});
    } else if (text.empty()) {
        ShowToast({Tr(L"没有识别到文字", L"No text found"), Tr(L"请框选包含文字的区域", L"Select an area containing text")});
    } else {
        const bool ok = CopyTextToClipboard(g_wnd, text);
        Log(L"OCR: %zu chars, clipboard=%d", text.size(), ok);
        std::wstring preview = text;
        for (wchar_t& c : preview) if (c == L'\r' || c == L'\n') c = L' ';
        if (!ok || GetShowToast()) ShowToast({ok ? Tr(L"文字已复制到剪贴板", L"Text copied to clipboard") : Tr(L"无法写入剪贴板", L"Could not copy to clipboard"), preview});
    }
}

void OnSnip(const SnipResult& r) {
    const uint64_t operation = r.kind == SnipResult::Cancel ? 0 : BeginClipboardOperation();
    if (r.mode == SnipMode::Rect || r.mode == SnipMode::Window || r.mode == SnipMode::Full || r.mode == SnipMode::Free)
        SetLastMode(int(r.mode));
    if (r.kind == SnipResult::Color) {
        const uint32_t c = r.color;
        wchar_t hex[16], body[64];
        swprintf(hex, 16, L"#%02X%02X%02X", (c >> 16) & 255, (c >> 8) & 255, c & 255);
        swprintf(body, 64, L"RGB(%u, %u, %u)", (c >> 16) & 255, (c >> 8) & 255, c & 255);
        const bool ok = CopyTextToClipboard(g_wnd, hex);
        Log(L"color %ls, clipboard=%d", hex, ok);
        ToastInfo t{std::wstring(ok ? Tr(L"已复制颜色 ", L"Color copied: ") : Tr(L"无法写入剪贴板 ", L"Could not copy to clipboard: ")) + hex, body};
        t.swatch = true;
        t.color = RGB((c >> 16) & 255, (c >> 8) & 255, c & 255);
        if (!ok || GetShowToast()) ShowToast(t);
    } else if (r.kind == SnipResult::Text) {
        auto* job = new OcrJob{Crop(g_desk.img, r.rect), g_wnd, operation};
        if (HANDLE th = CreateThread(nullptr, 0, OcrThread, job, 0, nullptr)) CloseHandle(th);
        else delete job;
    } else if (r.kind == SnipResult::Picture) {
        Image snip = Crop(g_desk.img, r.rect);
        if (!r.poly.empty()) {
            std::vector<POINT> poly = r.poly;
            for (POINT& p : poly) { p.x -= r.rect.left; p.y -= r.rect.top; }
            MaskPolygon(snip, poly);
        }
        std::vector<BYTE> png;
        if (snip.w > 0 && EncodePng(snip, png)) {
            const bool clip = CopyToClipboard(g_wnd, snip, png);  // the clipboard always gets PNG
            std::wstring saved, note;
            if (GetAutoSave()) {
                const DWORD t0 = GetTickCount();
                const Encoded enc = EncodeForSave(snip, SaveFormat(GetSaveFormat()), png, GetNvenc444());
                saved = SaveToScreenshots(enc.data, enc.ext);
                Log(L"saved as %ls, %zu bytes (PNG %zu), %lu ms", enc.ext, enc.data.size(), png.size(), GetTickCount() - t0);
                if (saved.empty()) note = Tr(L"保存失败 · 单击此处进行标注和编辑", L"Save failed · Click to edit");
                else if (!enc.fallback.empty()) note = Tr(L"已存为 PNG（", L"Saved as PNG (") + enc.fallback + Tr(L"）· 单击编辑", L") · Click to edit");
                else note = enc.used == SaveFormat::Png ? Tr(L"已自动保存 · 单击此处进行标注和编辑", L"Saved automatically · Click to edit")
                                                        : Tr(L"已存为 HEIC · 单击此处进行标注和编辑", L"Saved as HEIC · Click to edit");
            } else {
                note = Tr(L"单击此处进行标注和编辑", L"Click to annotate and edit");
            }
            Log(L"snip %dx%d at (%ld,%ld)%ls: clipboard=%d saved=%ls", snip.w, snip.h, r.rect.left, r.rect.top,
                r.poly.empty() ? L"" : L" free-form", clip, saved.empty() ? L"-" : saved.c_str());
            ToastInfo t{clip ? Tr(L"截图已复制到剪贴板", L"Screenshot copied to clipboard") : Tr(L"无法写入剪贴板", L"Could not copy to clipboard"), note};
            t.thumb = &snip;
            // With notices off, only problems are shown (clipboard or saving failed).
            const bool saveFailed = GetAutoSave() && saved.empty();
            if (GetShowToast() || !clip || saveFailed) ShowToast(t);
        } else {
            Log(L"PNG encode failed");
            Notify(Tr(L"截图编码失败", L"Screenshot encoding failed"));
        }
    }
    g_desk = Desktop{};
    g_busy = false;
}

void StartCapture() {
    if (g_busy || g_exitPromptActive || g_exiting) return;
    g_busy = true;
    const DWORD t0 = GetTickCount();
    std::vector<RECT> windows = VisibleWindowRects();
    Desktop d;
    if (!CaptureDesktop(d)) {
        Log(L"CaptureDesktop failed");
        Notify(Tr(L"截屏失败，详见 %TEMP%\\Screenshot.log", L"Capture failed. See %TEMP%\\Screenshot.log"));
        g_busy = false;
        return;
    }
    Log(L"captured %dx%d, %zu monitor(s), %d HDR, %lu ms", d.img.w, d.img.h, d.monitors.size(), d.hdrMonitors,
        GetTickCount() - t0);
    g_desk = std::move(d);
    const int def = GetDefaultMode();  // 0 = last used, 1..4 = Rect..Free
    const SnipMode mode = def ? SnipMode(def - 1) : SnipMode(GetLastMode());
    if (!ShowOverlay(g_desk.img, g_desk.exact, g_desk.bounds, windows, g_desk.monitors, mode, OnSnip)) {
        Log(L"ShowOverlay failed");
        g_desk = Desktop{};
        g_busy = false;
    }
}

void OpenFolder() {
    std::wstring dir = SaveFolder();
    if (!dir.empty()) OpenUnelevated(dir.c_str());
}

bool RequestExit() {
    if (g_exitPromptActive || g_exiting) return false;
    g_exitPromptActive = true;
    const bool accepted = CloseAllEditors();
    g_exitPromptActive = false;
    if (!accepted) return false;
    g_exiting = true;
    BeginClipboardOperation();
    DestroyWindow(g_wnd);
    return true;
}

void ShowMenu() {
    HMENU theme = CreatePopupMenu();
    AppendMenuW(theme, MF_STRING, ID_THEME_SYSTEM, Tr(L"跟随系统(&S)", L"Use system setting(&S)"));
    AppendMenuW(theme, MF_STRING, ID_THEME_LIGHT, Tr(L"浅色(&L)", L"Light(&L)"));
    AppendMenuW(theme, MF_STRING, ID_THEME_DARK, Tr(L"深色(&D)", L"Dark(&D)"));
    CheckMenuRadioItem(theme, ID_THEME_SYSTEM, ID_THEME_DARK, ID_THEME_SYSTEM + GetThemeSetting(), MF_BYCOMMAND);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_CAPTURE, Tr(L"截图", L"Capture"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (GetAutoSave() ? MF_CHECKED : 0), ID_AUTOSAVE, Tr(L"自动保存截图", L"Save screenshots automatically"));
    AppendMenuW(m, MF_STRING | (GetStartup() ? MF_CHECKED : 0), ID_STARTUP, Tr(L"开机启动", L"Start with Windows"));
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(theme), Tr(L"主题(&T)", L"Theme(&T)"));
    AppendMenuW(m, MF_STRING, ID_OPENFOLDER, Tr(L"打开截图保存文件夹", L"Open screenshot folder"));
    AppendMenuW(m, MF_STRING, ID_SETTINGS, Tr(L"设置(&O)…", L"Settings(&O)…"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, Tr(L"退出", L"Exit"));
    SetMenuDefaultItem(m, ID_CAPTURE, FALSE);
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_wnd);
    const UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, g_wnd, nullptr);
    DestroyMenu(m);  // also destroys the theme submenu
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    switch (cmd) {
    case ID_CAPTURE: SetTimer(g_wnd, kTrayCaptureTimer, 250, nullptr); break;  // let the menu fade first
    case ID_AUTOSAVE: SetAutoSave(!GetAutoSave()); break;
    case ID_STARTUP: SetStartup(!GetStartup()); break;
    case ID_OPENFOLDER: OpenFolder(); break;
    case ID_SETTINGS: OpenSettings(); break;
    case ID_THEME_SYSTEM:
    case ID_THEME_LIGHT:
    case ID_THEME_DARK: SetThemeSetting(int(cmd - ID_THEME_SYSTEM)); ThemeChanged(); break;
    case ID_EXIT: RequestExit(); break;
    }
    RefreshSettingsWindow();
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && g_taskbarCreated) {  // Explorer restarted
        Tray(NIM_ADD);
        return 0;
    }
    switch (msg) {
    case WM_APP_CAPTURE:
        StartCapture();
        return 0;
    case WM_HOTKEY:
        if (wp == kHotkeyId) StartCapture();
        return 0;
    case WM_APP_OCRDONE:
        OnOcrDone(reinterpret_cast<OcrResult*>(lp));
        return 0;
    case WM_SETTINGCHANGE:  // Windows light/dark switched
        if (lp && !lstrcmpW(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") && GetThemeSetting() == 0)
            ThemeChanged();
        if (GetLanguageSetting() == 0) LanguageChanged();
        return 0;
    case WM_APP_HOOKFAIL:
        Notify(Tr(L"无法注册 PrtScn / Win+Shift+S 快捷键，只能从托盘菜单截图", L"Could not register PrtScn / Win+Shift+S. Use the tray menu to capture."));
        return 0;
    case WM_TIMER:
        if (wp == kTrayCaptureTimer) { KillTimer(h, wp); StartCapture(); }
        return 0;
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) ShowMenu();
        else if (LOWORD(lp) == WM_LBUTTONUP) SetTimer(h, kTrayCaptureTimer, 150, nullptr);
        return 0;
    case WM_QUERYENDSESSION:
        return CanEndEditorSession() ? TRUE : FALSE;
    case WM_ENDSESSION:
        if (wp) DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_exiting = true;
        BeginClipboardOperation();
        Tray(NIM_DELETE);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// `Screenshot.exe --selftest out.png`: captures the whole desktop to a file.
int SelfTest(const wchar_t* path) {
    Desktop d;
    std::vector<BYTE> png;
    const DWORD t0 = GetTickCount();
    if (!CaptureDesktop(d) || !EncodePng(d.img, png) || !WriteBytes(path, png)) return 1;
    Log(L"selftest %dx%d, %d HDR monitor(s), %lu ms -> %ls", d.img.w, d.img.h, d.hdrMonitors, GetTickCount() - t0, path);
    return 0;
}

// `Screenshot.exe --encodetest in.png <0|1|2>`: runs the save encoder on a file,
// Exit: 0 chosen format saved, 1 fallback saved, 2 invalid input, 3 output failed.
int EncodeTest(const wchar_t* path, int fmt) {
    if (fmt < 0 || fmt > 2) return 2;
    Gdiplus::Bitmap bmp(path);
    Image img;
    std::vector<BYTE> png;
    if (bmp.GetLastStatus() != Gdiplus::Ok || !FromGdiplus(bmp, img) || !EncodePng(img, png)) return 2;
    const DWORD t0 = GetTickCount();
    std::vector<BYTE> rejected;
    g_rejectedHeic = &rejected;
    const Encoded e = EncodeForSave(img, SaveFormat(fmt), png, GetNvenc444());
    g_rejectedHeic = nullptr;
    const DWORD ms = GetTickCount() - t0;
    if (!WriteBytes(std::wstring(path) + L".out" + e.ext, e.data)) {
        Log(L"encodetest: output save failed");
        return 3;
    }
    if (!rejected.empty()) WriteBytes(std::wstring(path) + L".rejected.heic", rejected);
    Log(L"encodetest %dx%d fmt %d -> %ls %zu bytes (PNG %zu), %lu ms, fallback [%ls]", img.w, img.h, fmt, e.ext,
        e.data.size(), png.size(), ms, e.fallback.c_str());
    return int(e.used) == fmt ? 0 : 1;
}
}  // namespace

HICON AppIcon() { return g_icon; }

// Re-reads the theme and repaints every open window.
void ThemeChanged() {
    RefreshTheme();
    EnumThreadWindows(GetCurrentThreadId(), [](HWND h, LPARAM) -> BOOL {
        PostMessageW(h, WM_APP_THEMECHANGED, 0, 0);
        return TRUE;
    }, 0);
    Log(L"theme setting %d, dark=%d", GetThemeSetting(), int(Theme().dark));
}

void LanguageChanged() {
    RefreshLanguage();
    EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM) -> BOOL {
        PostMessageW(hwnd, WM_APP_LANGUAGECHANGED, 0, 0);
        return TRUE;
    }, 0);
}

void ApplyHookSettings() {
    SetHookOptions(GetTakePrtScn(), GetTakeWinShiftS(), GetBlockWinShots());
    Log(L"hook: PrtScn=%d Win+Shift+S=%d block Windows=%d", int(GetTakePrtScn()), int(GetTakeWinShiftS()), int(GetBlockWinShots()));
}

static bool RegisterCombo(DWORD combo) {
    return RegisterHotKey(g_wnd, kHotkeyId, (combo >> 16) | MOD_NOREPEAT, combo & 0xFFFF) != 0;
}

void SuspendCustomHotkey(bool suspend) {
    UnregisterHotKey(g_wnd, kHotkeyId);
    if (!suspend && GetCustomHotkey()) RegisterCombo(GetCustomHotkey());
}

struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComApartment() {
        if (SUCCEEDED(result)) CoUninitialize();
    }
    explicit operator bool() const { return SUCCEEDED(result); }
};

bool SetCustomHotkey(DWORD combo, std::wstring& error) {
    UnregisterHotKey(g_wnd, kHotkeyId);
    if (combo && !RegisterCombo(combo)) {
        error = Tr(L"这个组合键已被其他程序占用，请换一个", L"This shortcut is in use by another app. Choose a different one.");
        Log(L"hotkey 0x%08lX taken (error %lu)", combo, GetLastError());
        if (GetCustomHotkey()) RegisterCombo(GetCustomHotkey());  // keep the old one
        return false;
    }
    SaveCustomHotkey(combo);
    Log(L"custom hotkey 0x%08lX", combo);
    return true;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ComApartment com;
    if (!com) return 1;
    GdiplusSession gdiplus;
    RefreshLanguage();
    RefreshTheme();

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 3 && !lstrcmpW(argv[1], L"--selftest")) {
        const int rc = SelfTest(argv[2]);
        LocalFree(argv);
        return rc;
    }
    if (argv && argc >= 4 && !lstrcmpW(argv[1], L"--encodetest")) {
        const int rc = EncodeTest(argv[2], _wtoi(argv[3]));
        LocalFree(argv);
        return rc;
    }
    LocalFree(argv);

    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\Screenshot.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"ScreenshotMain";
    RegisterClassExW(&wc);
    // A hidden top-level window (not message-only) so it gets TaskbarCreated.
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Screenshot", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                            inst, nullptr);
    if (!g_wnd) return 1;
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    // Running elevated, Windows drops messages from Explorer (medium integrity)
    // unless they are let through: tray re-creation, tray clicks, theme changes.
    for (UINT m : {g_taskbarCreated, WM_APP_TRAY, UINT(WM_SETTINGCHANGE)})
        ChangeWindowMessageFilterEx(g_wnd, m, MSGFLT_ALLOW, nullptr);
    g_icon = MakeIcon();
    Tray(NIM_ADD);
    RefreshStartupTask();
    ApplyHookSettings();
    if (!StartKeyHook(g_wnd)) PostMessageW(g_wnd, WM_APP_HOOKFAIL, 0, 0);
    if (GetCustomHotkey() && !RegisterCombo(GetCustomHotkey())) Notify(Tr(L"自定义快捷键已被其他程序占用，请在设置里换一个", L"The custom shortcut is in use by another app. Change it in Settings."));
    Log(L"started");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    while (PeekMessageW(&msg, nullptr, WM_APP_OCRDONE, WM_APP_OCRDONE, PM_REMOVE))
        delete reinterpret_cast<OcrResult*>(msg.lParam);
    StopKeyHook();
    DestroyIcon(g_icon);
    if (single) CloseHandle(single);
    return 0;
}
