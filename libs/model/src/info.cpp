#include "info.h"
#include "stream.h"
#include "mapget/log.h"
#include "layerschema.h"

#include <tuple>
#include <random>
#include <sstream>
#include <charconv>
#include <cctype>
#include <regex>

namespace mapget
{

namespace {

constexpr std::string_view kProtocolReservedIdentifierChars = ":/,~";

/** Returns whether a metadata version was left at its legacy aggregate default. */
[[nodiscard]] bool isDefaultVersion(Version const& version)
{
    return version.major_ == 0 && version.minor_ == 0 && version.patch_ == 0;
}

/** Stamps local in-process datasources with the current stream protocol when they did not set one. */
[[nodiscard]] Version effectiveDataSourceProtocolVersion(Version const& version)
{
    return isDefaultVersion(version) ? TileLayerStream::CurrentProtocolVersion : version;
}

/** Standardize missing-field errors across model metadata JSON parsers. */
auto missing_field(std::string const& error, std::string const& context) {
    return std::runtime_error(
        fmt::format("{}::fromJson(): `{}`", context, error));
}

template <class T, class... Args>
/** Parse a full string into a number and reject trailing characters. */
std::optional<T> from_chars(std::string_view s, Args... args)
{
    auto end = s.data() + s.size();
    T number;
    auto result = std::from_chars(s.data(), end, number, args...);
    if (result.ec != std::errc{} || result.ptr != end)
        return {};
    return number;
}

/** Check whether a character can participate in a percent escape. */
[[nodiscard]] bool isHexDigit(char ch)
{
    return std::isdigit(static_cast<unsigned char>(ch)) ||
           (ch >= 'a' && ch <= 'f') ||
           (ch >= 'A' && ch <= 'F');
}

/** Decode one hexadecimal digit used by identifier escaping. */
[[nodiscard]] uint8_t hexValue(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return static_cast<uint8_t>(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return static_cast<uint8_t>(10 + (ch - 'a'));
    }
    return static_cast<uint8_t>(10 + (ch - 'A'));
}

/** Return true when a character has structural meaning in a flattened identifier. */
[[nodiscard]] bool isReservedIdentifierCharacter(char ch, std::string_view extraReserved)
{
    return kProtocolReservedIdentifierChars.find(ch) != std::string_view::npos ||
           extraReserved.find(ch) != std::string_view::npos;
}

/** Return true when an identifier character is reserved and not explicitly allowed in this metadata context. */
[[nodiscard]] bool isForbiddenIdentifierCharacter(
    char ch,
    std::string_view extraReserved,
    std::string_view allowedReserved)
{
    return isReservedIdentifierCharacter(ch, extraReserved) &&
           allowedReserved.find(ch) == std::string_view::npos;
}

/** Render a character for validation diagnostics without losing control characters. */
[[nodiscard]] std::string printableCharacter(char ch)
{
    auto const uch = static_cast<unsigned char>(ch);
    if (std::isprint(uch)) {
        return fmt::format("'{}'", ch);
    }
    return fmt::format("'\\x{:02X}'", uch);
}

}

std::string mapNameFromUri(const std::string& uri)
{
    std::string result = uri;

    // Convert to lowercase
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);

    // Replace special characters with dashes
    for (char& ch : result) {
        if (ch == ':' || ch == '\\' || ch == '/' || ch == '.') {
            ch = '-';
        }
    }

    // Merge successive dashes
    std::string::size_type pos;
    while ((pos = result.find("--")) != std::string::npos) {
        result.erase(pos, 1);
    }

    // Strip trailing "-openapi-json"
    const std::string suffix = "-openapi-json";
    if (result.size() >= suffix.size() && result.substr(result.size() - suffix.size()) == suffix) {
        result.resize(result.size() - suffix.size());
    }

    return result;
}

std::string generateNodeHexUuid()
{
    thread_local std::random_device rd;
    thread_local std::mt19937 rng(rd());
    std::stringstream ss;
    ss << std::hex;
    for (int i = 0; i < 4; ++i)
        ss << std::setw(4) << std::setfill('0') << (rng() & 0xFFFF);
    return ss.str();
}

