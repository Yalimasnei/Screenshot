// SPDX-License-Identifier: GPL-3.0-only
// Logging, settings (Screenshot.ini next to the exe), autostart (a scheduled
// task, since the exe runs as administrator) and the hotkey hook. Nothing is
// written to the registry.
#include "app/application.h"
#include "platform/windows_paths.h"
#include <knownfolders.h>
#include <sddl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <taskschd.h>
#include <exdisp.h>
#include <shldisp.h>
#include <shlguid.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <map>

void Log(const wchar_t* fmt, ...) {
    wchar_t msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[1100];
    const int n = swprintf(line, 1100, L"%04u-%02u-%02u %02u:%02u:%02u.%03u %ls\r\n", t.wYear, t.wMonth, t.wDay,
                           t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
    if (n <= 0) return;
    char utf8[3300];
    const int len = WideCharToMultiByte(CP_UTF8, 0, line, n, utf8, sizeof utf8, nullptr, nullptr);
    if (len <= 0) return;
    const std::wstring dir = TempDirectory();
    if (dir.empty()) return;
    const std::wstring path = ExtendedFilePath(dir + L"Screenshot.log");
    if (path.empty()) return;
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(f, utf8, DWORD(len), &w, nullptr);
    CloseHandle(f);
}

namespace {
// ---- Screenshot.ini: "key=value" lines, UTF-8, in the exe's folder ----
std::map<std::wstring, std::wstring> g_ini;
bool g_iniLoaded = false;
std::atomic<int> g_english{-1};
std::atomic<uint64_t> g_clipboardOperation{0};

std::wstring ExeDir() {
    const std::wstring p = ModulePath();
    return p.substr(0, p.find_last_of(L"\\/"));
}
std::wstring IniPath() { return ExeDir() + L"\\Screenshot.ini"; }

void LoadIni() {
    if (g_iniLoaded) return;
    g_iniLoaded = true;
    HANDLE f = CreateFileW(IniPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    std::string bytes(size_t(std::min<LONGLONG>(size.QuadPart, 1 << 20)), '\0');
    DWORD got = 0;
    ReadFile(f, bytes.data(), DWORD(bytes.size()), &got, nullptr);
    CloseHandle(f);
    bytes.resize(got);
    if (bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) bytes.erase(0, 3);
    std::wstring text(size_t(MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), text.data(), int(text.size()));
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty() || line[0] == L';' || line[0] == L'#' || line[0] == L'[') continue;
        const size_t eq = line.find(L'=');
        if (eq != std::wstring::npos) g_ini[line.substr(0, eq)] = line.substr(eq + 1);
    }
}

void SaveIni() {
    std::wstring text = L"[Screenshot]\r\n";
    for (const auto& kv : g_ini) text += kv.first + L"=" + kv.second + L"\r\n";
    std::string bytes = "\xEF\xBB\xBF";  // BOM, so Notepad shows Chinese paths correctly
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr);
    const size_t at = bytes.size();
    bytes.resize(at + size_t(n));
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), bytes.data() + at, n, nullptr, nullptr);
    const std::wstring path = IniPath(), tmp = path + L".tmp";  // write aside, then swap in
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { Log(L"cannot write %ls (error %lu)", tmp.c_str(), GetLastError()); return; }
    DWORD w = 0;
    const BOOL ok = WriteFile(f, bytes.data(), DWORD(bytes.size()), &w, nullptr) && w == bytes.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        Log(L"cannot save %ls (error %lu)", path.c_str(), GetLastError());
        DeleteFileW(tmp.c_str());
    }
}

DWORD GetDword(const wchar_t* name, DWORD def) {
    LoadIni();
    auto it = g_ini.find(name);
    if (it == g_ini.end() || it->second.empty()) return def;
    return DWORD(wcstoul(it->second.c_str(), nullptr, 0));
}
void SetDword(const wchar_t* name, DWORD v) {
    LoadIni();
    g_ini[name] = std::to_wstring(v);
    SaveIni();
}
std::wstring GetString(const wchar_t* name) {
    LoadIni();
    auto it = g_ini.find(name);
    return it == g_ini.end() ? std::wstring{} : it->second;
}
void SetString(const wchar_t* name, const std::wstring& v) {
    LoadIni();
    if (v.empty()) g_ini.erase(name);
    else g_ini[name] = v;
    SaveIni();
}

