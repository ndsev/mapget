#pragma once

#include <vector>
#include <string>

namespace mapget {
    int runFromCommandLine(std::vector<std::string> args, bool requireSubcommand = true);
}
