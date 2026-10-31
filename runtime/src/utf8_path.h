#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace botw {

inline std::string wide_to_utf8(std::wstring_view value) {
#if defined(_WIN32)
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                                   nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                            result.data(), size, nullptr, nullptr) != size)
        return {};
    return result;
#else
    return std::string(value.begin(), value.end());
#endif
}

inline std::filesystem::path path_from_utf8(std::string_view value) {
    if (value.empty()) return {};
#if defined(_WIN32)
    std::u8string_view u8(reinterpret_cast<const char8_t*>(value.data()), value.size());
    return std::filesystem::path(u8);
#else
    return std::filesystem::path(std::string(value));
#endif
}

inline std::string path_to_utf8(const std::filesystem::path& path) {
    std::u8string value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

inline FILE* fopen_utf8(std::string_view path, const char* mode) {
    if (path.empty() || !mode) return nullptr;
#if defined(_WIN32)
    std::filesystem::path native = path_from_utf8(path);
    std::wstring wide_mode;
    for (const unsigned char ch : std::string_view(mode)) wide_mode.push_back(static_cast<wchar_t>(ch));
    return _wfopen(native.c_str(), wide_mode.c_str());
#else
    std::string native(path);
    return std::fopen(native.c_str(), mode);
#endif
}

#if defined(_WIN32)
inline std::string getenv_utf8(const wchar_t* name) {
    if (!name || !*name) return {};
    DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return {};
    std::vector<wchar_t> value(size);
    DWORD written = GetEnvironmentVariableW(name, value.data(), size);
    if (!written || written >= size) return {};
    return wide_to_utf8(std::wstring_view(value.data(), written));
}

inline std::vector<std::string> command_line_utf8() {
    int count = 0;
    LPWSTR* wide_args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!wide_args) return {};
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) args.push_back(wide_to_utf8(wide_args[i]));
    LocalFree(wide_args);
    return args;
}
#else
inline std::string getenv_utf8(const char* name) {
    const char* value = name ? std::getenv(name) : nullptr;
    return value ? std::string(value) : std::string();
}
#endif

}  // namespace botw