// Start with Windows = a scheduled task. The exe requires administrator, and
// Windows silently skips such programs in the Startup folder; a task set to
// "run with highest privileges" starts it elevated at logon with no UAC prompt.
const wchar_t kTaskName[] = L"Screenshot 开机启动";
int g_startup = -1;  // cached task state; -1 = not looked up yet
ULONGLONG g_startupChecked = 0;

template <class F> HRESULT WithTaskFolder(F&& f) {
    Com<ITaskService> svc;
    Com<ITaskFolder> root;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService,
                                  reinterpret_cast<void**>(&svc));
    if (SUCCEEDED(hr)) hr = svc->Connect(VARIANT{}, VARIANT{}, VARIANT{}, VARIANT{});
    BSTR path = SysAllocString(L"\\");
    if (SUCCEEDED(hr)) hr = svc->GetFolder(path, &root);
    SysFreeString(path);
    return SUCCEEDED(hr) ? f(root.p) : hr;
}

std::wstring XmlEscape(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        switch (c) {
        case L'&': out += L"&amp;"; break;
        case L'<': out += L"&lt;"; break;
        case L'>': out += L"&gt;"; break;
        case L'"': out += L"&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

std::wstring UserSid() {
    std::wstring sid;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return sid;
    BYTE buf[256];
    DWORD len = 0;
    wchar_t* str = nullptr;
    if (GetTokenInformation(token, TokenUser, buf, sizeof buf, &len) &&
        ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf)->User.Sid, &str)) {
        sid = str;
        LocalFree(str);
    }
    CloseHandle(token);
    return sid;
}

HRESULT ReadTask(ITaskFolder* root, const std::wstring& name, Com<IRegisteredTask>& task) {
    BSTR value = SysAllocString(name.c_str());
    const HRESULT hr = root->GetTask(value, &task);
    SysFreeString(value);
    return hr;
}

// Task Scheduler may return an account name even when registered with a SID.
bool AccountMatchesSid(const wchar_t* account, const std::wstring& sid) {
    if (!account || !*account || sid.empty()) return false;
    PSID expected = nullptr, actual = nullptr;
    if (!ConvertStringSidToSidW(sid.c_str(), &expected)) return false;
    bool matches = false;
    if (ConvertStringSidToSidW(account, &actual)) {
        matches = EqualSid(expected, actual) != FALSE;
        LocalFree(actual);
    } else {
        DWORD bytes = 0, domainChars = 0;
        SID_NAME_USE type;
        LookupAccountNameW(nullptr, account, nullptr, &bytes, nullptr, &domainChars, &type);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytes) {
            std::vector<BYTE> resolved(bytes);
            std::vector<wchar_t> domain(domainChars ? domainChars : 1);
            if (LookupAccountNameW(nullptr, account, resolved.data(), &bytes, domain.data(), &domainChars, &type))
                matches = EqualSid(expected, resolved.data()) != FALSE;
        }
    }
    LocalFree(expected);
    return matches;
}

bool TaskOwnedBy(IRegisteredTask* task, const std::wstring& sid) {
    if (!task) return false;
    Com<ITaskDefinition> definition;
    Com<IPrincipal> principal;
    BSTR user = nullptr;
    const bool ok = SUCCEEDED(task->get_Definition(&definition)) && definition.p &&
                    SUCCEEDED(definition->get_Principal(&principal)) && principal.p &&
                    SUCCEEDED(principal->get_UserId(&user)) && AccountMatchesSid(user, sid);
    SysFreeString(user);
    return ok;
}

bool TaskMissing(HRESULT hr) {
    return hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
}

