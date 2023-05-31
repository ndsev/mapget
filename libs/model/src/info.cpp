#include "info.h"
#include <tuple>

namespace mapget
{

bool Version::isCompatible(const Version& other) const
{
    return other.major_ == major_ && other.minor_ == minor_;
}

std::string Version::toString() const
{
    return stx::format("{}.{}.{}", major_, minor_, patch_);
}

bool Version::operator==(const Version& other) const
{
    return (
        std::tie(other.major_, other.minor_, other.patch_) ==
        std::tie(major_, minor_, patch_));
}

bool Version::operator<(const Version& other) const
{
    return (
        std::tie(other.major_, other.minor_, other.patch_) <
        std::tie(major_, minor_, patch_));
}

}