std::string escapeIdentifierComponent(std::string_view input, std::string_view extraReserved)
{
    std::string result;
    result.reserve(input.size());
    for (char ch : input) {
        auto const uch = static_cast<unsigned char>(ch);
        if (ch == '%' || isReservedIdentifierCharacter(ch, extraReserved)) {
            // Percent escaping keeps delimiter-separated protocol strings
            // reversible without changing the underlying metadata value.
            fmt::format_to(std::back_inserter(result), FMT_STRING("%{:02X}"), uch);
        }
        else {
            result.push_back(ch);
        }
    }
    return result;
}

bool unescapeIdentifierComponent(std::string_view input, std::string& output, std::string* error)
{
    output.clear();
    output.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        char const ch = input[i];
        if (ch != '%') {
            output.push_back(ch);
            continue;
        }
        if (i + 2 >= input.size() || !isHexDigit(input[i + 1]) || !isHexDigit(input[i + 2])) {
            if (error) {
                *error = fmt::format("Malformed percent escape in identifier component '{}'.", input);
            }
            return false;
        }
        auto const decoded = static_cast<char>((hexValue(input[i + 1]) << 4U) | hexValue(input[i + 2]));
        output.push_back(decoded);
        i += 2;
    }
    return true;
}

void validateIdentifierName(
    std::string_view kind,
    std::string_view value,
    std::string_view extraReserved,
    std::string_view allowedReserved)
{
    for (char ch : value) {
        if (isForbiddenIdentifierCharacter(ch, extraReserved, allowedReserved)) {
            raise(fmt::format(
                "Invalid {} '{}': reserved character {} is not allowed.",
                kind,
                value,
                printableCharacter(ch)));
        }
    }
}

bool Version::isCompatible(const Version& other) const
{
    return other.major_ == major_ && other.minor_ == minor_;
}

std::string Version::toString() const
{
    return fmt::format("{}.{}.{}", major_, minor_, patch_);
}

bool Version::operator==(const Version& other) const
{
    return (
        std::tie(other.major_, other.minor_, other.patch_) ==
        std::tie(major_, minor_, patch_));
}

bool Version::operator<(const Version& other) const
{
    return (
        std::tie(other.major_, other.minor_, other.patch_) <
        std::tie(major_, minor_, patch_));
}

Version Version::fromJson(const nlohmann::json& j)
{
    try {
        return {
            j.at("major").get<uint16_t>(),
            j.at("minor").get<uint16_t>(),
            j.at("patch").get<uint16_t>()};
    }
    catch (nlohmann::json::out_of_range const& e) {
        throw missing_field(e.what(), "Version");
    }
}

nlohmann::json Version::toJson() const
{
    return nlohmann::json{{"major", major_}, {"minor", minor_}, {"patch", patch_}};
}

IdPart IdPart::fromJson(const nlohmann::json& j)
{
    try {
        auto idPart = j.at("partId").get<std::string>();
        validateIdentifierName("id-part label", idPart, ".");
        return {
            std::move(idPart),
            j.value("description", ""),
            j.value("datatype", IdPartDataType::I64),
            j.value("isSynthetic", false),
            j.value("isOptional", false)};
    }
    catch (nlohmann::json::out_of_range const& e) {
        throw missing_field(e.what(), "UniqueIdPart");
    }
}

nlohmann::json IdPart::toJson() const
{
    return nlohmann::json{
        {"partId", idPartLabel_},
        {"description", description_},
        {"datatype", datatype_},
        {"isSynthetic", isSynthetic_},
        {"isOptional", isOptional_}};
}

