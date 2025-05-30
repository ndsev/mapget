#pragma once

#include <vector>
#include <string>

namespace mapget
{
    int runFromCommandLine(std::vector<std::string> args, bool requireSubcommand = true);

    bool isPostConfigEndpointEnabled();
    bool isGetConfigEndpointEnabled();
    void setPostConfigEndpointEnabled(bool enabled);
    void setGetConfigEndpointEnabled(bool enabled);

    const std::string &getPathToSchema();
    void setPathToSchema(const std::string &path);
}
