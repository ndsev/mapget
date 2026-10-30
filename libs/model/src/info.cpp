#include "info.h"
#include <picosha2.h>
#include <bit>
#include "layerschema.h"
#include "mapget/log.h"
#include "mapschema.h"
#include "stream.h"

#include <tuple>
#include <random>
#include <sstream>
#include <charconv>
#include <cctype>
#include <regex>
#include <set>

namespace mapget
{

MapFormat MapFormat::fromJson(nlohmann::json const& json)
{
    if (!json.is_object())
        throw std::invalid_argument("mapFormat must be an object.");
    if (json.contains("profiles"))
        throw std::invalid_argument(
            "mapFormat uses a flat subsystem name/version map, not profiles.");
    MapFormat result;
    result.name_ = json.value("name", std::string{});
    result.version_ = json.value("version", std::string{});
    if (auto it = json.find("subsystems"); it != json.end()) {
        if (!it->is_object())
            throw std::invalid_argument("mapFormat.subsystems must contain name/version pairs.");
        for (auto const& [name, version] : it->items()) {
            if (name.empty() || !version.is_string())
                throw std::invalid_argument(
                    "Map subsystem names must be nonempty and versions must be strings.");
            result.subsystems_.emplace(name, version.get<std::string>());
        }
    }
    if (result.name_.empty() && (!result.version_.empty() || !result.subsystems_.empty()))
        throw std::invalid_argument("A versioned map format requires a name.");
    return result;
}

nlohmann::json MapFormat::toJson() const
{
    return {{"name", name_}, {"version", version_}, {"subsystems", subsystems_}};
}

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
    // U64 nodes deliberately project through simfil's signed scalar storage.
    // Decimal transport still parses the full unsigned domain without overflow.
    if (datatype_ == IdPartDataType::U64) {
        if (auto text = std::get_if<std::string_view>(&val)) {
            auto parsed = from_chars<uint64_t>(*text);
            if (!parsed) {
                if (error)
                    *error = "Expected an unsigned 64-bit decimal ID.";
                return false;
            }
            val = std::bit_cast<int64_t>(*parsed);
        }
        return true;
    }
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

std::shared_ptr<LayerInfo> LayerInfo::fromJson(
    const nlohmann::json& j,
    std::string const& layerId,
    std::shared_ptr<MapSchema> const& schemaBuilder)
{
    auto result = fromJsonMetadata(j, layerId);
    std::vector<FeatureTypeInfo> featureTypes;
    for (auto const& item : j.value("featureTypes", nlohmann::json::array()))
        featureTypes.push_back(FeatureTypeInfo::fromJson(item));
    auto owner = schemaBuilder ? schemaBuilder : std::make_shared<MapSchema>();
    // Standalone legacy layers, including untyped SourceData, still need an installed
    // open contract. Canonical catalog parsing uses fromJsonMetadata() instead.
    (void)owner->importLayer(
        result->layerId_,
        result->type_,
        j.value("featureModelSchema", nlohmann::json{}),
        featureTypes);
    if (!schemaBuilder) {
        owner->finalize();
        result->schemaOwner_ = std::move(owner);
    }
    return result;
}

std::shared_ptr<LayerInfo>
LayerInfo::fromJsonMetadata(nlohmann::json const& j, std::string const& layerId)
{
    try {
        const auto type = j.value("type", LayerType::Features);

        std::vector<Coverage> coverages;
        if (j.contains("coverage"))
            for (auto& item : j.at("coverage")) {
                coverages.push_back(Coverage::fromJson(item));
            }

        auto result = std::make_shared<LayerInfo>();
        result->layerId_ = j.value("layerId", layerId);
        result->type_ = type;
        auto kind = j.value("partitionKind", std::string("tile"));
        if (kind != "tile" && kind != "object")
            throw std::invalid_argument("Unknown layer partitionKind.");
        result->partitionKind_ = kind == "object" ? PartitionKind::Object : PartitionKind::Tile;
        if (j.contains("tileAssociationLevel")) {
            auto const& level = j.at("tileAssociationLevel");
            if (!level.is_number_integer() || level < 0 || level > 15)
                throw std::invalid_argument("tileAssociationLevel must be an integer in 0..15.");
            result->tileAssociationLevel_ = level.get<int>();
        }
        result->zoomLevels_ = j.value("zoomLevels", std::vector<int>());
        result->coverage_ = std::move(coverages);
        result->canRead_ = j.value("canRead", true);
        result->canWrite_ = j.value("canWrite", false);
        if (j.contains("defaultVisible")) result->defaultVisible_ = j.at("defaultVisible").get<bool>();
        result->version_ = Version::fromJson(j.value("version", Version().toJson()));
        result->validateIdentifiers();
        return result;
    }
    catch (nlohmann::json::out_of_range const& e) {
        throw missing_field(e.what(), "LayerInfo");
    }
}

nlohmann::json LayerInfo::toJson() const
{
    std::vector<nlohmann::json> coverages;
    coverages.reserve(coverage_.size());
    for (const auto& item : coverage_) {
        coverages.push_back(item.toJson());
    }

    auto result = nlohmann::json{
        {"layerId", layerId_},
        {"type", type_},
        {"zoomLevels", zoomLevels_},
        {"coverage", coverages},
        {"canRead", canRead_},
        {"canWrite", canWrite_},
        {"version", version_.toJson()}};

    result["partitionKind"] = partitionKind_ == PartitionKind::Object ? "object" : "tile";
    if (tileAssociationLevel_)
        result["tileAssociationLevel"] = *tileAssociationLevel_;
    if (defaultVisible_)
        result["defaultVisible"] = *defaultVisible_;
    return result;
}

MemoryUsageBreakdown LayerInfo::memoryUsage() const
{
    MemoryUsageBreakdown result;
    result.add("object", {sizeof(LayerInfo), sizeof(LayerInfo)});
    result.add("layer-id", stringMemoryUsage(layerId_));
    result.add("zoom-levels", vectorMemoryUsage(zoomLevels_));
    result.add("coverage", vectorMemoryUsage(coverage_));
    for (auto const& coverage : coverage_) {
        auto const logicalBytes = (coverage.filled_.size() + 7) / 8;
        auto const allocatedBytes = (coverage.filled_.capacity() + 7) / 8;
        result.add("coverage-bits", {logicalBytes, allocatedBytes});
    }
    // The service accounts unique MapSchema owners once, not once for each layer/source.
    return result;
}

simfil::model_ptr<LayerSchema> LayerInfo::layerSchema() const
{
    return schemaOwner_ ? schemaOwner_->layer(layerId_) : simfil::model_ptr<LayerSchema>{};
}

std::shared_ptr<MapSchema const> LayerInfo::schemaOwner() const
{
    return schemaOwner_;
}

std::span<FeatureTypeInfo const> LayerInfo::featureTypes() const
{
    auto schema = layerSchema();
    if (!schema)
        throw std::logic_error("Layer metadata has no installed MapSchema: " + layerId_);
    return schema->featureTypeInfos();
}

void LayerInfo::validateIdentifiers() const
{
    if (partitionKind_ != PartitionKind::Tile && partitionKind_ != PartitionKind::Object)
        throw std::invalid_argument("Unknown layer partition kind.");
    if (partitionKind_ == PartitionKind::Object &&
        (!tileAssociationLevel_ || *tileAssociationLevel_ < 0 || *tileAssociationLevel_ > 15))
        throw std::invalid_argument("Object layer requires tileAssociationLevel in 0..15.");
    if (partitionKind_ == PartitionKind::Tile && tileAssociationLevel_)
        throw std::invalid_argument("Tile layers do not have an object association level.");
    validateIdentifierName("layer id", layerId_);
    if (!schemaOwner_)
        return;  // Catalog metadata can be parsed before local schema installation.
    for (auto const& featureType : featureTypes()) {
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
    auto schema = layerSchema();
    if (!schema)
        throw std::logic_error("Layer metadata has no installed MapSchema: " + layerId_);
    return schema->getTypeInfo(sv, throwIfMissing);
}

std::optional<uint8_t> LayerInfo::matchingFeatureIdCompositionIndex(
    const std::string_view& typeId,
    KeyValueViewPairs const& featureIdParts,
    bool validateForNewFeature) const
{
    auto typeInfo = getTypeInfo(typeId);
    if (validateForNewFeature && !layerSchema()->producesFeatureType(typeId))
        return std::nullopt;

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

DataSourceInfo DataSourceInfo::fromJson(const nlohmann::json& j, SchemaResolveFun const& schemas)
{
    try {
        auto format = MapFormat::fromJson(j.value("mapFormat", nlohmann::json::object()));
        auto schemaId = j.value("schemaId", std::string{});
        // Hash only schema-producing inputs. Runtime names, credentials, coverage and
        // dictionary identity must neither duplicate graphs nor enter a cache key.
        picosha2::hash256_one_by_one hash;
        auto hashValue = [&](nlohmann::json const& value)
        {
            auto bytes = value.dump();
            hash.process(bytes.begin(), bytes.end());
            const char separator = '\0';
            hash.process(&separator, &separator + 1);
        };
        if (schemaId.empty())
            hashValue(format.toJson());
        std::unordered_map<std::string, std::shared_ptr<LayerInfo>> layers;
        for (auto& item : j.at("layers").items()) {
            validateIdentifierName("layer id", item.key());
            if (!schemaId.empty() &&
                (item.value().contains("featureTypes") ||
                 item.value().contains("featureModelSchema")))
                throw std::invalid_argument(
                    "Schema references and inline legacy declarations must not be mixed.");
            layers[item.key()] = LayerInfo::fromJsonMetadata(item.value(), item.key());
            if (schemaId.empty()) {
                hashValue(item.key());
                hashValue(item.value().value("type", LayerType::Features));
                hashValue(item.value().value("featureTypes", nlohmann::json::array()));
                hashValue(item.value().value("featureModelSchema", nlohmann::json{}));
            }
            if (layers[item.key()]->layerId_ != item.key())
                throw std::invalid_argument("Layer metadata key and layerId must agree.");
        }

        std::string stringPoolId;
        if (j.contains("stringPoolId"))
            stringPoolId = j.at("stringPoolId").get<std::string>();
        else
            // Datasource metadata may omit a stable node id for ad-hoc JSON
            // sources, so synthesize one to keep string-pool ownership valid.
            stringPoolId = generateNodeHexUuid();

        DataSourceInfo result;
        result.stringPoolId_ = std::move(stringPoolId);
        result.mapId_ = j.at("mapId").get<std::string>();
        result.layers_ = std::move(layers);
        result.maxParallelJobs_ = j.value("maxParallelJobs", 8);
        result.isAddOn_ = j.value("addOn", false);
        result.extraJsonAttachment_ = j.value("extraJsonAttachment", nlohmann::json::object());
        result.protocolVersion_ = Version::fromJson(
            j.value("protocolVersion", TileLayerStream::CurrentProtocolVersion.toJson()));
        result.mapFormat_ = std::move(format);
        result.schemaId_ = std::move(schemaId);
        if (result.schemaId_.empty()) {
            hash.finish();
            auto cacheKey = "mapget-legacy:" + picosha2::get_hash_hex_string(hash);
            // This synchronous import has no cancellation callback: j remains alive
            // until the shared builder completes, including when construction fails.
            auto schema = MapSchema::getOrBuild(
                std::move(cacheKey),
                [&j, format = result.mapFormat_]
                {
                    auto owner = std::make_shared<MapSchema>(format);
                    for (auto const& [name, layer] : j.at("layers").items())
                        (void)LayerInfo::fromJson(layer, name, owner);
                    owner->finalize();
                    return owner;
                });
            result.setMapSchema(std::move(schema));
        }
        else if (schemas) {
            auto schema = schemas(result.schemaId_);
            if (!schema)
                throw std::invalid_argument("Schema is not installed: " + result.schemaId_);
            result.setMapSchema(std::move(schema));
        }
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
        {"stringPoolId", stringPoolId_},
        {"mapId", mapId_},
        {"layers", layersJson},
        {"maxParallelJobs", maxParallelJobs_},
        {"addOn", isAddOn_},
        {"mapFormat", mapFormat_.toJson()},
        {"schemaId", schemaId_},
        {"extraJsonAttachment", extraJsonAttachment_},
        {"protocolVersion", effectiveDataSourceProtocolVersion(protocolVersion_).toJson()}};
}

void DataSourceInfo::setMapSchema(std::shared_ptr<MapSchema const> schema)
{
    if (!schema || !schema->finalized())
        throw std::invalid_argument("DataSourceInfo requires a finalized MapSchema.");
    if (!schemaId_.empty() && schemaId_ != schema->schemaId())
        throw std::invalid_argument("MapSchema identity does not match the advertised schemaId.");
    if (mapFormat_ != MapFormat{} && mapFormat_ != schema->format())
        throw std::invalid_argument("MapSchema format does not match the advertised mapFormat.");

    // Build privately so one bad layer cannot leave half the source rebound, nor mutate
    // LayerInfos still held by a previous published source incarnation.
    decltype(layers_) boundLayers;
    boundLayers.reserve(layers_.size());
    for (auto const& [name, metadata] : layers_) {
        auto layer = schema->layer(name);
        if (!metadata || metadata->layerId_ != name || !layer || layer->name() != name ||
            layer->layerType() != metadata->type_)
            throw std::invalid_argument("MapSchema has no matching layer contract: " + name);
        auto bound = std::make_shared<LayerInfo>(*metadata);
        bound->schemaOwner_ = schema;
        bound->validateIdentifiers();
        boundLayers.emplace(name, std::move(bound));
    }
    auto format = schema->format();
    std::string id(schema->schemaId());
    layers_ = std::move(boundLayers);
    mapFormat_ = std::move(format);
    schemaId_ = std::move(id);
    mapSchema_ = std::move(schema);
}

void DataSourceInfo::validateSchema() const
{
    if (!mapSchema_ || !mapSchema_->finalized() || schemaId_ != mapSchema_->schemaId() ||
        mapFormat_ != mapSchema_->format())
        throw std::invalid_argument("Datasource has no complete matching MapSchema: " + mapId_);
    for (auto const& [name, layer] : layers_) {
        auto definition = mapSchema_->layer(name);
        if (!layer || layer->layerId_ != name || layer->schemaOwner_ != mapSchema_ || !definition ||
            definition->layerType() != layer->type_)
            throw std::invalid_argument(
                "Datasource has an incomplete layer schema binding: " + name);
    }
}

MemoryUsageBreakdown DataSourceInfo::memoryUsage() const
{
    MemoryUsageBreakdown result;
    result.add("object", {sizeof(DataSourceInfo), sizeof(DataSourceInfo)});
    result.add("string-pool-id", stringMemoryUsage(stringPoolId_));
    result.add("map-id", stringMemoryUsage(mapId_));
    result.add("schema-id", stringMemoryUsage(schemaId_));
    result.add("format", stringMemoryUsage(mapFormat_.name_));
    result.add("format", stringMemoryUsage(mapFormat_.version_));
    result.add(
        "format",
        {mapFormat_.subsystems_.size() * sizeof(decltype(mapFormat_.subsystems_)::value_type),
         mapFormat_.subsystems_.size() *
             (sizeof(decltype(mapFormat_.subsystems_)::value_type) + 3 * sizeof(void*))});
    for (auto const& [name, version] : mapFormat_.subsystems_) {
        result.add("format", stringMemoryUsage(name));
        result.add("format", stringMemoryUsage(version));
    }
    result.add("layers-index", {
        layers_.size() * sizeof(decltype(layers_)::value_type),
        layers_.bucket_count() * sizeof(void*) +
            layers_.size() * (sizeof(decltype(layers_)::value_type) + 2 * sizeof(void*)),
    });
    for (auto const& [layerId, layer] : layers_) {
        result.add("layer-index-keys", stringMemoryUsage(layerId));
        if (layer) {
            result.merge("layer." + layerId, layer->memoryUsage());
        }
    }
    result.add("extra-json", jsonMemoryUsage(extraJsonAttachment_));
    return result;
}

void DataSourceInfo::validateIdentifiers() const
{
    validateIdentifierName("map id", mapId_, {}, "/");
    for (auto const& [layerId, layerInfo] : layers_) {
        validateIdentifierName("layer id", layerId);
        if (!layerInfo || layerInfo->layerId_ != layerId)
            throw std::invalid_argument("Datasource layer key and metadata must agree: " + layerId);
        layerInfo->validateIdentifiers();
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
