#pragma once

#include "mapget/location/location.h"
#include "mapget/service/service.h"
#include "mcp-auth.h"
#include "mcp-help.h"

#include <condition_variable>

namespace mapget::detail
{

/** Native tool contracts and admission; queries run on service workers, never the MCP loop. */
class McpNativeTools
{
public:
    using Principal = McpViewerRelay::Principal;
    using Complete = McpViewerRelay::Complete;

    /** Borrow service/host collectors until stop() has drained admitted operations. */
    McpNativeTools(
        Service& service,
        McpConfig config,
        std::function<nlohmann::json()> diagnostics,
        LocationLookup const* location,
        std::function<nlohmann::json()> readConfig,
        std::function<nlohmann::json(nlohmann::json const&, std::string const&)> writeConfig);
    /** Cancel and drain operations before borrowed service owners disappear. */
    ~McpNativeTools();

    /** Return only tools permitted by both deployment and caller, without a browser requirement. */
    [[nodiscard]] nlohmann::json tools(Principal const& principal) const;
    /** Match a native name, including disabled tools so calls cannot fall through to a browser. */
    [[nodiscard]] bool contains(std::string_view name) const;
    /** Validate arguments against the same compiled schema advertised by tools/list. */
    [[nodiscard]] bool
    acceptsArguments(std::string_view name, nlohmann::json const& arguments) const;
    /** Check every successful result before sending structuredContent. */
    [[nodiscard]] bool acceptsResult(std::string_view name, nlohmann::json const& result) const;
    /** Start one operation and return a cancellation token; completion may be synchronous. */
    [[nodiscard]] std::shared_ptr<std::atomic_bool>
    invoke(Principal principal, std::string name, nlohmann::json arguments, Complete complete);
    /** Cancel by server-owned handle, never by a user-wide JSON-RPC request ID. */
    void cancel(std::shared_ptr<std::atomic_bool> const& token);
    /** Schedule at most one periodic help refresh; never scan files on the control loop. */
    void refreshHelp();
    /** Stop admission and drain callbacks before borrowed service state can be destroyed. */
    void stop();

private:
    class Call;
    /** Schema validators and their public descriptions have one immutable owner. */
    struct Action
    {
        nlohmann::json tool;
        nlohmann::json_schema::json_validator input;
        nlohmann::json_schema::json_validator output;
    };
    Service& service_;
    McpConfig config_;
    McpHelp help_;
    std::function<nlohmann::json()> diagnostics_;
    LocationLookup const* location_;
    std::function<nlohmann::json()> readConfig_;
    std::function<nlohmann::json(nlohmann::json const&, std::string const&)> writeConfig_;
    std::map<std::string, Action, std::less<>> actions_;
    std::mutex mutex_;
    std::condition_variable idle_;
    bool stopped_ = false;
    bool helpRefreshPending_ = false;
    std::chrono::steady_clock::time_point nextHelpRefresh_{};
    std::vector<std::shared_ptr<Call>> calls_;

    /** Compile native schemas once at startup, without a generated frontend artifact. */
    void buildCatalog();
    /** Apply independent data/configuration/diagnostics privileges. */
    [[nodiscard]] bool permitted(std::string_view name, Principal const& principal) const;
    /** Retire admission only after execution has really finished, including timed-out mutations. */
    void retire(Call* call);
};

}  // namespace mapget::detail
