#include "mapget/model/layerschema.h"
#include "mapget/model/mapschema.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace mapget
{

/** One bounded search owns its ranking, representative paths and transient projection budget. */
class LayerSchema::Search
{
public:
    /** Borrow the selected immutable roots and prepare one allocation-free text matcher. */
    Search(
        LayerSchema const& layer,
        std::string_view text,
        std::span<simfil::TypeId const> roots,
        std::span<std::string const> types,
        simfil::EvaluationOptions const& limits,
        size_t maxBytes)
        : layer_(layer),
          owner_(layer.mapSchema()),
          roots_(roots),
          types_(types),
          limits_(limits),
          remainingWork_(limits.maxWork),
          remainingBytes_(maxBytes)
    {
        if (!owner_->finalized())
            throw std::logic_error("Schema search requires a finalized MapSchema.");
        if (text.size() > 128 || (needle_ = normalize(text)).empty())
            throw std::invalid_argument(
                "find requires a declared field, type or symbol name (at most 128 bytes).");
        if (!types.empty() &&
            (types.size() != roots.size() || layer.layerType() != LayerType::Features))
            throw std::invalid_argument("Feature search contexts must match the selected roots.");
        if (limits.maxResults == 0)
            throw std::invalid_argument("Schema search requires a positive result limit.");
        prefix_.resize(needle_.size());
        for (size_t i = 1, length = 0; i < needle_.size(); ++i) {
            while (length && needle_[i] != needle_[length])
                length = prefix_[length - 1];
            if (needle_[i] == needle_[length])
                ++length;
            prefix_[i] = length;
        }
    }

    /** Rank the bounded scan, then project retained rows even when scanning stopped early. */
    nlohmann::json run(simfil::EvaluationSummary* summary)
    {
        // Keep enough work to emit useful partial results after the scan reaches its limit.
        auto reserve = std::min<size_t>(remainingWork_ / 4, 8000);
        remainingWork_ -= reserve;
        auto rootMatches = Json::array();
        try {
            charge(512);  // Response framing, statistics and bounded root-navigation framing.
            for (root_ = 0; root_ < roots_.size() && step(); ++root_) {
                auto id = roots_[root_];
                if (!layer_.hasSchema(id))
                    throw std::invalid_argument("Unknown schema search root.");
                if (!types_.empty() &&
                    (!layer_.producesFeatureType(types_[root_]) ||
                     layer_.featureSchema(types_[root_]) != id))
                    throw std::invalid_argument(
                        "Feature search root does not match its produced type.");
                visited_.clear();
                rootMatches_.clear();
                walk(id, {});
                if (!rootMatches_.empty() && rootMatches.size() < 8 && roots_.size() > 1) {
                    charge(96);
                    Json entry{{"typeId", id}, {"matchedTypes", rootMatches_.size()}};
                    if (!types_.empty())
                        entry["featureType"] = copyText(types_[root_]);
                    rootMatches.push_back(std::move(entry));
                }
            }
        }
        catch (std::length_error const&) {
            // A failing candidate never becomes visible in best_. Earlier complete rows survive.
        }
        auto scanReason = std::exchange(reason_, {});
        remainingWork_ += reserve;
        std::vector<std::pair<simfil::TypeId, Match*>> ordered;
        for (auto& [id, match] : best_)
            ordered.emplace_back(id, &match);
        std::stable_sort(
            ordered.begin(),
            ordered.end(),
            [](auto const& a, auto const& b) { return a.second->rank < b.second->rank; });
        auto items = Json::array();
        for (auto const& [id, match] : ordered) {
            try {
                if (!step())
                    break;
                auto literals = layer_.enumValues(id);
                if (!literals.empty()) {
                    auto preview = Json::array();
                    charge(96);
                    for (size_t i = 0; i < std::min<size_t>(literals.size(), 8); ++i)
                        preview.push_back(scalar(literals[i]));
                    match->item["enum"]["literalPreview"] = std::move(preview);
                    match->item["enum"]["literalsOmitted"] = literals.size() > 8;
                }
                items.push_back(std::move(match->item));
            }
            catch (std::length_error const&) {
                break;
            }
        }
        if (reason_.empty())
            reason_ = !scanReason.empty() ?
                scanReason :
                matched_.size() > items.size() ?
                "matches_omitted" :
                "";
        if (summary) {
            using Reason = simfil::EvaluationSummary::Reason;
            summary->reason = reason_.empty()    ? Reason::Complete :
                reason_ == "work_limit"          ? Reason::WorkLimit :
                reason_ == "serialization_limit" ? Reason::DepthLimit :
                reason_ == "cancelled"           ? Reason::Canceled :
                reason_ == "timeout"             ? Reason::Timeout :
                reason_ == "matches_omitted"     ? Reason::ResultLimit :
                                                   Reason::ConsumerStopped;
            summary->results = items.size();
            summary->work = limits_.maxWork - remainingWork_;
        }
        Json discovery{
            {"scanComplete", scanReason.empty()},
            {"matchedTypes", matched_.size()},
            {"returnedTypes", items.size()},
            {"omittedTypes", matched_.size() - items.size()},
            {"paths", "representative"}};
        return {
            {"items", std::move(items)},
            {"complete", reason_.empty()},
            {"reason", reason_.empty() ? Json(nullptr) : Json(reason_)},
            {"discovery", std::move(discovery)},
            {"rootMatches", std::move(rootMatches)}};
    }

private:
    using Json = nlohmann::json;
    /** Retained-byte ownership follows a candidate when ranking evicts it. */
    struct Match
    {
        int rank;
        size_t bytes;
        Json item;
    };

    LayerSchema const& layer_;
    MapSchema::ConstPtr owner_;
    std::span<simfil::TypeId const> roots_;
    std::span<std::string const> types_;
    simfil::EvaluationOptions limits_;
    size_t remainingWork_, remainingBytes_, root_ = 0;
    std::string needle_, reason_;
    std::vector<size_t> prefix_;
    std::map<simfil::TypeId, Match> best_;
    std::set<simfil::TypeId> matched_, visited_, rootMatches_;
    std::vector<std::string_view> path_;
    std::vector<bool> elementSteps_;

    static std::string normalize(std::string_view value)
    {
        std::string result;
        for (unsigned char c : value)
            if (!std::isspace(c) && c != '_' && c != '-')
                result.push_back(static_cast<char>(std::tolower(c)));
        return result;
    }

    bool step(size_t work = 1)
    {
        if (!reason_.empty())
            return false;
        if (limits_.cancel && limits_.cancel->load())
            reason_ = "cancelled";
        else if (std::chrono::steady_clock::now() >= limits_.deadline)
            reason_ = "timeout";
        else if (work > remainingWork_)
            reason_ = "work_limit";
        else {
            remainingWork_ -= work;
            return true;
        }
        return false;
    }

    void charge(size_t bytes)
    {
        if (!step())
            throw std::length_error("Schema search work budget.");
        if (bytes > remainingBytes_) {
            reason_ = "byte_limit";
            throw std::length_error("Schema search byte budget.");
        }
        remainingBytes_ -= bytes;
    }

    void chargeText(std::string_view value)
    {
        // Worst-case JSON escaping, checked before allocating text or its encoded copy.
        if (value.size() > remainingBytes_ / 6) {
            reason_ = "byte_limit";
            throw std::length_error("Schema search text budget.");
        }
        charge(2 + value.size() * 6);
    }

    std::string copyText(std::string_view value)
    {
        chargeText(value);
        return std::string(value);
    }

    /** Return exact/substring relevance while charging scanned characters as work. */
    int matching(std::string_view value)
    {
        if (!step())
            throw std::length_error("Schema search text work budget.");
        // Match normalized substrings without allocating copies of potentially huge producer
        // names. The prefix table keeps overlapping patterns linear, with cooperative checkpoints.
        size_t length = 0, count = 0;
        bool equal = true, contains = false;
        for (size_t i = 0; i < value.size(); ++i) {
            if (i && i % 64 == 0 && !step())
                throw std::length_error("Schema search text work budget.");
            auto c = static_cast<unsigned char>(value[i]);
            if (std::isspace(c) || c == '_' || c == '-')
                continue;
            auto normalized = static_cast<char>(std::tolower(c));
            equal &= count < needle_.size() && normalized == needle_[count];
            ++count;
            while (length && normalized != needle_[length])
                length = prefix_[length - 1];
            if (normalized == needle_[length])
                ++length;
            if (length == needle_.size()) {
                contains = true;
                length = prefix_[length - 1];
            }
        }
        return equal && count == needle_.size() ? 2 : contains ? 1 : 0;
    }

    /** Preview literals losslessly, sharing the native MCP integer/bytes/mask tag contract. */
    Json scalar(simfil::ScalarValueType const& value)
    {
        charge(96);
        return std::visit(
            [&](auto const& v) -> Json
            {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::monostate>)
                    return nullptr;
                else if constexpr (
                    std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>)
                    return copyText(v);
                else if constexpr (std::is_same_v<T, int64_t>) {
                    if (v < -9007199254740991LL || v > 9007199254740991LL)
                        return {{"$mapget", "int64"}, {"value", std::to_string(v)}};
                    return v;
                }
                else if constexpr (std::is_same_v<T, double>) {
                    if (!std::isfinite(v))
                        return {
                            {"$mapget", "nonfinite"},
                            {"value",
                             std::isnan(v) ? "NaN" :
                                 v > 0     ? "Infinity" :
                                             "-Infinity"}};
                    return v;
                }
                else if constexpr (std::is_same_v<T, simfil::ByteArray>) {
                    if (v.bytes.size() > remainingBytes_ / 2) {
                        reason_ = "byte_limit";
                        throw std::length_error("Schema search byte literal budget.");
                    }
                    charge(v.bytes.size() * 2);
                    return {{"$mapget", "bytes"}, {"hex", v.toHex(false)}};
                }
                else if constexpr (std::is_same_v<T, simfil::Bitmask>) {
                    if (v.type)
                        chargeText(v.type.binding->identity());
                    return v.toJson();
                }
                else
                    return v;
            },
            value);
    }

    /** Describe one representative path and its real feature/attribute owner, if supplied. */
    Json context(std::string_view field)
    {
        charge(192);
        std::vector<std::string> path;
        for (auto segment : path_)
            path.push_back(copyText(segment));
        // The native path and its JSON projection have separate string ownership.
        for (auto segment : path_)
            chargeText(segment);
        Json result{{"path", path}};
        if (!field.empty())
            result["fieldName"] = copyText(field);
        if (types_.empty()) {
            result["relativeToTypeId"] = roots_[root_];
            return result;
        }
        result["featureType"] = copyText(types_[root_]);
        auto owner = layer_.ownerForPath(types_[root_], roots_[root_], path);
        for (size_t end = path.size(); owner.kind_ == PathOwnerKind::Unknown && end; --end) {
            if (!step())
                throw std::length_error("Schema search owner work budget.");
            owner =
                layer_.ownerForPath(types_[root_], roots_[root_], std::span(path.data(), end - 1));
        }
        if (owner.kind_ == PathOwnerKind::Attribute) {
            charge(128);
            result["attributeContext"] = {
                {"featureType", copyText(owner.attribute_.featureType_)},
                {"layer", copyText(owner.attribute_.attributeLayerName_)},
                {"name", copyText(owner.attribute_.attributeName_)},
                {"typeId", owner.attribute_.attributeSchema_}};
        }
        std::string query = "_";
        for (size_t i = 0; i < path.size(); ++i) {
            if (elementSteps_[i]) {
                charge(2);
                query += ".*";
            }
            else {
                // The quoted subscript is escaped a second time in the JSON response.
                if (path[i].size() > remainingBytes_ / 12) {
                    reason_ = "byte_limit";
                    throw std::length_error("Schema search query text budget.");
                }
                charge(8 + path[i].size() * 12);
                query += "[" + Json(path[i]).dump() + "]";
            }
        }
        result["featureQuery"] = std::move(query);
        return result;
    }

    /** Score every incoming edge, but expand each domain only once per selected root. */
    void walk(simfil::TypeId id, std::string_view field, size_t depth = 0)
    {
        if (!step() || id == simfil::NoTypeId)
            return;
        // Alternatives retain the same data path but still consume a C++ stack frame.
        if (depth > std::min<size_t>(limits_.maxDepth, 256)) {
            reason_ = "serialization_limit";
            return;
        }
        auto fieldMatch = matching(field), typeMatch = matching(layer_.typeName(id));
        int rank = fieldMatch == 2 ? 1 : typeMatch == 2 ? 2 : fieldMatch ? 3 : typeMatch ? 5 : 6;
        auto symbols = layer_.directEnumSymbols(id);
        std::vector<std::string_view> matchedSymbols, matchedNames;
        size_t symbolMatches = 0, namedCount = 0, namedMatches = 0;
        auto remember = [](std::vector<std::string_view>& preview, std::string_view text, int score)
        {
            if (score == 2) {
                preview.insert(preview.begin(), text);
                if (preview.size() > 8)
                    preview.pop_back();
            }
            else if (preview.size() < 8)
                preview.push_back(text);
        };
        for (auto symbol : symbols) {
            auto score = matching(symbol);
            if (!score)
                continue;
            ++symbolMatches;
            rank = std::min(rank, score == 2 ? 0 : 4);
            remember(matchedSymbols, symbol, score);
        }
        layer_.forEachNamedValue(
            id,
            [&](simfil::Schema::NamedValue const& value)
            {
                ++namedCount;
                auto score = matching(value.name);
                if (!score)
                    return;
                ++namedMatches;
                rank = std::min(rank, score == 2 ? 0 : 4);
                remember(matchedNames, value.name, score);
            });
        rank = rank * 512 + static_cast<int>(path_.size());
        if (rank < 6 * 512) {
            matched_.insert(id);
            rootMatches_.insert(id);
            auto found = best_.find(id);
            if (found == best_.end() && best_.size() >= limits_.maxResults) {
                auto worst = std::max_element(
                    best_.begin(),
                    best_.end(),
                    [](auto const& a, auto const& b) {
                        return std::pair(a.second.rank, a.first) <
                            std::pair(b.second.rank, b.first);
                    });
                if (std::pair(rank, id) < std::pair(worst->second.rank, worst->first)) {
                    remainingBytes_ += worst->second.bytes;
                    best_.erase(worst);
                }
            }
            if (found != best_.end() || best_.size() < limits_.maxResults) {
                auto before = remainingBytes_;
                try {
                    if (found == best_.end()) {
                        charge(384);
                        Json item{
                            {"typeId", id},
                            {"kind",
                             copyText(owner_->strings()
                                          ->resolve(simfil::Schema::kindNameId(layer_.kind(id)))
                                          .value_or("unknown"))},
                            {"contexts", Json::array()},
                            {"contextsOmitted", false}};
                        if (!layer_.typeName(id).empty())
                            item["typename"] = copyText(layer_.typeName(id));
                        if (namedCount) {
                            charge(160);
                            item["namedValues"] = {
                                {"count", namedCount},
                                {"matchingCount", namedMatches},
                                {"matchingNames", Json::array()}};
                            for (auto name : matchedNames)
                                item["namedValues"]["matchingNames"].push_back(copyText(name));
                        }
                        auto literals = layer_.enumValues(id);
                        if (!symbols.empty() || !literals.empty()) {
                            charge(384);
                            auto& enumeration = item["enum"];
                            enumeration = {
                                {"symbolCount", symbols.size()},
                                {"matchingSymbolCount", symbolMatches},
                                {"matchingSymbols", Json::array()},
                                {"symbolsOmitted", symbols.size() > 8},
                                {"preview", Json::array()},
                                {"literalCount", literals.size()},
                                {"symbolSemantics",
                                 layer_.kind(id) == LayerSchema::BitmaskKind ?
                                     "flags" :
                                     "string_literals"}};
                            for (auto symbol : matchedSymbols)
                                enumeration["matchingSymbols"].push_back(copyText(symbol));
                            for (size_t i = 0; i < std::min<size_t>(symbols.size(), 8); ++i)
                                enumeration["preview"].push_back(copyText(symbols[i]));
                        }
                        item["contexts"].push_back(context(field));
                        best_.emplace(id, Match{rank, before - remainingBytes_, std::move(item)});
                    }
                    else {
                        auto candidate = context(field);
                        auto& contexts = found->second.item["contexts"];
                        auto retained = false;
                        if (std::find(contexts.begin(), contexts.end(), candidate) ==
                            contexts.end()) {
                            if (rank < found->second.rank) {
                                contexts.insert(contexts.begin(), std::move(candidate));
                                if (contexts.size() > 2) {
                                    contexts.erase(contexts.end() - 1);
                                    found->second.item["contextsOmitted"] = true;
                                }
                                retained = true;
                            }
                            else if (contexts.size() < 2) {
                                contexts.push_back(std::move(candidate));
                                retained = true;
                            }
                            else
                                found->second.item["contextsOmitted"] = true;
                        }
                        found->second.rank = std::min(found->second.rank, rank);
                        if (retained)
                            found->second.bytes += before - remainingBytes_;
                        else
                            remainingBytes_ = before;
                    }
                }
                catch (std::length_error const&) {
                    remainingBytes_ = before;
                    throw;
                }
            }
        }
        if (!visited_.insert(id).second)
            return;
        // The packed enumerable edge view skips only context roles, never '$'-prefixed data.
        owner_->type(id)->forEachEnumerableField(
            [&](simfil::StringId field, auto children)
            {
                if (!step())
                    throw std::length_error("Schema search field work budget.");
                auto name = owner_->strings()->resolve(field).value();
                path_.push_back(name);
                elementSteps_.push_back(false);
                for (auto child : children)
                    walk(child, name, depth + 1);
                path_.pop_back();
                elementSteps_.pop_back();
            });
        layer_.forEachElementSchema(
            id,
            [&](simfil::TypeId child)
            {
                path_.emplace_back("*");
                elementSteps_.push_back(true);
                walk(child, {}, depth + 1);
                path_.pop_back();
                elementSteps_.pop_back();
            });
        for (auto alternative : layer_.alternatives(id))
            walk(alternative, field, depth + 1);
    }
};

nlohmann::json LayerSchema::find(
    std::string_view text,
    std::span<simfil::TypeId const> roots,
    std::span<std::string const> featureTypes,
    simfil::EvaluationOptions const& limits,
    size_t maxBytes,
    simfil::EvaluationSummary* summary) const
{
    return Search(*this, text, roots, featureTypes, limits, maxBytes).run(summary);
}

}  // namespace mapget
