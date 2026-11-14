// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "core/common.h"

constexpr UINT WM_APP_CAPTURE = WM_APP + 1;    // hotkey pressed
constexpr UINT WM_APP_HOOKFAIL = WM_APP + 2;   // keyboard hook could not be installed
constexpr UINT WM_APP_TRAY = WM_APP + 3;       // tray icon callback
constexpr UINT WM_APP_OCRDONE = WM_APP + 4;    // lParam owns the completed OCR result

// A newer screenshot, OCR request or editor action supersedes older automatic copies.
uint64_t BeginClipboardOperation();
bool ClipboardOperationCurrent(uint64_t operation);

// Low-level keyboard hook on its own thread (so saving a PNG never stalls
// typing). Takes over PrtScn and Win+Shift+S and posts WM_APP_CAPTURE.
bool StartKeyHook(HWND target);
void StopKeyHook();

// Opens a folder or URL through Explorer, so the browser or folder window runs
// with normal rights instead of inheriting this process's administrator token.
void OpenUnelevated(const wchar_t* target);

bool GetAutoSave();
void SetAutoSave(bool on);
int GetLastMode();  // SnipMode the capture bar opens in
void SetLastMode(int mode);
int GetThemeSetting();  // 0 follow Windows (default), 1 light, 2 dark
void SetThemeSetting(int v);
int GetDefaultMode();   // 0 = last used (default), 1..4 = Rect, Window, Full, Free
void SetDefaultMode(int v);
std::wstring GetCustomSaveDir();  // empty = Pictures\Screenshots
void SetCustomSaveDir(const std::wstring& dir);
bool GetTakePrtScn();   // hook PrtScn (default on)
void SetTakePrtScn(bool on);
bool GetTakeWinShiftS();
void SetTakeWinShiftS(bool on);
bool GetBlockWinShots();  // while running, Windows' own screenshot shortcuts do nothing (default on)
void SetBlockWinShots(bool on);
DWORD GetCustomHotkey();  // (MOD_* << 16) | virtual key; 0 = none
void SaveCustomHotkey(DWORD combo);
int GetSaveFormat();      // SaveFormat: 0 PNG (default), 1 HEIC via NVENC, 2 HEIC via Windows
void SetSaveFormat(int v);
bool GetNvenc444();       // NVENC chroma: false = 4:2:0 (default), true = 4:4:4
void SetNvenc444(bool on);
bool GetShowToast();      // notice after a capture (default on); failures are always shown
void SetShowToast(bool on);
bool GetShowSelectionSize();  // rectangle, window and text selection width x height (default off)
void SetShowSelectionSize(bool on);
void SetHookOptions(bool prtScn, bool winShiftS, bool blockWindows);  // what the keyboard hook takes over

// main.cpp
void ThemeChanged();       // re-read theme, repaint all windows
void LanguageChanged();    // re-read language, repaint all windows
void ApplyHookSettings();  // push the PrtScn / Win+Shift+S switches to the hook
bool SetCustomHotkey(DWORD combo, std::wstring& error);  // registers and saves; 0 clears
void SuspendCustomHotkey(bool suspend);                 // while the settings window records a key

// settings.cpp
void OpenSettings();
void RefreshSettingsWindow();
bool GetStartup();
void SetStartup(bool on);
void RefreshStartupTask();  // remove this user's duplicate tasks and refresh an enabled task's path