std::optional<uint32_t> IdPart::compositionMatchEndIndex(
    const std::vector<IdPart>& candidateComposition,
    uint32_t compositionMatchStartIdx,
    const KeyValueViewPairs& featureIdParts,
    size_t matchLength,
    std::string* error)
{
    auto featureIdIter = featureIdParts.begin();
    auto compositionIter = candidateComposition.begin();

    while (compositionMatchStartIdx > 0) {
        ++compositionIter;
        --compositionMatchStartIdx;
    }

    while (matchLength > 0 && compositionIter != candidateComposition.end()) {
        // Have we exhausted feature ID parts?
        if (featureIdIter == featureIdParts.end()) {
            return std::nullopt;
        }

        auto [idPartKey, idPartValue] = *featureIdIter;

        // Does this ID part's field name match?
        if (compositionIter->idPartLabel_ != idPartKey) {
            if (compositionIter->isOptional_) {
                // Optional composition slots may be absent entirely, so keep
                // scanning until we either find the requested part or hit a
                // required mismatch.
                ++compositionIter;
                continue;
            }
            return std::nullopt;
        }

        // Does the ID part's value match?
        if (!compositionIter->validate(idPartValue, error))
            return std::nullopt;

        ++featureIdIter;
        ++compositionIter;
        --matchLength;
    }

    if (matchLength != 0) {
        return std::nullopt;
    }

    return static_cast<uint32_t>(std::distance(candidateComposition.begin(), compositionIter));
}

bool IdPart::idPartsMatchComposition(
    const std::vector<IdPart>& candidateComposition,
    uint32_t compositionMatchStartIdx,
    const KeyValueViewPairs& featureIdParts,
    size_t matchLength,
    bool requireCompositionEnd,
    std::string* error)
{
    auto const matchEndIndex = compositionMatchEndIndex(
        candidateComposition,
        compositionMatchStartIdx,
        featureIdParts,
        matchLength,
        error);
    if (!matchEndIndex) {
        return false;
    }

    if (requireCompositionEnd) {
        auto compositionIter = candidateComposition.begin() + *matchEndIndex;
        while (compositionIter != candidateComposition.end()) {
            if (!compositionIter->isOptional_) {
                // Exact feature ids must consume every remaining required part
                // of the composition. Only optional tail fields may be omitted.
                return false;
            }
            ++compositionIter;
        }
    }

    return true;
}

bool IdPart::validate(std::variant<int64_t, std::string>& val, std::string* error) const
{
    if (std::holds_alternative<std::string>(val)) {
        auto& strVal = std::get<std::string>(val);
        auto result = std::variant<int64_t, std::string_view>(strVal);
        auto resultBool = validate(result, error);
        // Numeric id parts accept string input during parsing, but normalize to
        // integers once validation succeeds.
        if (std::holds_alternative<int64_t>(result)) {
            val = std::get<int64_t>(result);
        }
        return resultBool;
    }
    auto result = std::variant<int64_t, std::string_view>(std::get<int64_t>(val));
    return validate(result, error);
}

bool IdPart::validate(std::variant<int64_t, std::string_view>& val, std::string* error) const
{
    std::optional<int64_t> intVal;
    if (std::holds_alternative<std::string_view>(val)) {
        intVal = from_chars<int64_t>(std::get<std::string_view>(val));
    }
    else {
        intVal = std::get<int64_t>(val);
    }

    auto expectInteger = [this, &error, &intVal, &val](auto const minVal, auto const maxVal) -> bool {
        if (!intVal) {
            if (error)
                *error = fmt::format(
                    "Value '{}' for {} is not an integer!",
                    std::get<std::string_view>(val),
                    idPartLabel_);
            return false;
        }
        if (intVal < minVal) {
            if (error)
                *error = fmt::format("Value {} for {} is smaller than allowed ({}).", *intVal, idPartLabel_, minVal);
            return false;
        }
        if (intVal > maxVal) {
            if (error)
                *error = fmt::format("Value {} for {} is larger than allowed ({}).", *intVal, idPartLabel_, maxVal);
            return false;
        }
        val = *intVal;
        return true;
    };

    auto expectString = [this, &error, &val](auto const& validator) -> bool {
        if (!std::holds_alternative<std::string_view>(val)) {
            if (error)
                *error = fmt::format(
                    "Value for {} must be a string!",
                    idPartLabel_);
            return false;
        }
        return validator(std::get<std::string_view>(val), error);
    };

    auto expectUuid128 = [&expectString]() -> bool {
        return expectString([](auto const& strVal, auto error){
            // UUID128 ids are stored as the mapget-specific 16-character token,
            // not a hyphenated RFC 4122 string.
            if (strVal.size() != 16) {
                if (error)
                    *error = fmt::format("Value for {} must have 16 characters!", strVal);
                return false;
            }
            return true;
        });
    };

    switch (datatype_) {
    case IdPartDataType::I32:
        return expectInteger(INT32_MIN, INT32_MAX);
    case IdPartDataType::U32:
        return expectInteger(0, UINT32_MAX);
    case IdPartDataType::U64:
        return expectInteger(0, UINT64_MAX);
    case IdPartDataType::I64:
        return expectInteger(INT64_MIN, INT64_MAX);
    case IdPartDataType::UUID128:
        return expectUuid128();
    case IdPartDataType::STR:
        return expectString([](auto s, auto e){return true;});
    }

    if (error)
        *error = fmt::format("Part datatype {} is not supported.", static_cast<std::underlying_type_t<IdPartDataType>>(datatype_));
    return false;
}

