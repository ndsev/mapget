#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <functional>
#include <CLI/CLI.hpp>

namespace mapget
{
    /** Called after `serve` has bound its listener, with the configured host and actual port. */
    using ServeStartedCallback = std::function<void(std::string const&, std::uint16_t)>;

    /**
     * Run the mapget command-line interface.
     *
     * Embedding applications may extend the CLI and observe successful server
     * startup without polling. The startup callback executes before `serve`
     * begins waiting for a termination signal.
     */
    int runFromCommandLine(
        std::vector<std::string> args,
        bool requireSubcommand = true,
        std::function<void(CLI::App&)> additionalCommandLineSetupFun = {},
        ServeStartedCallback serveStartedCallback = {});

    bool isPostConfigEndpointEnabled();
    bool isGetConfigEndpointEnabled();
    void setPostConfigEndpointEnabled(bool enabled);
    void setGetConfigEndpointEnabled(bool enabled);

    const std::string &getPathToSchemaPatch();
    void setPathToSchema(const std::string &path);
}
