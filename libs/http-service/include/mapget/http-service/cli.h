#pragma once

#include <vector>
#include <string>

namespace mapget
{
    int runFromCommandLine(std::vector<std::string> args, bool requireSubcommand = true);

    bool isPostConfigEndpointEnabled();
    void setPostConfigEndpointEnabled(bool enabled);

    const std::string &getPathToSchema();
    void setPathToSchema(const std::string &path);
}
