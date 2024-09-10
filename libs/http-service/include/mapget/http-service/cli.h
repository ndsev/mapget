#pragma once

#include <vector>
#include <string>

namespace mapget
{
    int runFromCommandLine(std::vector<std::string> args, bool requireSubcommand = true);

    bool isConfigEndpointEnabled();
    void setConfigEndpointEnabled(bool enabled);

    const std::string &getPathToSchema();
    void setPathToSchema(const std::string &path);
}
