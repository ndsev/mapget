#pragma once

#include "mcp-mapget-call.h"

namespace mapget::detail
{

/** Invocation-private simfil runtime, bounded value conversion and schema inspection.
 * Borrows its enclosing Call for authority, response budgets and completion.
 */
class McpMapgetTools::Call::Query
{
public:
    /** Bind the runtime to the invocation that owns it. */
    explicit Query(Call& call) : call_(call) {}

    /** Expose the private dictionary to extraction, after environment(strings) initialized it. */
    simfil::Environment* runtime() const { return env_.get(); }

    /** Reserve actual JSON string escaping without allocating a serialized copy. */
    void chargeString(std::string_view text);

    /** Walk existing JSON under the same limits without first dumping an arbitrarily large value.
     */
    nlohmann::json boundedJson(nlohmann::json const& value, size_t depth = 0);

    /** Convert values with resource bounds, a stack-safety guard and exact scalar tags. */
    nlohmann::json valueJson(simfil::Value const& value, size_t depth = 0);

    /** Build one private dictionary/environment; never mutate a cached model or its strings. */
    void environment(std::shared_ptr<simfil::StringPool> strings = {});

    /** Report query-local diagnostics separately from administrative operational snapshots. */
    void diagnostics(simfil::Diagnostics const& diagnostics, std::string const& expression);

    /** Reuse compilation per bound dictionary/root schema and stream bounded result conversion. */
    nlohmann::json evaluate(
        std::string const& query,
        simfil::ModelNode const& root,
        simfil::TypeId schema,
        bool predicate = false);

    /** Project a schema descriptor, expanding only the selected semantic containers. */
    nlohmann::json schemaOverview(
        simfil::ModelNode const& node,
        std::set<simfil::TypeId> const& expanded,
        size_t depth = 0);

    /** Query the lazy schema graph, not synthetic sample features. */
    void querySchema();

    /** Find declared fields, producer names and enum symbols without expanding their payloads. */
    void
    findSchema(std::vector<simfil::TypeId> const& roots, std::vector<std::string> const& types);

    /** Compile and inspect schema references without fetching any map payload. */
    void validateExpression();

private:
    class Trace;
    Call& call_;
    std::unique_ptr<simfil::Environment> env_;
    std::unique_ptr<simfil::Function> traceFunction_;
    std::map<std::tuple<std::string, simfil::TypeId, bool>, simfil::ASTPtr> expressions_;

    /** Stack-safety guard, not a user-selectable discovery boundary. */
    static constexpr size_t SerializationNestingLimit = 256;
};

}  // namespace mapget::detail
