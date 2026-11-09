#pragma once

#include "mapget/model/simfilutil.h"
#include "mapget/service/locate.h"
#include "mcp-mapget-tools.h"
#include "simfil/function.h"
#include "simfil/simfil.h"

#include <set>

namespace mapget::detail
{

/** One native invocation owns budgets, query bindings and its single outstanding partition. */
class McpMapgetTools::Call : public std::enable_shared_from_this<Call>
{
public:
    Principal principal;
    std::string name;
    nlohmann::json arguments;
    Complete complete;
    std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);

    /** Capture authority and bound execution by both the request deadline and token expiry. */
    Call(
        McpMapgetTools& owner,
        Principal caller,
        std::string action,
        nlohmann::json args,
        Complete callback);

    /** Destroy invocation-private query and extraction state after callbacks release this call. */
    ~Call();

    /** Worker entry for metadata tools or asynchronous extraction preparation. */
    void run(bool admitted);

    /** Stop a queued/running read and detach a child request without blocking the event loop. */
    void cancel();

private:
    class Query;
    class Extraction;
    McpMapgetTools& owner_;
    std::chrono::steady_clock::time_point deadline_;
    size_t remainingWork_ = 100000;
    size_t remainingBytes_ = 0;

    size_t limit_ = 100;
    size_t completeRows_ = 0;
    std::string incomplete_;
    nlohmann::json items_ = nlohmann::json::array();
    nlohmann::json issues_ = nlohmann::json::array();
    nlohmann::json traces_ = nlohmann::json::object();
    nlohmann::json helpRevision_ = nullptr;
    nlohmann::json discovery_ = nlohmann::json::object();
    nlohmann::json narrowing_ = nlohmann::json::array();
    std::shared_ptr<LayerInfo> layer_;
    DataSourceCatalogEntry source_;

    /** Fence inventory/narrowing responses which have no single bound layer owner. */
    std::optional<uint64_t> schemaCatalogRevision_;
    std::optional<size_t> nextOffset_;

    /** Serializes callback state; cancellation itself only changes the atomic token. */
    std::recursive_mutex executionMutex_;
    std::atomic_bool finished_{false};

    std::unique_ptr<Query> query_;
    std::unique_ptr<Extraction> extraction_;

    /** Charge model walking as well as evaluator work, including traversals with no matches. */
    bool step(size_t count = 1);

    /** Retain an explicit reason for partial enumeration, without invalid JSON truncation. */
    void truncate(std::string reason);

    /** Fail without exposing arbitrary datasource exceptions, file paths or config secrets. */
    void fail(std::string code, std::string message);

    /** Final schema/byte validation and exactly-once callback/owner retirement. */
    void finish(nlohmann::json result);

    /** Wrap bounded items with completeness and per-operation issues. */
    void finishItems();

    /** Append one bounded row without retaining response-task state. */
    bool append(nlohmann::json item);

    /** Reserve result construction bytes before allocating another bounded value. */
    void charge(size_t bytes);

    /** Choose one authorized primary datasource; ambiguous identities require sourceId. */
    void selectLayer();

    /** Resolve a map, descriptive format or exact artifact; ambiguity returns narrowing choices. */
    bool selectSchemaLayer();

    /** Serialize compact authorized source metadata without invoking JSON Schema emission. */
    void listSources();

    /** List authorized immutable artifacts once, independently of their consuming maps. */
    void listSchemas();

    /** Search source-backed help with the same response, cancellation and allocation budgets. */
    void documentation();

    /** Return advertised coverage separately from source discovery, retaining sparse occupancy. */
    void getCoverage();

    /** Produce packed tile/grid/bounds information through ndslive-math only. */
    nlohmann::json convertTile();

    /** Convert longitude/latitude between WGS84 degrees and signed NDS coordinates. */
    nlohmann::json convertCoordinates();
};

}  // namespace mapget::detail
