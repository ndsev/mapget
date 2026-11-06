#pragma once

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <sfl/small_vector.hpp>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mapget
{
class MapSchema;

/**
 * The KeyValue(View)Pairs type is a vector of pairs, where each pair
 * consists of a string_view key and a variant value that can be
 * either an int64_t or a string_view. It is used as the interface-
 * type for feature id parts. By using sfl::small_vector instead of
 * std::vector, it is kept on the stack.
 */
using KeyValueViewPairs =
    sfl::small_vector<std::pair<std::string_view, std::variant<int64_t, std::string_view>>, 16>;
using KeyValuePairs =
    sfl::small_vector<std::pair<std::string, std::variant<int64_t, std::string>>, 16>;
using KeyValuePairVec = std::vector<std::pair<std::string, std::variant<int64_t, std::string>>>;

/** Enum to represent the possible data types */
enum class IdPartDataType { I32, U32, I64, U64, UUID128, STR };
NLOHMANN_JSON_SERIALIZE_ENUM(
    IdPartDataType,
    {
        {IdPartDataType::I32, "I32"},
        {IdPartDataType::U32, "U32"},
        {IdPartDataType::I64, "I64"},
        {IdPartDataType::U64, "U64"},
        {IdPartDataType::UUID128, "UUID128"},
        {IdPartDataType::STR, "STR"},
    })

/** Structure to represent a part of a feature id composition. */
struct IdPart
{
    /** Label/identifier for this ID part.
     * Unique under all ID parts of a feature. */
    std::string idPartLabel_;

    /** Description of the identifier. */
    std::string description_;

    /** Data type of the identifier. */
    IdPartDataType datatype_;

    /** Is the identifier synthetic or part of a map specification? */
    bool isSynthetic_ = false;

    /** Is the identifier optional in feature queries? */
    bool isOptional_ = false;

    /** Create IdPart from JSON. */
    static IdPart fromJson(const nlohmann::json& j);

    /** Serialize IdPart to JSON. */
    [[nodiscard]] nlohmann::json toJson() const;

    /**
     * Check whether the given value satisfies the constraints of this
     * IdPart specification. The value will be converted to an integer if provided
     * as a string, but not vice versa. Returns true and the correct converted value
     * written into val if the validation succeeds, false otherwise.
     */
    bool validate(std::variant<int64_t, std::string_view>& val, std::string* error = nullptr) const;
    bool validate(std::variant<int64_t, std::string>& val, std::string* error = nullptr) const;
};

/** Structure to represent the feature type info */
struct FeatureTypeInfo
{
    /** Name of the feature type */
    std::string name_;

    /**
     * List of allowed unique id compositions (each id composition is a list of id parts)
     * A single id composition must never have more than 16 parts.
     * The first unique id composition in the list is the primary, which must be
     * used by all features. Secondary compositions may only be used by references/relations.
     */
    std::vector<std::vector<IdPart>> uniqueIdCompositions_;

    /** Validate native or imported identity definitions without a JSON roundtrip. */
    void validate() const;
    /** Identity equality excludes prose descriptions, but preserves composition order. */
    [[nodiscard]] bool sameIdentity(FeatureTypeInfo const& other) const;

    /**
     * Deserializes a FeatureTypeInfo object from JSON.
     *
     * The JSON is expected to have the following structure:
     *
     * {
     *   "name": <string>,                  // Mandatory: The name of the feature type.
     *   "uniqueIdCompositions": [          // Mandatory: A list of unique ID compositions.
     *     [                                // A list of UniqueIdPart objects, each having the
     * following structure:
     *       {
     *         "partId": <string>,          // Mandatory: The ID of the unique ID part.
     *         "description": <string>,     // Optional: The description of the unique ID part.
     *         "datatype": <IdPartDataType>,// Optional: The data type of the unique ID part, must
     * be one of the enum values from IdPartDataType. Defaults to I64.
     *         TODO why "datatype"  and not "dataType" to match other fields?
     *         "isSynthetic": <bool>,       // Optional: A flag indicating if the unique ID part is
     * synthetic. Defaults to false. "isOptional": <bool>         // Optional: A flag indicating if
     * the unique ID part is optional. Defaults to false.
     *       },
     *       ...
     *     ],
     *     ...
     *   ]
     * }
     *
     * @param j The JSON to deserialize.
     * @return The deserialized FeatureTypeInfo object.
     * @throws std::runtime_error if any mandatory field is missing.
     */
    static FeatureTypeInfo fromJson(const nlohmann::json& j);

    /** Serialize FeatureTypeInfo to JSON. */
    [[nodiscard]] nlohmann::json toJson() const;
};

/** Borrowed canonical identity, with explicit ownership only at an escape boundary.
 * Definitions belong once to MapSchema, independently of any layer's production.
 */
class FeatureIdentity
{
public:
    class Composition;
    /** One canonical part, never a copied DTO or a pointer into a temporary view. */
    class Part
    {
    public:
        /** An invalid default part is distinct from an optional part. */
        Part() = default;
        /** Test whether this view identifies a declared part. */
        explicit operator bool() const noexcept { return owner_ != nullptr; }
        /** Return the exact declared label in the schema dictionary. */
        std::string_view name() const;
        /** Read the physical ID value encoding. */
        IdPartDataType datatype() const;
        /** Whether references may omit this part. */
        bool optional() const;
        /** Whether the producer synthesizes this part. */
        bool synthetic() const;
        /** Borrow descriptive text without adding it to a datasource dictionary. */
        std::string_view description() const;
        /** Export this canonical part without constructing a declaration DTO. */
        nlohmann::json toJson() const;
        /** Validate and normalize one borrowed value without materializing a declaration. */
        bool
        validateValue(std::variant<int64_t, std::string_view>& value, std::string* error = nullptr)
            const;
        /** Normalize an owning transport string only after successful validation. */
        bool validateValue(std::variant<int64_t, std::string>& value, std::string* error = nullptr)
            const;
        /** Pin the same schema when this part escapes its caller's owner lifetime. */
        Part retain() const;
        /** Compare canonical owner, array and element identity, not textual resemblance. */
        bool operator==(Part const& other) const noexcept;

    private:
        friend class FeatureIdentity;
        friend class Composition;
        Part(MapSchema const* owner, uint32_t parts, size_t index)
            : owner_(owner), parts_(parts), index_(index)
        {
        }
        MapSchema const* owner_ = nullptr;
        uint32_t parts_ = 0;
        size_t index_ = 0;
        std::shared_ptr<MapSchema const> retained_;
    };

    /** One ordered ID composition; a valid composition may contain zero parts. */
    class Composition
    {
    public:
        /** Default construction has no owner, unlike a declared empty composition. */
        Composition() = default;
        /** Test identity, not part count. */
        explicit operator bool() const noexcept { return owner_ != nullptr; }
        /** Return borrowed part values; keep this composition or the schema alive. */
        auto parts() const
        {
            return std::views::iota(size_t{0}, partCount()) |
                std::views::transform([self = *this](size_t index) { return self.partAt(index); });
        }
        /** Match a prefix/suffix and report its ending composition slot. */
        std::optional<uint32_t> matchEndIndex(
            uint32_t start,
            KeyValueViewPairs const& values,
            size_t length,
            std::string* error = nullptr) const;
        /** Match values, optionally requiring all remaining non-optional parts. */
        bool matches(
            uint32_t start,
            KeyValueViewPairs const& values,
            size_t length,
            bool requireEnd,
            std::string* error = nullptr) const;
        /** Pin the same schema when this composition escapes. */
        Composition retain() const;
        /** Compare canonical identity within the exact owner. */
        bool operator==(Composition const& other) const noexcept;

    private:
        friend class FeatureIdentity;
        Composition(MapSchema const* owner, uint32_t parts) : owner_(owner), parts_(parts) {}
        size_t partCount() const;
        Part partAt(size_t index) const;
        MapSchema const* owner_ = nullptr;
        uint32_t parts_ = 0;
        std::shared_ptr<MapSchema const> retained_;
    };

    /** Concrete features use only their primary composition; references may use alternatives. */
    enum class MatchMode { PrimaryOnly, AnyComposition };

    /** An invalid default identity is the nonthrowing lookup result for an unknown name. */
    FeatureIdentity() = default;
    /** Test presence without conflating identity with a layer's production. */
    explicit operator bool() const noexcept { return owner_ != nullptr; }
    /** Return the authoritative feature type spelling. */
    std::string_view name() const;
    /** Return borrowed composition values in declaration order, primary first. */
    auto compositions() const
    {
        return std::views::iota(size_t{0}, compositionCount()) |
            std::views::transform([self = *this](size_t index)
                                  { return self.compositionAt(index); });
    }
    /** Find an exact matching composition; production checks belong to LayerInfo. */
    std::optional<uint8_t> matchingCompositionIndex(
        KeyValueViewPairs const& values,
        MatchMode mode = MatchMode::AnyComposition,
        std::string* error = nullptr) const;
    /** Compare semantic declarations across owners, excluding prose descriptions. */
    bool sameIdentity(FeatureIdentity const& other) const;
    /** Compare a new input declaration without allocating a reconstructed DTO. */
    bool sameIdentity(FeatureTypeInfo const& declaration) const;
    /** Materialize only at an explicit legacy declaration boundary. */
    FeatureTypeInfo toDeclaration() const;
    /** Export the canonical declaration directly for documentation/legacy transport. */
    nlohmann::json toJson() const;
    /** Pin the existing schema owner at an escape boundary. */
    FeatureIdentity retain() const;
    /** Drop ownership while preserving exact identity; the caller keeps the owner alive. */
    FeatureIdentity borrow() const noexcept { return {owner_, index_}; }
    /** Borrow the owner without touching its reference count. */
    MapSchema const* pool() const noexcept { return owner_; }
    /** Equality means the same canonical row in the same schema, not semantic equivalence. */
    bool operator==(FeatureIdentity const& other) const noexcept
    {
        return owner_ == other.owner_ && index_ == other.index_;
    }

private:
    friend class MapSchema;
    FeatureIdentity(MapSchema const* owner, size_t index) : owner_(owner), index_(index) {}
    size_t compositionCount() const;
    Composition compositionAt(size_t index) const;
    MapSchema const* owner_ = nullptr;
    size_t index_ = 0;
    std::shared_ptr<MapSchema const> retained_;
};

}  // namespace mapget