// Existing installations keep their legacy task if it belongs to this user.
// New users receive separate names, so one account cannot replace another's task.
std::wstring StartupTaskName(ITaskFolder* root, const std::wstring& sid) {
    const std::wstring name = std::wstring(kTaskName) + L"-" + sid;
    Com<IRegisteredTask> current;
    const HRESULT hr = ReadTask(root, name, current);
    if (SUCCEEDED(hr)) return TaskOwnedBy(current.p, sid) ? name : L"";
    if (!TaskMissing(hr)) return {};
    Com<IRegisteredTask> legacy;
    if (SUCCEEDED(ReadTask(root, kTaskName, legacy)) && TaskOwnedBy(legacy.p, sid)) return kTaskName;
    return name;
}

HRESULT DeleteStartupTasks(ITaskFolder* root, const std::wstring& sid, const std::wstring& keep = {}) {
    for (const std::wstring& name : {std::wstring(kTaskName) + L"-" + sid, std::wstring(kTaskName)}) {
        if (name == keep) continue;
        Com<IRegisteredTask> task;
        const HRESULT read = ReadTask(root, name, task);
        if (TaskMissing(read)) continue;
        if (FAILED(read)) return read;
        if (!TaskOwnedBy(task.p, sid)) continue;
        BSTR value = SysAllocString(name.c_str());
        const HRESULT result = root->DeleteTask(value, 0);
        SysFreeString(value);
        if (FAILED(result)) return result;
    }
    return S_OK;
}
}  // namespace

uint64_t BeginClipboardOperation() { return ++g_clipboardOperation; }
bool ClipboardOperationCurrent(uint64_t operation) { return operation && operation == g_clipboardOperation.load(); }

bool GetAutoSave() { return GetDword(L"AutoSave", 1) != 0; }
void SetAutoSave(bool on) { SetDword(L"AutoSave", on); }
int GetLastMode() {
    const DWORD m = GetDword(L"LastMode", 0);
    return m <= 3 ? int(m) : 0;  // Rect, Window, Full or Free
}
void SetLastMode(int mode) { SetDword(L"LastMode", DWORD(mode)); }
int GetThemeSetting() {
    const DWORD v = GetDword(L"Theme", 0);
    return v <= 2 ? int(v) : 0;
}
void SetThemeSetting(int v) { SetDword(L"Theme", DWORD(v)); }
int GetLanguageSetting() {
    const DWORD value = GetDword(L"Language", 0);
    return value <= 2 ? int(value) : 0;
}
void RefreshLanguage() {
    const int setting = GetLanguageSetting();
    // Prefer Windows' UI language list, which may differ from regional formats.
    wchar_t languages[256]{};
    ULONG count = 0, size = _countof(languages);
    LANGID language = GetUserDefaultUILanguage();
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, languages, &size) && count)
        language = LANGIDFROMLCID(LocaleNameToLCID(languages, 0));
    g_english.store(setting == 2 || (setting == 0 && PRIMARYLANGID(language) != LANG_CHINESE));
}
void SetLanguageSetting(int value) {
    SetDword(L"Language", value >= 0 && value <= 2 ? DWORD(value) : 0);
    RefreshLanguage();
}
bool English() {
    if (g_english.load() < 0) RefreshLanguage();
    return g_english.load() != 0;
}
const wchar_t* Tr(const wchar_t* chinese, const wchar_t* english) { return English() ? english : chinese; }
int GetDefaultMode() {
    const DWORD v = GetDword(L"DefaultMode", 0);
    return v <= 4 ? int(v) : 0;
}
void SetDefaultMode(int v) { SetDword(L"DefaultMode", DWORD(v)); }
bool GetTakePrtScn() { return GetDword(L"HookPrtScn", 1) != 0; }
void SetTakePrtScn(bool on) { SetDword(L"HookPrtScn", on); }
bool GetTakeWinShiftS() { return GetDword(L"HookWinShiftS", 1) != 0; }
void SetTakeWinShiftS(bool on) { SetDword(L"HookWinShiftS", on); }
bool GetBlockWinShots() { return GetDword(L"BlockWinShots", 1) != 0; }
void SetBlockWinShots(bool on) { SetDword(L"BlockWinShots", on); }
DWORD GetCustomHotkey() { return GetDword(L"CustomHotkey", 0); }
void SaveCustomHotkey(DWORD combo) { SetDword(L"CustomHotkey", combo); }
int GetSaveFormat() {
    const DWORD v = GetDword(L"SaveFormat", 0);
    return v <= 2 && (v != 1 || NvencAvailable()) ? int(v) : 0;
}
void SetSaveFormat(int v) {
    if (v >= 0 && v <= 2 && (v != 1 || NvencAvailable())) SetDword(L"SaveFormat", DWORD(v));
}
bool GetNvenc444() { return GetDword(L"Nvenc444", 0) == 1; }
void SetNvenc444(bool on) { SetDword(L"Nvenc444", on); }
bool GetShowToast() { return GetDword(L"ShowToast", 1) != 0; }
void SetShowToast(bool on) { SetDword(L"ShowToast", on); }
bool GetShowSelectionSize() { return GetDword(L"ShowSelectionSize", 0) == 1; }
void SetShowSelectionSize(bool on) { SetDword(L"ShowSelectionSize", on); }

