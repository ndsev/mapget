#pragma once

#include <vector>
#include <string>

#include "nlohmann/json.hpp"
#include "yaml-cpp/yaml.h"

namespace mapget
{
    bool validateConfig(const nlohmann::json &config, const nlohmann::json &schema);
    const std::array<std::string_view, 2>& sharedTopLevelConfigKeys();
    std::string stringToHash(const std::string& input);
    nlohmann::json yamlToJson(
        const YAML::Node& yamlNode,
        std::unordered_map<std::string, std::string>* maskedSecretMap = nullptr,
        const bool mask = false);

    int runFromCommandLine(std::vector<std::string> args, bool requireSubcommand = true);

    bool isPostConfigEndpointEnabled();
    bool isGetConfigEndpointEnabled();
    void setPostConfigEndpointEnabled(bool enabled);
    void setGetConfigEndpointEnabled(bool enabled);

    const std::string &getPathToSchema();
    void setPathToSchema(const std::string &path);
}