FeatureTypeInfo FeatureTypeInfo::fromJson(const nlohmann::json& j)
{
    try {
        std::vector<std::vector<IdPart>> idCompositions;
        for (auto& item : j.at("uniqueIdCompositions")) {
            std::vector<IdPart> idParts;
            for (auto& idPart : item) {
                idParts.push_back(IdPart::fromJson(idPart));
            }
            idCompositions.push_back(idParts);
        }

        auto name = j.at("name").get<std::string>();
        validateIdentifierName("feature type name", name, ".");
        return {std::move(name), idCompositions};
    }
    catch (nlohmann::json::out_of_range const& e) {
        throw missing_field(e.what(), "FeatureTypeInfo");
    }
}

nlohmann::json FeatureTypeInfo::toJson() const
{
    std::vector<nlohmann::json> idCompositions;
    for (auto& item : uniqueIdCompositions_) {
        nlohmann::json idParts;
        for (auto& part : item) {
            idParts.push_back(part.toJson());
        }
        idCompositions.push_back(idParts);
    }

    return nlohmann::json{{"name", name_}, {"uniqueIdCompositions", idCompositions}};
}

Coverage Coverage::fromJson(const nlohmann::json& j)
{
    try {
        if (j.is_number_integer() || j.is_number_unsigned()) {
            // A bare integer is shorthand for a single covered packed tile.
            auto tileId = j.get<int32_t>();
            return {
                TileId::fromValue(tileId),
                TileId::fromValue(tileId),
                std::vector<bool>()
            };
        }
        return {
            TileId::fromValue(j.at("min").get<int32_t>()),
            TileId::fromValue(j.at("max").get<int32_t>()),
            j.value("filled", std::vector<bool>())};
    }
    catch (nlohmann::json::out_of_range const& e) {
        throw missing_field(e.what(), "Coverage");
    }
}

nlohmann::json Coverage::toJson() const
{
    if (min_ == max_ && filled_.empty())
        return min_.value();
    return nlohmann::json{{"min", min_.value()}, {"max", max_.value()}, {"filled", filled_}};
}

std::shared_ptr<LayerInfo> LayerInfo::fromJson(const nlohmann::json& j, std::string const& layerId)
{
    try {
        const auto type = j.value("type", LayerType::Features);

        std::vector<FeatureTypeInfo> featureTypes;
        if (type == LayerType::Features) {
            for (auto& item : j.at("featureTypes")) {
                featureTypes.push_back(FeatureTypeInfo::fromJson(item));
            }
        }

        std::vector<Coverage> coverages;
        if (j.contains("coverage"))
            for (auto& item : j.at("coverage")) {
                coverages.push_back(Coverage::fromJson(item));
            }

        const auto stages = std::max<uint32_t>(1U, j.value("stages", 1U));
        auto stageLabels = j.value("stageLabels", std::vector<std::string>{});
        if (stageLabels.size() < stages) {
            // Import pads missing labels so stage index -> label lookup remains
            // total even when metadata only names a few stages explicitly.
            stageLabels.reserve(stages);
            for (uint32_t i = static_cast<uint32_t>(stageLabels.size()); i < stages; ++i) {
                stageLabels.emplace_back(fmt::format("Stage {}", i));
            }
        }
        const auto defaultHighFidelityStage = stages > 1U ? 1U : 0U;
        const auto highFidelityStage = std::min<uint32_t>(
            stages - 1U,
            j.value("highFidelityStage", defaultHighFidelityStage));
        // High-fidelity stage is clamped into the configured stage range so
        // downstream geometry-name lookups never index past the metadata.

        auto result = std::make_shared<LayerInfo>();
        result->layerId_ = j.value("layerId", layerId);
        result->type_ = type;
        result->featureTypes_ = std::move(featureTypes);
        result->zoomLevels_ = j.value("zoomLevels", std::vector<int>());
        result->coverage_ = std::move(coverages);
        result->stages_ = stages;
        result->stageLabels_ = std::move(stageLabels);
        result->highFidelityStage_ = highFidelityStage;
        result->canRead_ = j.value("canRead", true);
        result->canWrite_ = j.value("canWrite", false);
        result->version_ = Version::fromJson(j.value("version", Version().toJson()));
        if (auto schemaIt = j.find("featureModelSchema"); schemaIt != j.end()) {
            result->featureModelSchema_ = LayerSchema::fromJsonSchema(*schemaIt);
        }
        result->validateIdentifiers();
        return result;
    }
    catch (nlohmann::json::out_of_range const& e) {
        throw missing_field(e.what(), "LayerInfo");
    }
}