std::wstring GetCustomSaveDir() { return GetString(L"SaveDir"); }
void SetCustomSaveDir(const std::wstring& dir) { SetString(L"SaveDir", dir); }

std::wstring SaveFolder() {
    const std::wstring custom = GetCustomSaveDir();
    if (!custom.empty()) {
        SHCreateDirectoryExW(nullptr, custom.c_str(), nullptr);  // fine if it already exists
        const DWORD a = GetFileAttributesW(custom.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return custom;
        Log(L"save folder %ls unusable, using Screenshots", custom.c_str());
    }
    return KnownFolder(FOLDERID_Screenshots, KF_FLAG_CREATE);
}

// Start with Windows = the scheduled task above.
bool GetStartup() {
    if (g_startup < 0 || GetTickCount64() - g_startupChecked >= 1000) {
        bool found = false;
        const std::wstring sid = UserSid();
        if (sid.empty()) return false;
        const HRESULT hr = WithTaskFolder([&](ITaskFolder* root) {
            Com<IRegisteredTask> task;
            const std::wstring name = StartupTaskName(root, sid);
            VARIANT_BOOL enabled = VARIANT_FALSE;
            found = !name.empty() && SUCCEEDED(ReadTask(root, name, task)) &&
                    TaskOwnedBy(task.p, sid) && SUCCEEDED(task->get_Enabled(&enabled)) && enabled != VARIANT_FALSE;
            return S_OK;
        });
        if (FAILED(hr)) Log(L"task scheduler unavailable 0x%08X", unsigned(hr));
        g_startup = found;
        g_startupChecked = GetTickCount64();
    }
    return g_startup == 1;
}

void SetStartup(bool on) {
    const std::wstring sid = UserSid();
    if (sid.empty()) { Log(L"cannot read the user SID for the start-up task"); g_startup = -1; return; }
    HRESULT hr;
    if (!on) {
        hr = WithTaskFolder([&](ITaskFolder* root) {
            return DeleteStartupTasks(root, sid);
        });
    } else {
        const std::wstring exe = ModulePath();
        if (exe.empty()) { g_startup = -1; return; }
        const std::wstring xml =
            L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
            L"<RegistrationInfo><Description>登录时以管理员身份启动 Screenshot</Description></RegistrationInfo>"
            L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + sid + L"</UserId></LogonTrigger></Triggers>"
            L"<Principals><Principal id=\"Author\"><UserId>" + sid + L"</UserId>"
            L"<LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>"
            L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
            L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
            L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
            L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>4</Priority></Settings>"
            L"<Actions Context=\"Author\"><Exec><Command>" + XmlEscape(exe) + L"</Command>"
            L"<WorkingDirectory>" + XmlEscape(ExeDir()) + L"</WorkingDirectory></Exec></Actions></Task>";
        hr = WithTaskFolder([&](ITaskFolder* root) {
            const std::wstring taskName = StartupTaskName(root, sid);
            if (taskName.empty()) return E_ACCESSDENIED;
            Com<IRegisteredTask> task;
            BSTR name = SysAllocString(taskName.c_str()), text = SysAllocString(xml.c_str());
            HRESULT r = root->RegisterTask(name, text, TASK_CREATE_OR_UPDATE, VARIANT{}, VARIANT{},
                                           TASK_LOGON_INTERACTIVE_TOKEN, VARIANT{}, &task);
            SysFreeString(name);
            SysFreeString(text);
            if (SUCCEEDED(r)) {
                const HRESULT cleanup = DeleteStartupTasks(root, sid, taskName);
                if (FAILED(cleanup)) Log(L"cannot remove duplicate start-up tasks: 0x%08X", unsigned(cleanup));
            }
            return r;
        });
    }
    if (FAILED(hr)) Log(L"cannot %ls the start-up task: 0x%08X", on ? L"create" : L"delete", unsigned(hr));
    g_startup = SUCCEEDED(hr) ? int(on) : -1;  // on failure, look the real state up next time
    g_startupChecked = GetTickCount64();
}

void RefreshStartupTask() {
    const std::wstring sid = UserSid();
    if (sid.empty()) return;
    const HRESULT hr = WithTaskFolder([&](ITaskFolder* root) {
        const std::wstring keep = StartupTaskName(root, sid);
        if (keep.empty()) return S_OK;
        Com<IRegisteredTask> task;
        const HRESULT read = ReadTask(root, keep, task);
        if (TaskMissing(read)) return S_OK;
        if (FAILED(read)) return read;
        if (!TaskOwnedBy(task.p, sid)) return S_OK;
        return DeleteStartupTasks(root, sid, keep);
    });
    if (FAILED(hr)) Log(L"cannot remove duplicate start-up tasks: 0x%08X", unsigned(hr));
    g_startup = -1;
    if (GetStartup()) SetStartup(true);  // update the enabled task's executable path
}

// Explorer's desktop runs at normal rights; asking it to do the ShellExecute
// keeps the folder window or browser from starting as administrator.
void OpenUnelevated(const wchar_t* target) {
    HRESULT hr;
    {
        Com<IShellWindows> windows;
        Com<IDispatch> desktopDisp, folderDisp, appDisp;
        Com<IServiceProvider> sp;
        Com<IShellBrowser> browser;
        Com<IShellView> view;
        Com<IShellFolderViewDual> folderView;
        Com<IShellDispatch2> shell;
        VARIANT empty{};
        long hwnd = 0;
        hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_IShellWindows,
                              reinterpret_cast<void**>(&windows));
        if (SUCCEEDED(hr)) hr = windows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &desktopDisp);
        if (hr == S_FALSE || (SUCCEEDED(hr) && !desktopDisp.p)) hr = E_FAIL;
        if (SUCCEEDED(hr)) hr = desktopDisp->QueryInterface(IID_IServiceProvider, reinterpret_cast<void**>(&sp));
        if (SUCCEEDED(hr)) hr = sp->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, reinterpret_cast<void**>(&browser));
        if (SUCCEEDED(hr)) hr = browser->QueryActiveShellView(&view);
        if (SUCCEEDED(hr)) hr = view->GetItemObject(SVGIO_BACKGROUND, IID_IDispatch, reinterpret_cast<void**>(&folderDisp));
        if (SUCCEEDED(hr)) hr = folderDisp->QueryInterface(IID_IShellFolderViewDual, reinterpret_cast<void**>(&folderView));
        if (SUCCEEDED(hr)) hr = folderView->get_Application(&appDisp);
        if (SUCCEEDED(hr)) hr = appDisp->QueryInterface(IID_IShellDispatch2, reinterpret_cast<void**>(&shell));
        if (SUCCEEDED(hr)) {
            BSTR file = SysAllocString(target);
#pragma push_macro("ShellExecute")
#undef ShellExecute  // shellapi.h's ShellExecute -> ShellExecuteW macro hides the COM method
            hr = shell->ShellExecute(file, empty, empty, empty, empty);
#pragma pop_macro("ShellExecute")
            SysFreeString(file);
        }
    }
    if (FAILED(hr)) {
        Log(L"open via Explorer failed 0x%08X, opening directly", unsigned(hr));
        ShellExecuteW(nullptr, L"open", target, nullptr, nullptr, SW_SHOWNORMAL);
    }
}

