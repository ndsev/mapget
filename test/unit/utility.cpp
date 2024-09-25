#include "utility.h"
#include <chrono>

namespace mapget::test
{

std::string generateTimestampedDirectoryName(const std::string& baseName)
{
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    return baseName + "_" + std::to_string(millis);
}

}
