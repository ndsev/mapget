#include "utility.h"
#include <chrono>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <sddl.h>
#else
#include <sys/stat.h>
#include <cerrno>
#endif

namespace mapget::test
{

std::string generateTimestampedDirectoryName(const std::string& baseName)
{
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    return baseName + "_" + std::to_string(millis);
}

TemporaryDirectory::TemporaryDirectory(std::filesystem::path path) : path_(std::move(path))
{
    // Set access restrictions at creation, not afterward: a shared temporary
    // parent must never expose a window in which another user can insert files.
#ifdef _WIN32
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;OW)",
            SDDL_REVISION_1,
            &descriptor,
            nullptr))
    {
        throw std::system_error(GetLastError(), std::system_category(), "Private directory ACL");
    }
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    auto created = CreateDirectoryW(path_.c_str(), &attributes);
    auto error = GetLastError();
    LocalFree(descriptor);
    if (!created)
        throw std::system_error(error, std::system_category(), "Create private directory");
#else
    if (::mkdir(path_.c_str(), S_IRWXU) != 0)
        throw std::system_error(errno, std::generic_category(), "Create private directory");
#endif
}

TemporaryDirectory::~TemporaryDirectory()
{
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
}

}  // namespace mapget::test