namespace {
HWND g_target = nullptr;
HANDLE g_hookThread = nullptr;
DWORD g_hookThreadId = 0;
bool g_eatUp[256] = {};  // key-downs we swallowed: swallow their key-ups too
std::atomic<bool> g_takePrtScn{true}, g_takeWinShiftS{true}, g_blockWin{true};

bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// After swallowing a Win+key combo, Windows would treat the coming Win
// release as "Win pressed alone" and open Start. An unassigned key in between
// prevents that (the same trick AutoHotkey uses).
void MaskWinKey() {
    INPUT in[2] = {};
    for (int i = 0; i < 2; ++i) {
        in[i].type = INPUT_KEYBOARD;
        in[i].ki.wVk = 0xE8;  // unassigned
        in[i].ki.dwFlags = i ? KEYEVENTF_KEYUP : 0;
        in[i].ki.dwExtraInfo = kInjectTag;
    }
    SendInput(2, in, sizeof(INPUT));
}

LRESULT CALLBACK KeyProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        const bool up = wp == WM_KEYUP || wp == WM_SYSKEYUP;
        const DWORD vk = k->vkCode & 0xFF;
        if (k->dwExtraInfo != kInjectTag) {
            if (up) {
                if (g_eatUp[vk]) { g_eatUp[vk] = false; return 1; }
                return CallNextHookEx(nullptr, code, wp, lp);
            }
            // One request per physical press; keep repeats swallowed even if
            // the user releases the modifiers before the captured key.
            if (g_eatUp[vk]) return 1;
            const bool win = Down(VK_LWIN) || Down(VK_RWIN), shift = Down(VK_SHIFT), ctrl = Down(VK_CONTROL),
                       alt = Down(VK_MENU);
            auto eat = [&](bool capture) {
                g_eatUp[vk] = true;
                if (win) MaskWinKey();
                if (capture) PostMessageW(g_target, WM_APP_CAPTURE, 0, 0);
                return LRESULT(1);
            };
            if (vk == VK_SNAPSHOT) {
                const bool plain = !win && !shift && !ctrl && !alt;
                if (plain && g_takePrtScn) return eat(true);
                // Win+PrtScn, Alt+PrtScn, Win+Alt+PrtScn (Game Bar) and plain PrtScn when not ours
                if (g_blockWin) return eat(false);
            } else if (vk == 'S' && win && shift && !ctrl && !alt) {
                if (g_takeWinShiftS) return eat(true);
                if (g_blockWin) return eat(false);  // Snipping Tool capture
            } else if (vk == 'R' && win && shift && !ctrl && !alt) {
                if (g_blockWin) return eat(false);  // Snipping Tool screen recording
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

DWORD WINAPI HookThread(void*) {
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);  // create the message queue
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyProc, GetModuleHandleW(nullptr), 0);
    if (!hook) {
        Log(L"SetWindowsHookEx failed: %lu", GetLastError());
        PostMessageW(g_target, WM_APP_HOOKFAIL, 0, 0);
        return 1;
    }
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {}
    UnhookWindowsHookEx(hook);
    return 0;
}
}  // namespace

bool StartKeyHook(HWND target) {
    g_target = target;
    g_hookThread = CreateThread(nullptr, 0, HookThread, nullptr, 0, &g_hookThreadId);
    return g_hookThread != nullptr;
}

void SetHookOptions(bool prtScn, bool winShiftS, bool blockWindows) {
    g_blockWin = blockWindows;
    g_takePrtScn = prtScn;
    g_takeWinShiftS = winShiftS;
}

void StopKeyHook() {
    if (!g_hookThread) return;
    PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(g_hookThread, 2000);
    CloseHandle(g_hookThread);
    g_hookThread = nullptr;
}
