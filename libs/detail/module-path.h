#pragma once

#include <filesystem>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

namespace mapget::detail
{

/** Locate resources beside the module containing anchor, including a Python extension. */
inline std::filesystem::path moduleDirectory(void const* anchor)
{
#ifdef _WIN32
    HMODULE module = nullptr;
    auto flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(anchor), &module)) {
        std::wstring buffer(256, L'\0');
        while (buffer.size() <= 32768) {
            auto size =
                GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (!size)
                break;
            if (size < buffer.size()) {
                buffer.resize(size);
                return std::filesystem::path(buffer).parent_path();
            }
            buffer.resize(buffer.size() * 2);
        }
    }
#else
    Dl_info info{};
    if (dladdr(anchor, &info) && info.dli_fname && *info.dli_fname)
        return std::filesystem::path(info.dli_fname).parent_path();
#endif
    return std::filesystem::current_path();
}

}  // namespace mapget::detail