nlohmann::json LayerInfo::toJson() const
{
    std::vector<nlohmann::json> featureTypes;
    featureTypes.reserve(featureTypes_.size());
    for (const auto& item : featureTypes_) {
        featureTypes.push_back(item.toJson());
    }

    std::vector<nlohmann::json> coverages;
    coverages.reserve(coverage_.size());
    for (const auto& item : coverage_) {
        coverages.push_back(item.toJson());
    }

    auto result = nlohmann::json{
        {"layerId", layerId_},
        {"type", type_},
        {"featureTypes", featureTypes},
        {"zoomLevels", zoomLevels_},
        {"coverage", coverages},
        {"stages", stages_},
        {"stageLabels", stageLabels_},
        {"highFidelityStage", highFidelityStage_},
        {"canRead", canRead_},
        {"canWrite", canWrite_},
        {"version", version_.toJson()}};

    if (featureModelSchema_) {
        result["featureModelSchema"] = featureModelSchema_->toJsonSchema();
    }

    return result;
}

std::shared_ptr<LayerSchema const> LayerInfo::layerSchema() const
{
    return featureModelSchema_;
}

void LayerInfo::validateIdentifiers() const
{
    validateIdentifierName("layer id", layerId_);
    for (auto const& featureType : featureTypes_) {
        validateIdentifierName("feature type name", featureType.name_, ".");
        for (auto const& composition : featureType.uniqueIdCompositions_) {
            for (auto const& idPart : composition) {
                validateIdentifierName("id-part label", idPart.idPartLabel_, ".");
            }
        }
    }
}

FeatureTypeInfo const* LayerInfo::getTypeInfo(const std::string_view& sv, bool throwIfMissing) const
{
    auto typeIt = std::find_if(
        featureTypes_.begin(),
        featureTypes_.end(),
        [&sv](auto&& tp) { return tp.name_ == sv; });
    if (typeIt != featureTypes_.end())
        return &*typeIt;
    if (throwIfMissing) {
        raise(fmt::format("Could not find feature type {}", sv));
    }
    return nullptr;
}

std::optional<uint8_t> LayerInfo::matchingFeatureIdCompositionIndex(
    const std::string_view& typeId,
    KeyValueViewPairs const& featureIdParts,
    bool validateForNewFeature) const
{
    auto typeInfo = getTypeInfo(typeId);

    for (uint32_t compositionIndex = 0;
         compositionIndex < typeInfo->uniqueIdCompositions_.size();
         ++compositionIndex) {
        auto const& candidateComposition = typeInfo->uniqueIdCompositions_[compositionIndex];
        if (IdPart::idPartsMatchComposition(
            candidateComposition,
            0,
            featureIdParts,
            featureIdParts.size(),
            true))
        {
            return static_cast<uint8_t>(std::min<uint32_t>(compositionIndex, 255));
        }

        // References may use alternative ID compositions,
        // but the feature itself must always use the first (primary) one.
        if (validateForNewFeature)
            // Once the primary composition failed, later alternatives are not
            // considered for concrete feature creation.
            return std::nullopt;
    }

    return std::nullopt;
}

