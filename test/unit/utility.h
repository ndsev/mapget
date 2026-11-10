#pragma once
#include <filesystem>
#include <string>

namespace mapget
{

namespace test
{

std::string generateTimestampedDirectoryName(const std::string& baseName);

/** Own a newly created private directory; never adopt an existing path or symlink.
 * The parent must be trusted (e.g. the OS temporary directory with sticky/private
 * access); Windows requires an ACL-capable filesystem. */
class TemporaryDirectory
{
public:
    /** Atomically create path with owner-only access, or throw without changing it. */
    explicit TemporaryDirectory(std::filesystem::path path);
    ~TemporaryDirectory();

    TemporaryDirectory(TemporaryDirectory const&) = delete;
    TemporaryDirectory& operator=(TemporaryDirectory const&) = delete;

    std::filesystem::path const& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace test

}  // namespace mapget
