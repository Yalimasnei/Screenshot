// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "core/common.h"
#include <algorithm>

inline std::wstring ModulePath() {
    for (DWORD capacity = 260; capacity <= 32768; capacity = std::min<DWORD>(capacity * 2, 32768)) {
        std::wstring path(capacity, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), capacity);
        if (!length) return {};
        if (length < capacity) { path.resize(length); return path; }
        if (capacity == 32768) break;
    }
    return {};
}

inline std::wstring ExtendedFilePath(const std::wstring& path) {
    const DWORD capacity = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!capacity) return {};
    std::wstring full(capacity, L'\0');
    const DWORD length = GetFullPathNameW(path.c_str(), capacity, full.data(), nullptr);
    if (!length || length >= capacity) return {};
    full.resize(length);
    if (full.compare(0, 4, L"\\\\?\\") == 0) return full;
    return full.compare(0, 2, L"\\\\") == 0 ? L"\\\\?\\UNC\\" + full.substr(2) : L"\\\\?\\" + full;
}

inline std::wstring TempDirectory() {
    DWORD capacity = GetTempPathW(0, nullptr);
    for (int attempt = 0; capacity && capacity <= 32768 && attempt < 4; ++attempt) {
        std::wstring path(capacity, L'\0');
        const DWORD length = GetTempPathW(capacity, path.data());
        if (!length) return {};
        if (length < capacity) { path.resize(length); return path; }
        capacity = length + 1;
    }
    return {};
}
