#pragma once

#include "mapget/model/simfilutil.h"
#include "mcp-native-tools.h"
#include "simfil/function.h"
#include "simfil/simfil.h"

#include <set>

namespace mapget::detail
{

/** One native invocation owns budgets, query bindings and its single outstanding partition. */
class McpNativeTools::Call : public std::enable_shared_from_this<Call>
{
public:
    Principal principal;
    std::string name;
    nlohmann::json arguments;
    Complete complete;
    std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);

    /** Capture authority and bound execution by both the request deadline and token expiry. */
    Call(
        McpNativeTools& owner,
        Principal caller,
        std::string action,
        nlohmann::json args,
        Complete callback);
    /** Worker entry for metadata tools or asynchronous extraction preparation. */
    void run(bool admitted);
    /** Stop a queued/running read and detach a child request without blocking the event loop. */
    void cancel();

private:
    class Trace;
    McpNativeTools& owner_;
    std::chrono::steady_clock::time_point deadline_;
    size_t remainingWork_ = 100000;
    size_t remainingBytes_ = 0;
    /** Last-resort stack guard; not a discovery setting or a schema expansion boundary. */
    static constexpr size_t SerializationNestingLimit = 256;
    size_t limit_ = 100;
    std::string incomplete_;
    nlohmann::json items_ = nlohmann::json::array();
    nlohmann::json issues_ = nlohmann::json::array();
    nlohmann::json traces_ = nlohmann::json::object();
    nlohmann::json helpRevision_ = nullptr;
    std::shared_ptr<LayerInfo> layer_;
    DataSourceCatalogEntry source_;
    std::vector<PartitionId> partitions_;
    size_t nextPartition_ = 0;
    std::vector<std::string> featureIds_;
    std::unique_ptr<simfil::Environment> env_;
    std::unique_ptr<simfil::Function> traceFunction_;
    std::map<std::tuple<std::string, simfil::SchemaId, bool>, simfil::ASTPtr> expressions_;
    std::mutex requestMutex_;
    /** Serializes callback state; cancellation itself only changes the atomic token. */
    std::recursive_mutex executionMutex_;
    LayerTilesRequest::Ptr request_;
    std::atomic_bool finished_{false};

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
    /** Append one bounded result, never a page/cursor or retained continuation. */
    bool append(nlohmann::json item);
    /** Convert values with resource bounds, a stack-safety guard and exact scalar tags. */
    nlohmann::json valueJson(simfil::Value const& value, size_t depth = 0);
    /** Walk existing JSON under the same limits without first dumping an arbitrarily large value.
     */
    nlohmann::json boundedJson(nlohmann::json const& value, size_t depth = 0);
    /** Account string escaping before allocating result strings. */
    void charge(size_t bytes);
    /** Reuse compilation per bound dictionary/root schema and stream bounded result conversion. */
    nlohmann::json evaluate(
        std::string const& query,
        simfil::ModelNode const& root,
        simfil::SchemaId schema,
        bool predicate = false);
    /** Report query-local diagnostics separately from administrative operational snapshots. */
    void diagnostics(simfil::Diagnostics const& diagnostics);
    /** Choose one authorized primary datasource; ambiguous identities require sourceId. */
    void selectLayer();
    /** Build one private dictionary/environment; never mutate a cached model or its strings. */
    void environment(std::shared_ptr<simfil::StringPool> strings = {});

    /** Serialize compact authorized source metadata without invoking JSON Schema emission. */
    void listSources();
    /** Search source-backed help with the same response, cancellation and allocation budgets. */
    void documentation();
    /** Return advertised coverage separately from source discovery, retaining sparse occupancy. */
    void getCoverage();
    /** Query the lazy schema graph, not synthetic sample features. */
    void querySchema();
    /** Project a schema descriptor, expanding only the selected semantic containers. */
    nlohmann::json schemaOverview(
        simfil::ModelNode const& node,
        std::set<simfil::SchemaId> const& expanded,
        size_t depth = 0);
    /** Compile and inspect schema references without fetching any map payload. */
    void validateExpression();
    /** Prepare explicit partitions or cheap primary-ID locate candidates. */
    void prepareExtraction();
    /** Schedule one partition and evaluate on the delivering worker before loading another. */
    void loadNext();
    /** Iterate feature/attribute contexts with bounded predicates and projected sequences. */
    void extractFeatures(PartitionFeatureLayer::Ptr const& tile);
    /** Resolve native source addresses or query source model roots directly. */
    void extractSourceData(PartitionSourceDataLayer::Ptr const& tile);
    /** Materialize one matched row with optional effective geometry and native provenance. */
    bool extractRow(
        model_ptr<Feature> const& feature,
        simfil::ModelNode const& context,
        simfil::SchemaId schema,
        nlohmann::json identity,
        model_ptr<Validity> const& validity = {});
    /** Produce packed tile/grid/bounds information through ndslive-math only. */
    nlohmann::json convertTile();
    /** Convert longitude/latitude between WGS84 degrees and signed NDS coordinates. */
    nlohmann::json convertCoordinates();
};

}  // namespace mapget::detail
