#pragma once

#include <vector>
#include <string>
#include <functional>
#include <CLI/CLI.hpp>

namespace mapget
{
    int runFromCommandLine(std::vector<std::string> args, bool requireSubcommand = true, std::function<void(CLI::App&)> additionalCommandLineSetupFun = {});

    bool isPostConfigEndpointEnabled();
    bool isGetConfigEndpointEnabled();
    void setPostConfigEndpointEnabled(bool enabled);
    void setGetConfigEndpointEnabled(bool enabled);

    const std::string &getPathToSchema();
    void setPathToSchema(const std::string &path);
}
