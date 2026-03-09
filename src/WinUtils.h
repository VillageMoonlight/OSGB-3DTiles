#pragma once
/**
 * WinUtils.h
 * Windows 平台工具函数（目前包含中文路径转短路径功能）
 */

#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/**
 * 将 UTF-8 路径转换为 Windows 8.3 短路径（全 ASCII），
 * 用于绕过不支持 Unicode 的旧式 C API（如 osgDB 内部的 fopen/ANSI 接口）。
 *
 * 若文件系统不支持短路径（如 ReFS），则回退为系统代码页转换。
 */
inline std::string toShortPath(const std::string& utf8Path) {
    // 1. UTF-8 → wchar_t
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return utf8Path;
    std::wstring wpath(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, &wpath[0], wlen);
    if (!wpath.empty() && wpath.back() == L'\0') wpath.pop_back();

    // 2. 获取 8.3 短路径
    DWORD slen = GetShortPathNameW(wpath.c_str(), nullptr, 0);
    if (slen == 0) {
        // 短路径不可用，退回用系统代码页转换
        int mlen = WideCharToMultiByte(CP_ACP, 0, wpath.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (mlen <= 0) return utf8Path;
        std::string result(mlen, '\0');
        WideCharToMultiByte(CP_ACP, 0, wpath.c_str(), -1, &result[0], mlen, nullptr, nullptr);
        if (!result.empty() && result.back() == '\0') result.pop_back();
        return result;
    }
    std::wstring shortW(slen, L'\0');
    GetShortPathNameW(wpath.c_str(), &shortW[0], slen);
    if (!shortW.empty() && shortW.back() == L'\0') shortW.pop_back();

    // 3. wchar_t → 窄字符串（8.3 路径全为 ASCII，CP_ACP 安全）
    int mlen = WideCharToMultiByte(CP_ACP, 0, shortW.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (mlen <= 0) return utf8Path;
    std::string result(mlen, '\0');
    WideCharToMultiByte(CP_ACP, 0, shortW.c_str(), -1, &result[0], mlen, nullptr, nullptr);
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}

#else
// 非 Windows 平台：路径直接透传
inline std::string toShortPath(const std::string& path) { return path; }
#endif