bool LayerInfo::validFeatureId(
    const std::string_view& typeId,
    KeyValueViewPairs const& featureIdParts,
    bool validateForNewFeature) const
{
    return matchingFeatureIdCompositionIndex(typeId, featureIdParts, validateForNewFeature).has_value();
}

std::shared_ptr<LayerInfo> DataSourceInfo::getLayer(std::string const& layerId, bool throwIfMissing) const
{
    auto it = layers_.find(layerId);
    if (it != layers_.end())
        return it->second;
    if (throwIfMissing) {
        raise(
            fmt::format("Could not find layer '{}' in map '{}'", layerId, mapId_)
        );
    }
    return {};
}

DataSourceInfo DataSourceInfo::fromJson(const nlohmann::json& j)
{
    try {
        std::unordered_map<std::string, std::shared_ptr<LayerInfo>> layers;
        for (auto& item : j.at("layers").items()) {
            validateIdentifierName("layer id", item.key());
            layers[item.key()] = LayerInfo::fromJson(item.value(), item.key());
        }

        std::string nodeId;
        if (j.contains("nodeId"))
            nodeId = j.at("nodeId").get<std::string>();
        else
            // Datasource metadata may omit a stable node id for ad-hoc JSON
            // sources, so synthesize one to keep string-pool ownership valid.
            nodeId = generateNodeHexUuid();

        auto result = DataSourceInfo{
            nodeId,
            j.at("mapId").get<std::string>(),
            layers,
            j.value("maxParallelJobs", 8),
            j.value("addOn", false),
            j.value("extraJsonAttachment", nlohmann::json::object()),
            Version::fromJson(
                j.value("protocolVersion", TileLayerStream::CurrentProtocolVersion.toJson()))};
        result.validateIdentifiers();
        return result;
    }
    catch (nlohmann::json::out_of_range const& e) {
        throw missing_field(e.what(), "DataSourceInfo");
    }
}

nlohmann::json DataSourceInfo::toJson() const
{
    nlohmann::json layersJson;
    for (const auto& [layerName, layerPtr] : layers_) {
        layersJson[layerName] = layerPtr->toJson();
    }

    return nlohmann::json{
        {"nodeId", nodeId_},
        {"mapId", mapId_},
        {"layers", layersJson},
        {"maxParallelJobs", maxParallelJobs_},
        {"addOn", isAddOn_},
        {"extraJsonAttachment", extraJsonAttachment_},
        {"protocolVersion", effectiveDataSourceProtocolVersion(protocolVersion_).toJson()}};
}

void DataSourceInfo::validateIdentifiers() const
{
    validateIdentifierName("map id", mapId_, {}, "/");
    for (auto const& [layerId, layerInfo] : layers_) {
        validateIdentifierName("layer id", layerId);
        if (layerInfo) {
            layerInfo->validateIdentifiers();
        }
    }
}

KeyValueViewPairs castToKeyValueView(const KeyValuePairs& kvp)
{
    KeyValueViewPairs kvpView;
    for (auto const& [k, v] : kvp) {
        std::visit([&kvpView, &k](auto&& vv){
            if constexpr (std::is_same_v<std::decay_t<decltype(vv)>, std::string>)
               kvpView.emplace_back(k, std::string_view(vv));
            else
               kvpView.emplace_back(k, vv);
        }, v);
    }
    return kvpView;
}

KeyValuePairs castToKeyValue(const KeyValueViewPairs& kvpView)
{
    KeyValuePairs kvp;
    for (auto const& [k, v] : kvpView) {
        std::visit([&kvp, &k](auto&& vv){
            if constexpr (std::is_same_v<std::decay_t<decltype(vv)>, std::string_view>)
                kvp.emplace_back(k, std::string(vv));
            else
                kvp.emplace_back(k, vv);
        }, v);
    }
    return kvp;
}

KeyValueViewPairs castToKeyValueView(const KeyValuePairVec& kvp)
{
    KeyValueViewPairs kvpView;
    for (auto const& [k, v] : kvp) {
        std::visit([&kvpView, &k](auto&& vv){
            if constexpr (std::is_same_v<std::decay_t<decltype(vv)>, std::string>)
               kvpView.emplace_back(k, std::string_view(vv));
            else
               kvpView.emplace_back(k, vv);
        }, v);
    }
    return kvpView;
}

}
