#include "subsetlayer.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <set>
#include <sstream>
#include <type_traits>
#include <utility>
#include <variant>

#include <bitsery/adapter/buffer.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/bitsery.h>
#include <bitsery/ext/std_bitset.h>
#include <bitsery/ext/std_optional.h>
#include <bitsery/traits/string.h>
#include <bitsery/traits/vector.h>

#include "featurelayer.h"
#include "mapget/log.h"
#include "pointnode.h"
#include "simfil/model/bitsery-traits.h"
#include "sourcedatareference.h"
#include "stringpool.h"

namespace bitsery
{

template <typename S>
void serialize(S& s, glm::vec3& value)
{
    s.value4b(value.x);
    s.value4b(value.y);
    s.value4b(value.z);
}

} // namespace bitsery

namespace mapget
{
namespace
{

struct DependencyWire
{
    std::string sourceTileKey_;
    uint32_t sourceFeatureCount_ = 0;
};

struct IssueWire
{
    std::string channelId_;
    std::string expression_;
    Scope scope_ = Scope::Feature;
    std::string message_;
    uint64_t occurrenceCount_ = 1;
};

simfil::ArrayIndex idPartValuesToArrayIndex(
    TileSubsetLayer& layer,
    std::vector<IdPart> const& composition,
    KeyValueViewPairs const& idParts)
{
    auto values = layer.newArray(std::max<size_t>(1, composition.size()), true);
    auto part = idParts.begin();
    for (auto const& definition : composition) {
        if (part != idParts.end() && definition.idPartLabel_ == part->first) {
            values->append(std::visit(
                [&](auto&& value) -> simfil::ModelNode::Ptr {
                    return layer.newValue(value);
                },
                part->second));
            ++part;
            continue;
        }
        if (!definition.isOptional_) {
            raiseFmt(
                "Missing non-optional ID part '{}' while materializing a subset feature ID.",
                definition.idPartLabel_);
        }
        values->append(layer.resolve<simfil::ModelNode>(
            {simfil::Model::Null, 1},
            simfil::ScalarValueType{}));
    }
    if (part != idParts.end()) {
        raiseFmt(
            "Unexpected trailing ID part '{}' while materializing a subset feature ID.",
            part->first);
    }
    return static_cast<simfil::ArrayIndex>(values->addr().index());
}

nlohmann::json arrayToJson(model_ptr<Array> const& array)
{
    auto result = nlohmann::json::array();
    if (!array) {
        return result;
    }
    for (uint32_t index = 0; index < array->size(); ++index) {
        auto value = array->at(index);
        result.push_back(value ? value->toJson() : nlohmann::json());
    }
    return result;
}

nlohmann::json diagnosticsToJson(simfil::Diagnostics const& diagnostics)
{
    auto result = nlohmann::json::array();
    auto messages = simfil::diagnostics(diagnostics);
    if (!messages) {
        return result;
    }
    for (auto const& message : *messages) {
        auto item = nlohmann::json::object({
            {"message", message.message},
            {"location", {
                {"offset", message.location.offset},
                {"size", message.location.size},
            }},
        });
        if (message.fix) {
            item["fix"] = *message.fix;
        }
        result.push_back(std::move(item));
    }
    return result;
}

int64_t traceCounterToInt64(uint64_t value)
{
    auto const max = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    return value > max ? std::numeric_limits<int64_t>::max() : static_cast<int64_t>(value);
}

template<typename S>
void readWriteDiagnostics(S& serializer, simfil::Diagnostics& data)
{
    serializer.container(
        data.exprIndex_,
        std::numeric_limits<uint16_t>::max(),
        [](auto& nested, uint32_t& value) {
            nested.value4b(value);
        });
    serializer.container(
        data.fieldData_,
        std::numeric_limits<uint16_t>::max(),
        [](auto& nested, simfil::Diagnostics::FieldExprData& value) {
            nested.value4b(value.location.offset);
            nested.value4b(value.location.size);
            nested.value4b(value.hits);
            nested.value4b(value.evaluations);
            nested.text1b(value.name, 0xff);
        });
    serializer.container(
        data.comparisonData_,
        std::numeric_limits<uint16_t>::max(),
        [](auto& nested, simfil::Diagnostics::ComparisonExprData& value) {
            nested.value4b(value.location.offset);
            nested.value4b(value.location.size);
            nested.ext(value.leftTypes.flags, bitsery::ext::StdBitset{});
            nested.ext(value.rightTypes.flags, bitsery::ext::StdBitset{});
            nested.value4b(value.evaluations);
            nested.value4b(value.trueResults);
            nested.value4b(value.falseResults);
        });
}

template<typename S>
void readWriteDependencyWire(S& serializer, DependencyWire& dependency)
{
    serializer.text1b(
        dependency.sourceTileKey_,
        std::numeric_limits<uint32_t>::max());
    serializer.value4b(dependency.sourceFeatureCount_);
}

template<typename S>
void readWriteIssueWire(S& serializer, IssueWire& issue)
{
    serializer.text1b(issue.channelId_, std::numeric_limits<uint32_t>::max());
    serializer.text1b(issue.expression_, std::numeric_limits<uint32_t>::max());
    serializer.value1b(issue.scope_);
    serializer.text1b(issue.message_, std::numeric_limits<uint32_t>::max());
    serializer.value8b(issue.occurrenceCount_);
}

model_ptr<Array> arrayAt(TileSubsetLayer const& layer, simfil::ArrayIndex index)
{
    if (index == simfil::InvalidArrayIndex) {
        return {};
    }
    return layer.resolve<Array>({
        simfil::ModelPool::ColumnId::Arrays,
        static_cast<uint32_t>(index),
    });
}

std::string nodeStringValue(simfil::ModelNode::Ptr const& node)
{
    if (!node) {
        return {};
    }
    auto value = node->value();
    if (auto string = std::get_if<std::string>(&value)) {
        return *string;
    }
    if (auto string = std::get_if<std::string_view>(&value)) {
        return std::string(*string);
    }
    return node->toJson().dump();
}

bool arrayContainsAddress(
    model_ptr<Array> const& array,
    simfil::ModelNodeAddress address)
{
    if (!array) {
        return false;
    }
    for (uint32_t index = 0; index < array->size(); ++index) {
        auto node = array->at(index);
        if (node && node->addr() == address) {
            return true;
        }
    }
    return false;
}

} // namespace

FilterTrace::FilterTrace(
    Data* data,
    simfil::ModelConstPtr pool,
    simfil::ModelNodeAddress address,
    simfil::detail::mp_key key)
    : simfil::ProceduralObject<4, FilterTrace, TileSubsetLayer>(
          std::move(pool),
          address,
          key),
      data_(data)
{
    fields_.emplace_back(StringPool::NameStr, [](FilterTrace const& self) {
        return self.model().resolve(self.data_->name_);
    });
    fields_.emplace_back(StringPool::CallsStr, [](FilterTrace const& self) {
        return self.model().newValue(traceCounterToInt64(self.data_->calls_));
    });
    fields_.emplace_back(StringPool::TotalUsStr, [](FilterTrace const& self) {
        return self.model().newValue(self.data_->totalUs_);
    });
    fields_.emplace_back(StringPool::ValuesStr, [](FilterTrace const& self) {
        return self.values();
    });
}

std::string FilterTrace::name() const
{
    return nodeStringValue(model().resolve(data_->name_));
}

uint64_t FilterTrace::calls() const
{
    return data_->calls_;
}

std::chrono::microseconds FilterTrace::totalUs() const
{
    return std::chrono::microseconds{data_->totalUs_};
}

model_ptr<Array> FilterTrace::values() const
{
    return data_->values_ ? model().resolve<Array>(data_->values_) : model_ptr<Array>{};
}

nlohmann::json FilterTrace::toJson() const
{
    return {
        {"type", "FilterTrace"},
        {"name", name()},
        {"calls", calls()},
        {"totalus", totalUs().count()},
        {"values", arrayToJson(values())},
    };
}

FeatureEntry::FeatureEntry(
    Data* data,
    simfil::ModelConstPtr pool,
    simfil::ModelNodeAddress address,
    simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileSubsetLayer>(
          std::move(pool),
          address,
          key),
      data_(data)
{
}

model_ptr<FeatureId> FeatureEntry::featureId() const
{
    return model().resolve<FeatureId>(data_->featureId_);
}

model_ptr<GeometryCollection> FeatureEntry::geometry() const
{
    return model().resolve<GeometryCollection>(data_->geometry_);
}

model_ptr<Array> FeatureEntry::values() const
{
    return model().resolve<Array>(data_->values_);
}

nlohmann::json FeatureEntry::toJson() const
{
    return {
        {"type", "FeatureEntry"},
        {"featureId", featureId() ? featureId()->toString() : ""},
        {"geometry", geometry() ? geometry()->toJson() : nlohmann::json()},
        {"values", arrayToJson(values())},
    };
}

simfil::ValueType FeatureEntry::type() const
{
    return simfil::ValueType::Object;
}

simfil::ModelNode::Ptr FeatureEntry::at(int64_t index) const
{
    return get(keyAt(index));
}

uint32_t FeatureEntry::size() const
{
    return 3;
}

simfil::ModelNode::Ptr FeatureEntry::get(simfil::StringId const& field) const
{
    if (field == StringPool::FeatureIdStr) return featureId();
    if (field == StringPool::GeometryStr) return geometry();
    if (field == StringPool::ValuesStr) return values();
    return {};
}

simfil::StringId FeatureEntry::keyAt(int64_t index) const
{
    switch (index) {
    case 0: return StringPool::FeatureIdStr;
    case 1: return StringPool::GeometryStr;
    case 2: return StringPool::ValuesStr;
    default: return simfil::StringPool::Empty;
    }
}

bool FeatureEntry::iterate(IterCallback const& callback) const
{
    for (uint32_t index = 0; index < size(); ++index) {
        if (auto value = at(index); value && !callback(*value)) {
            return false;
        }
    }
    return true;
}

AttributeValidityEntry::AttributeValidityEntry(
    Data* data,
    simfil::ModelConstPtr pool,
    simfil::ModelNodeAddress address,
    simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileSubsetLayer>(
          std::move(pool),
          address,
          key),
      data_(data)
{
}

model_ptr<FeatureId> AttributeValidityEntry::featureId() const
{
    return model().resolve<FeatureId>(data_->featureId_);
}

model_ptr<GeometryCollection> AttributeValidityEntry::geometry() const
{
    return model().resolve<GeometryCollection>(data_->geometry_);
}

model_ptr<Array> AttributeValidityEntry::hostValues() const
{
    return model().resolve<Array>(data_->hostValues_);
}

model_ptr<Array> AttributeValidityEntry::values() const
{
    return model().resolve<Array>(data_->values_);
}

std::optional<std::string> AttributeValidityEntry::attributeLayer() const
{
    if (!data_->attributeLayer_) {
        return std::nullopt;
    }
    return nodeStringValue(model().resolve(data_->attributeLayer_));
}

std::optional<std::string> AttributeValidityEntry::attributeName() const
{
    if (!data_->attributeName_) {
        return std::nullopt;
    }
    return nodeStringValue(model().resolve(data_->attributeName_));
}

std::optional<uint32_t> AttributeValidityEntry::attributeIndex() const
{
    return data_->attributeIndex_ == InvalidAttributeIndex
        ? std::nullopt
        : std::optional<uint32_t>{data_->attributeIndex_};
}

bool AttributeValidityEntry::hasValidity() const
{
    return data_->hasValidity_;
}

uint32_t AttributeValidityEntry::validityIndex() const
{
    return data_->validityIndex_;
}

uint32_t AttributeValidityEntry::validityCount() const
{
    return data_->validityCount_;
}

AttributeValidityEntry::GeometryDescriptionType
AttributeValidityEntry::geometryDescriptionType() const
{
    return data_->geometryDescriptionType_;
}

bool AttributeValidityEntry::isFeatureTransition() const
{
    return geometryDescriptionType() == ValidityData::FeatureTransition;
}

model_ptr<FeatureId> AttributeValidityEntry::transitionFromFeatureId() const
{
    return isFeatureTransition()
        ? model().resolve<FeatureId>(data_->transitionFromFeatureId_)
        : model_ptr<FeatureId>{};
}

model_ptr<FeatureId> AttributeValidityEntry::transitionToFeatureId() const
{
    return isFeatureTransition()
        ? model().resolve<FeatureId>(data_->transitionToFeatureId_)
        : model_ptr<FeatureId>{};
}

std::optional<AttributeValidityEntry::TransitionEnd>
AttributeValidityEntry::transitionFromConnectedEnd() const
{
    if (!isFeatureTransition()) {
        return std::nullopt;
    }
    return (data_->transitionConnectedEnds_ & 0x1U) != 0
        ? ValidityData::End
        : ValidityData::Start;
}

std::optional<AttributeValidityEntry::TransitionEnd>
AttributeValidityEntry::transitionToConnectedEnd() const
{
    if (!isFeatureTransition()) {
        return std::nullopt;
    }
    return (data_->transitionConnectedEnds_ & 0x2U) != 0
        ? ValidityData::End
        : ValidityData::Start;
}

std::optional<uint32_t> AttributeValidityEntry::transitionPivotIndex() const
{
    return isFeatureTransition() &&
        data_->transitionPivotIndex_ != InvalidTransitionPivotIndex
        ? std::optional<uint32_t>{data_->transitionPivotIndex_}
        : std::nullopt;
}

nlohmann::json AttributeValidityEntry::toJson() const
{
    auto result = nlohmann::json::object({
        {"type", "AttributeValidityEntry"},
        {"featureId", featureId() ? featureId()->toString() : ""},
        {"geometry", geometry() ? geometry()->toJson() : nlohmann::json()},
        {"hostValues", arrayToJson(hostValues())},
        {"values", arrayToJson(values())},
        {"hasValidity", hasValidity()},
        {"validityIndex", validityIndex()},
        {"validityCount", validityCount()},
    });
    if (auto index = attributeIndex()) result["attributeIndex"] = *index;
    if (auto layer = attributeLayer()) result["attributeLayer"] = *layer;
    if (auto name = attributeName()) result["attributeName"] = *name;
    if (isFeatureTransition()) {
        auto const from = transitionFromFeatureId();
        auto const to = transitionToFeatureId();
        result["geometryDescriptionType"] = "feature-transition";
        result["transitionFromFeatureId"] = from ? from->toString() : "";
        result["transitionToFeatureId"] = to ? to->toString() : "";
        result["transitionFromConnectedEnd"] =
            transitionFromConnectedEnd() == ValidityData::End ? "end" : "start";
        result["transitionToConnectedEnd"] =
            transitionToConnectedEnd() == ValidityData::End ? "end" : "start";
        if (auto pivot = transitionPivotIndex()) {
            result["transitionPivotIndex"] = *pivot;
        }
    }
    return result;
}

simfil::ValueType AttributeValidityEntry::type() const
{
    return simfil::ValueType::Object;
}

simfil::ModelNode::Ptr AttributeValidityEntry::at(int64_t index) const
{
    return get(keyAt(index));
}

uint32_t AttributeValidityEntry::size() const
{
    return 10;
}

simfil::ModelNode::Ptr AttributeValidityEntry::get(simfil::StringId const& field) const
{
    if (field == StringPool::FeatureIdStr) return featureId();
    if (field == StringPool::GeometryStr) return geometry();
    if (field == StringPool::HostValuesStr) return hostValues();
    if (field == StringPool::ValuesStr) return values();
    if (field == StringPool::AttributeIndexStr) {
        if (auto index = attributeIndex()) return model().newValue(static_cast<int64_t>(*index));
        return {};
    }
    if (field == StringPool::AttributeLayerStr) {
        return data_->attributeLayer_ ? model().resolve(data_->attributeLayer_) : simfil::ModelNode::Ptr{};
    }
    if (field == StringPool::AttributeNameStr) {
        return data_->attributeName_ ? model().resolve(data_->attributeName_) : simfil::ModelNode::Ptr{};
    }
    if (field == StringPool::HasValidityStr) return model().newSmallValue(hasValidity());
    if (field == StringPool::ValidityIndexStr) {
        return model().newValue(static_cast<int64_t>(validityIndex()));
    }
    if (field == StringPool::ValidityCountStr) {
        return model().newValue(static_cast<int64_t>(validityCount()));
    }
    return {};
}

simfil::StringId AttributeValidityEntry::keyAt(int64_t index) const
{
    switch (index) {
    case 0: return StringPool::FeatureIdStr;
    case 1: return StringPool::GeometryStr;
    case 2: return StringPool::HostValuesStr;
    case 3: return StringPool::ValuesStr;
    case 4: return StringPool::AttributeIndexStr;
    case 5: return StringPool::AttributeLayerStr;
    case 6: return StringPool::AttributeNameStr;
    case 7: return StringPool::HasValidityStr;
    case 8: return StringPool::ValidityIndexStr;
    case 9: return StringPool::ValidityCountStr;
    default: return simfil::StringPool::Empty;
    }
}

bool AttributeValidityEntry::iterate(IterCallback const& callback) const
{
    for (uint32_t index = 0; index < size(); ++index) {
        if (auto value = at(index); value && !callback(*value)) {
            return false;
        }
    }
    return true;
}

RelationEntry::RelationEntry(
    Data* data,
    simfil::ModelConstPtr pool,
    simfil::ModelNodeAddress address,
    simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileSubsetLayer>(
          std::move(pool),
          address,
          key),
      data_(data)
{
}

std::string RelationEntry::relationId() const
{
    return nodeStringValue(model().resolve(data_->relationId_));
}

std::string RelationEntry::name() const
{
    return nodeStringValue(model().resolve(data_->name_));
}

std::string RelationEntry::provenance() const
{
    return nodeStringValue(model().resolve(data_->provenance_));
}

RelationDirection RelationEntry::direction() const
{
    return data_->direction_;
}

bool RelationEntry::twoway() const
{
    return data_->twoway_;
}

model_ptr<FeatureEntry> RelationEntry::source() const
{
    return model().resolve<FeatureEntry>(data_->source_);
}

model_ptr<FeatureEntry> RelationEntry::target() const
{
    return model().resolve<FeatureEntry>(data_->target_);
}

model_ptr<GeometryCollection> RelationEntry::sourceGeometry() const
{
    return model().resolve<GeometryCollection>(data_->sourceGeometry_);
}

model_ptr<GeometryCollection> RelationEntry::targetGeometry() const
{
    return model().resolve<GeometryCollection>(data_->targetGeometry_);
}

model_ptr<Array> RelationEntry::values() const
{
    return model().resolve<Array>(data_->values_);
}

nlohmann::json RelationEntry::toJson() const
{
    return {
        {"type", "RelationEntry"},
        {"relationId", relationId()},
        {"name", name()},
        {"provenance", provenance()},
        {"direction", direction()},
        {"twoway", twoway()},
        {"source", source() ? source()->toJson() : nlohmann::json()},
        {"target", target() ? target()->toJson() : nlohmann::json()},
        {"sourceGeometry", sourceGeometry() ? sourceGeometry()->toJson() : nlohmann::json()},
        {"targetGeometry", targetGeometry() ? targetGeometry()->toJson() : nlohmann::json()},
        {"values", arrayToJson(values())},
    };
}

simfil::ValueType RelationEntry::type() const
{
    return simfil::ValueType::Object;
}

simfil::ModelNode::Ptr RelationEntry::at(int64_t index) const
{
    return get(keyAt(index));
}

uint32_t RelationEntry::size() const
{
    return 10;
}

simfil::ModelNode::Ptr RelationEntry::get(simfil::StringId const& field) const
{
    if (field == StringPool::RelationIdStr) return model().resolve(data_->relationId_);
    if (field == StringPool::NameStr) return model().resolve(data_->name_);
    if (field == StringPool::ProvenanceStr) return model().resolve(data_->provenance_);
    if (field == StringPool::DirectionStr) {
        return model().newValue(nlohmann::json(direction()).get<std::string>());
    }
    if (field == StringPool::TwowayStr) return model().newSmallValue(twoway());
    if (field == StringPool::SourceStr) return source();
    if (field == StringPool::TargetStr) return target();
    if (field == StringPool::SourceGeometryStr) return sourceGeometry();
    if (field == StringPool::TargetGeometryStr) return targetGeometry();
    if (field == StringPool::ValuesStr) return values();
    return {};
}

simfil::StringId RelationEntry::keyAt(int64_t index) const
{
    switch (index) {
    case 0: return StringPool::RelationIdStr;
    case 1: return StringPool::NameStr;
    case 2: return StringPool::ProvenanceStr;
    case 3: return StringPool::DirectionStr;
    case 4: return StringPool::TwowayStr;
    case 5: return StringPool::SourceStr;
    case 6: return StringPool::TargetStr;
    case 7: return StringPool::SourceGeometryStr;
    case 8: return StringPool::TargetGeometryStr;
    case 9: return StringPool::ValuesStr;
    default: return simfil::StringPool::Empty;
    }
}

bool RelationEntry::iterate(IterCallback const& callback) const
{
    for (uint32_t index = 0; index < size(); ++index) {
        if (auto value = at(index); value && !callback(*value)) {
            return false;
        }
    }
    return true;
}

GroupEntry::GroupEntry(
    Data* data,
    simfil::ModelConstPtr pool,
    simfil::ModelNodeAddress address,
    simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileSubsetLayer>(
          std::move(pool),
          address,
          key),
      data_(data)
{
}

simfil::ModelNode::Ptr GroupEntry::groupKey() const
{
    return model().resolve(data_->groupKey_);
}

model_ptr<FeatureId> GroupEntry::representativeFeatureId() const
{
    return model().resolve<FeatureId>(data_->representativeFeatureId_);
}

model_ptr<GeometryCollection> GroupEntry::geometry() const
{
    return model().resolve<GeometryCollection>(data_->geometry_);
}

model_ptr<Array> GroupEntry::values() const
{
    return model().resolve<Array>(data_->values_);
}

model_ptr<Array> GroupEntry::memberFeatureIds() const
{
    return model().resolve<Array>(data_->memberFeatureIds_);
}

nlohmann::json GroupEntry::toJson() const
{
    return {
        {"type", "GroupEntry"},
        {"groupKey", groupKey() ? groupKey()->toJson() : nlohmann::json()},
        {"representativeFeatureId",
            representativeFeatureId() ? representativeFeatureId()->toString() : ""},
        {"geometry", geometry() ? geometry()->toJson() : nlohmann::json()},
        {"values", arrayToJson(values())},
        {"memberFeatureIds", arrayToJson(memberFeatureIds())},
    };
}

simfil::ValueType GroupEntry::type() const
{
    return simfil::ValueType::Object;
}

simfil::ModelNode::Ptr GroupEntry::at(int64_t index) const
{
    return get(keyAt(index));
}

uint32_t GroupEntry::size() const
{
    return 5;
}

simfil::ModelNode::Ptr GroupEntry::get(simfil::StringId const& field) const
{
    if (field == StringPool::GroupKeyStr) return groupKey();
    if (field == StringPool::RepresentativeFeatureIdStr) return representativeFeatureId();
    if (field == StringPool::GeometryStr) return geometry();
    if (field == StringPool::ValuesStr) return values();
    if (field == StringPool::MemberFeatureIdsStr) return memberFeatureIds();
    return {};
}

simfil::StringId GroupEntry::keyAt(int64_t index) const
{
    switch (index) {
    case 0: return StringPool::GroupKeyStr;
    case 1: return StringPool::RepresentativeFeatureIdStr;
    case 2: return StringPool::GeometryStr;
    case 3: return StringPool::ValuesStr;
    case 4: return StringPool::MemberFeatureIdsStr;
    default: return simfil::StringPool::Empty;
    }
}

bool GroupEntry::iterate(IterCallback const& callback) const
{
    for (uint32_t index = 0; index < size(); ++index) {
        if (auto value = at(index); value && !callback(*value)) {
            return false;
        }
    }
    return true;
}

TileSubsetChannel::TileSubsetChannel(
    Data* data,
    simfil::ModelConstPtr pool,
    simfil::ModelNodeAddress address,
    simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileSubsetLayer>(
          std::move(pool),
          address,
          key),
      data_(data)
{
}

std::string TileSubsetChannel::channelId() const
{
    return nodeStringValue(model().resolve(data_->channelId_));
}

Scope TileSubsetChannel::scope() const
{
    return data_->scope_;
}

uint32_t TileSubsetChannel::geometryTypes() const
{
    return data_->geometryTypes_;
}

bool TileSubsetChannel::hasWildcardGeometryName() const
{
    return !data_->geometryName_;
}

std::optional<std::string> TileSubsetChannel::geometryName() const
{
    if (hasWildcardGeometryName()) {
        return std::nullopt;
    }
    return nodeStringValue(model().resolve(data_->geometryName_));
}

size_t TileSubsetChannel::featureFieldCount() const
{
    auto fields = arrayAt(model(), data_->featureFields_);
    return fields ? fields->size() : 0;
}

std::string TileSubsetChannel::featureField(size_t index) const
{
    auto fields = arrayAt(model(), data_->featureFields_);
    if (!fields || index >= fields->size()) {
        raiseFmt("Feature field index {} is out of range.", index);
    }
    return nodeStringValue(fields->at(static_cast<int64_t>(index)));
}

std::vector<std::string> TileSubsetChannel::featureFields() const
{
    std::vector<std::string> result;
    result.reserve(featureFieldCount());
    for (size_t index = 0; index < featureFieldCount(); ++index) {
        result.push_back(featureField(index));
    }
    return result;
}

size_t TileSubsetChannel::entryFieldCount() const
{
    auto fields = arrayAt(model(), data_->entryFields_);
    return fields ? fields->size() : 0;
}

std::string TileSubsetChannel::entryField(size_t index) const
{
    auto fields = arrayAt(model(), data_->entryFields_);
    if (!fields || index >= fields->size()) {
        raiseFmt("Entry field index {} is out of range.", index);
    }
    return nodeStringValue(fields->at(static_cast<int64_t>(index)));
}

std::vector<std::string> TileSubsetChannel::entryFields() const
{
    std::vector<std::string> result;
    result.reserve(entryFieldCount());
    for (size_t index = 0; index < entryFieldCount(); ++index) {
        result.push_back(entryField(index));
    }
    return result;
}

size_t TileSubsetChannel::featureEntryCount() const
{
    auto entries = arrayAt(model(), data_->featureEntries_);
    return entries ? entries->size() : 0;
}

size_t TileSubsetChannel::attributeValidityEntryCount() const
{
    auto entries = arrayAt(model(), data_->attributeValidityEntries_);
    return entries ? entries->size() : 0;
}

size_t TileSubsetChannel::relationEntryCount() const
{
    auto entries = arrayAt(model(), data_->relationEntries_);
    return entries ? entries->size() : 0;
}

size_t TileSubsetChannel::groupEntryCount() const
{
    auto entries = arrayAt(model(), data_->groupEntries_);
    return entries ? entries->size() : 0;
}

size_t TileSubsetChannel::entryCount() const
{
    switch (scope()) {
    case Scope::Feature: return featureEntryCount();
    case Scope::Attribute: return attributeValidityEntryCount();
    case Scope::Relation: return relationEntryCount();
    case Scope::Group: return groupEntryCount();
    }
    return 0;
}

model_ptr<FeatureEntry> TileSubsetChannel::newFeatureEntry(
    model_ptr<FeatureId> const& featureId,
    model_ptr<GeometryCollection> const& geometry,
    std::span<simfil::ModelNode::Ptr const> values)
{
    if (scope() != Scope::Feature && scope() != Scope::Relation) {
        raise("Feature entries are valid only in feature and relation channels.");
    }
    if (values.size() != featureFieldCount()) {
        raiseFmt(
            "Feature entry has {} values for {} feature fields.",
            values.size(),
            featureFieldCount());
    }
    auto entry = model().newFeatureEntry(featureId, geometry, values);
    arrayAt(model(), data_->featureEntries_)->append(entry);
    model().updateEntryStatistics();
    return entry;
}

model_ptr<AttributeValidityEntry> TileSubsetChannel::newAttributeValidityEntry(
    model_ptr<FeatureId> const& featureId,
    model_ptr<GeometryCollection> const& geometry,
    uint32_t attributeIndex,
    bool hasValidity,
    uint32_t validityIndex,
    uint32_t validityCount,
    std::span<simfil::ModelNode::Ptr const> hostValues,
    std::span<simfil::ModelNode::Ptr const> values,
    std::optional<std::string_view> attributeLayer,
    std::optional<std::string_view> attributeName,
    AttributeValidityEntry::GeometryDescriptionType geometryDescriptionType,
    model_ptr<FeatureId> const& transitionFromFeatureId,
    AttributeValidityEntry::TransitionEnd transitionFromConnectedEnd,
    model_ptr<FeatureId> const& transitionToFeatureId,
    AttributeValidityEntry::TransitionEnd transitionToConnectedEnd,
    uint32_t transitionPivotIndex)
{
    if (scope() != Scope::Attribute) {
        raise("Attribute-validity entries are valid only in attribute channels.");
    }
    if (hostValues.size() != featureFieldCount()) {
        raiseFmt(
            "Attribute-validity entry has {} host values for {} feature fields.",
            hostValues.size(),
            featureFieldCount());
    }
    if (values.size() != entryFieldCount()) {
        raiseFmt(
            "Attribute-validity entry has {} values for {} entry fields.",
            values.size(),
            entryFieldCount());
    }
    auto entry = model().newAttributeValidityEntry(
        featureId,
        geometry,
        attributeIndex,
        hasValidity,
        validityIndex,
        validityCount,
        hostValues,
        values,
        attributeLayer,
        attributeName,
        geometryDescriptionType,
        transitionFromFeatureId,
        transitionFromConnectedEnd,
        transitionToFeatureId,
        transitionToConnectedEnd,
        transitionPivotIndex);
    arrayAt(model(), data_->attributeValidityEntries_)->append(entry);
    model().updateEntryStatistics();
    return entry;
}

model_ptr<RelationEntry> TileSubsetChannel::newRelationEntry(
    std::string_view relationId,
    std::string_view name,
    std::string_view provenance,
    RelationDirection direction,
    bool twoway,
    model_ptr<FeatureEntry> const& source,
    model_ptr<FeatureEntry> const& target,
    model_ptr<GeometryCollection> const& sourceGeometry,
    model_ptr<GeometryCollection> const& targetGeometry,
    std::span<simfil::ModelNode::Ptr const> values)
{
    if (scope() != Scope::Relation) {
        raise("Relation entries are valid only in relation channels.");
    }
    if (values.size() != entryFieldCount()) {
        raiseFmt(
            "Relation entry has {} values for {} entry fields.",
            values.size(),
            entryFieldCount());
    }
    auto supportingEntries = arrayAt(model(), data_->featureEntries_);
    if (!source || !arrayContainsAddress(supportingEntries, source->addr()) ||
        !target || !arrayContainsAddress(supportingEntries, target->addr()))
    {
        raise("Relation endpoints must be supporting FeatureEntries of the same channel.");
    }
    auto entry = model().newRelationEntry(
        relationId,
        name,
        provenance,
        direction,
        twoway,
        source,
        target,
        sourceGeometry,
        targetGeometry,
        values);
    arrayAt(model(), data_->relationEntries_)->append(entry);
    model().updateEntryStatistics();
    return entry;
}

model_ptr<GroupEntry> TileSubsetChannel::newGroupEntry(
    simfil::ModelNode::Ptr const& groupKey,
    model_ptr<FeatureId> const& representativeFeatureId,
    model_ptr<GeometryCollection> const& geometry,
    std::span<simfil::ModelNode::Ptr const> values,
    std::span<model_ptr<FeatureId> const> memberFeatureIds)
{
    if (scope() != Scope::Group) {
        raise("Group entries are valid only in group channels.");
    }
    if (values.size() != entryFieldCount()) {
        raiseFmt(
            "Group entry has {} values for {} entry fields.",
            values.size(),
            entryFieldCount());
    }
    auto entry = model().newGroupEntry(
        groupKey,
        representativeFeatureId,
        geometry,
        values,
        memberFeatureIds);
    arrayAt(model(), data_->groupEntries_)->append(entry);
    model().updateEntryStatistics();
    return entry;
}

bool TileSubsetChannel::forEachFeatureEntry(
    std::function<bool(model_ptr<FeatureEntry> const&)> const& callback) const
{
    return model().forEachEntry(data_->featureEntries_, callback);
}

bool TileSubsetChannel::forEachAttributeValidityEntry(
    std::function<bool(model_ptr<AttributeValidityEntry> const&)> const& callback) const
{
    return model().forEachEntry(data_->attributeValidityEntries_, callback);
}

bool TileSubsetChannel::forEachRelationEntry(
    std::function<bool(model_ptr<RelationEntry> const&)> const& callback) const
{
    return model().forEachEntry(data_->relationEntries_, callback);
}

bool TileSubsetChannel::forEachGroupEntry(
    std::function<bool(model_ptr<GroupEntry> const&)> const& callback) const
{
    return model().forEachEntry(data_->groupEntries_, callback);
}

nlohmann::json TileSubsetChannel::toJson() const
{
    auto result = nlohmann::json::object({
        {"type", "TileSubsetChannel"},
        {"channelId", channelId()},
        {"scope", scope()},
        {"geometryTypes", geometryTypes()},
        {"geometryName", geometryName() ? nlohmann::json(*geometryName()) : nlohmann::json("*")},
        {"featureFields", featureFields()},
        {"entryFields", entryFields()},
        {"featureEntries", nlohmann::json::array()},
        {"attributeValidityEntries", nlohmann::json::array()},
        {"relationEntries", nlohmann::json::array()},
        {"groupEntries", nlohmann::json::array()},
    });
    forEachFeatureEntry([&](auto const& entry) {
        result["featureEntries"].push_back(entry->toJson());
        return true;
    });
    forEachAttributeValidityEntry([&](auto const& entry) {
        result["attributeValidityEntries"].push_back(entry->toJson());
        return true;
    });
    forEachRelationEntry([&](auto const& entry) {
        result["relationEntries"].push_back(entry->toJson());
        return true;
    });
    forEachGroupEntry([&](auto const& entry) {
        result["groupEntries"].push_back(entry->toJson());
        return true;
    });
    return result;
}

simfil::ValueType TileSubsetChannel::type() const
{
    return simfil::ValueType::Object;
}

simfil::ModelNode::Ptr TileSubsetChannel::at(int64_t index) const
{
    return get(keyAt(index));
}

uint32_t TileSubsetChannel::size() const
{
    return 10;
}

simfil::ModelNode::Ptr TileSubsetChannel::get(simfil::StringId const& field) const
{
    if (field == StringPool::ChannelIdStr) return model().resolve(data_->channelId_);
    if (field == StringPool::ScopeStr) {
        return model().newValue(nlohmann::json(scope()).get<std::string>());
    }
    if (field == StringPool::GeometryTypesStr) {
        return model().newValue(static_cast<int64_t>(geometryTypes()));
    }
    if (field == StringPool::GeometryNameStr) {
        return hasWildcardGeometryName()
            ? model().newValue("*")
            : model().resolve(data_->geometryName_);
    }
    if (field == StringPool::FeatureFieldsStr) return arrayAt(model(), data_->featureFields_);
    if (field == StringPool::EntryFieldsStr) return arrayAt(model(), data_->entryFields_);
    if (field == StringPool::FeatureEntriesStr) return arrayAt(model(), data_->featureEntries_);
    if (field == StringPool::AttributeValidityEntriesStr) {
        return arrayAt(model(), data_->attributeValidityEntries_);
    }
    if (field == StringPool::RelationEntriesStr) return arrayAt(model(), data_->relationEntries_);
    if (field == StringPool::GroupEntriesStr) return arrayAt(model(), data_->groupEntries_);
    return {};
}

simfil::StringId TileSubsetChannel::keyAt(int64_t index) const
{
    switch (index) {
    case 0: return StringPool::ChannelIdStr;
    case 1: return StringPool::ScopeStr;
    case 2: return StringPool::GeometryTypesStr;
    case 3: return StringPool::GeometryNameStr;
    case 4: return StringPool::FeatureFieldsStr;
    case 5: return StringPool::EntryFieldsStr;
    case 6: return StringPool::FeatureEntriesStr;
    case 7: return StringPool::AttributeValidityEntriesStr;
    case 8: return StringPool::RelationEntriesStr;
    case 9: return StringPool::GroupEntriesStr;
    default: return simfil::StringPool::Empty;
    }
}

bool TileSubsetChannel::iterate(IterCallback const& callback) const
{
    for (uint32_t index = 0; index < size(); ++index) {
        if (auto value = at(index); value && !callback(*value)) {
            return false;
        }
    }
    return true;
}

TileSubsetLayer::TileSubsetLayer(
    TileId tileId,
    std::string const& stringPoolId,
    std::string const& mapId,
    std::shared_ptr<LayerInfo> const& layerInfo,
    std::shared_ptr<simfil::StringPool> const& strings,
    std::string filterId,
    uint64_t generation)
    : TileFeatureModelLayerBase(tileId, stringPoolId, mapId, layerInfo, strings),
      geometryAnchor_(tileId.centerWgs84()),
      filterId_(std::move(filterId)),
      generation_(generation)
{
}

TileSubsetLayer::TileSubsetLayer(
    std::vector<uint8_t> const& input,
    LayerInfoResolveFun const& layerInfoResolveFun,
    StringPoolResolveFun const& stringPoolGetter)
    : TileFeatureModelLayerBase(
          input,
          layerInfoResolveFun,
          stringPoolGetter,
          &deserializationOffsetBytes_),
      geometryAnchor_(tileId_.centerWgs84())
{
    using Adapter = bitsery::InputBufferAdapter<std::vector<uint8_t>>;
    if (deserializationOffsetBytes_ > input.size()) {
        raise("Failed to read TileSubsetLayer: invalid deserialization offset.");
    }
    bitsery::Deserializer<Adapter> serializer(Adapter(
        input.begin() + static_cast<std::ptrdiff_t>(deserializationOffsetBytes_),
        input.end()));

    serializer.text1b(filterId_, std::numeric_limits<uint32_t>::max());
    serializer.value8b(generation_);
    serializer.value8b(geometryAnchor_.x);
    serializer.value8b(geometryAnchor_.y);
    serializer.value8b(geometryAnchor_.z);

    std::vector<DependencyWire> dependencyWire;
    serializer.container(
        dependencyWire,
        std::numeric_limits<uint32_t>::max(),
        [](auto& nested, DependencyWire& dependency) {
            readWriteDependencyWire(nested, dependency);
        });
    std::vector<IssueWire> issueWire;
    serializer.container(
        issueWire,
        std::numeric_limits<uint32_t>::max(),
        [](auto& nested, IssueWire& issue) {
            readWriteIssueWire(nested, issue);
        });
    bool hasGlbAttachmentName = false;
    serializer.value1b(hasGlbAttachmentName);
    if (hasGlbAttachmentName) {
        std::string glbAttachmentName;
        serializer.text1b(
            glbAttachmentName,
            std::numeric_limits<uint32_t>::max());
        glbAttachmentName_ = std::move(glbAttachmentName);
    }
    readWriteDiagnostics(serializer, diagnostics_);
    serializer.object(channels_);
    serializer.object(featureEntries_);
    serializer.object(attributeValidityEntries_);
    serializer.object(relationEntries_);
    serializer.object(groupEntries_);
    serializer.object(traces_);
    readWriteCommonColumns(serializer);

    if (serializer.adapter().error() != bitsery::ReaderError::NoError) {
        raiseFmt(
            "Failed to read TileSubsetLayer: Error {}",
            static_cast<std::underlying_type_t<bitsery::ReaderError>>(
                serializer.adapter().error()));
    }

    dependencies_.reserve(dependencyWire.size());
    for (auto&& dependency : dependencyWire) {
        dependencies_.push_back({
            MapTileKey(dependency.sourceTileKey_),
            dependency.sourceFeatureCount_,
        });
    }
    issues_.reserve(issueWire.size());
    for (auto&& issue : issueWire) {
        issues_.push_back({
            std::move(issue.channelId_),
            std::move(issue.expression_),
            issue.scope_,
            std::move(issue.message_),
            issue.occurrenceCount_,
        });
    }

    validateGeometryNameStorage();
    auto const modelOffset =
        deserializationOffsetBytes_ + serializer.adapter().currentReadPos();
    if (auto result = ModelPool::read(input, modelOffset); !result) {
        raise(result.error().message);
    }
}

TileSubsetLayer::~TileSubsetLayer() = default;

FilterIdentity TileSubsetLayer::readFilterIdentity(
    std::vector<uint8_t> const& input,
    LayerInfoResolveFun const& layerInfoResolveFun,
    size_t* bytesRead)
{
    size_t offset = 0;
    TileLayer base(input, layerInfoResolveFun, &offset);
    using Adapter = bitsery::InputBufferAdapter<std::vector<uint8_t>>;
    if (offset > input.size()) {
        raise("Failed to read TileSubsetLayer identity: invalid base-layer offset.");
    }
    bitsery::Deserializer<Adapter> serializer(Adapter(
        input.begin() + static_cast<std::ptrdiff_t>(offset),
        input.end()));
    FilterIdentity result;
    serializer.text1b(result.filterId_, std::numeric_limits<uint32_t>::max());
    serializer.value8b(result.generation_);
    if (serializer.adapter().error() != bitsery::ReaderError::NoError) {
        raise("Failed to read TileSubsetLayer identity.");
    }
    if (bytesRead) {
        *bytesRead = offset + serializer.adapter().currentReadPos();
    }
    return result;
}

TileSubsetLayerMetadata TileSubsetLayer::readMetadata(
    std::vector<uint8_t> const& input,
    LayerInfoResolveFun const& layerInfoResolveFun,
    size_t* bytesRead)
{
    size_t offset = 0;
    TileSubsetLayerMetadata result;
    result.identity_ = readFilterIdentity(
        input,
        layerInfoResolveFun,
        &offset);

    using Adapter = bitsery::InputBufferAdapter<std::vector<uint8_t>>;
    if (offset > input.size()) {
        raise("Failed to read TileSubsetLayer metadata: invalid prelude offset.");
    }
    bitsery::Deserializer<Adapter> serializer(Adapter(
        input.begin() + static_cast<std::ptrdiff_t>(offset),
        input.end()));

    Point ignoredGeometryAnchor;
    serializer.value8b(ignoredGeometryAnchor.x);
    serializer.value8b(ignoredGeometryAnchor.y);
    serializer.value8b(ignoredGeometryAnchor.z);

    std::vector<DependencyWire> dependencyWire;
    serializer.container(
        dependencyWire,
        std::numeric_limits<uint32_t>::max(),
        [](auto& nested, DependencyWire& dependency) {
            readWriteDependencyWire(nested, dependency);
        });
    std::vector<IssueWire> issueWire;
    serializer.container(
        issueWire,
        std::numeric_limits<uint32_t>::max(),
        [](auto& nested, IssueWire& issue) {
            readWriteIssueWire(nested, issue);
        });
    bool hasGlbAttachmentName = false;
    serializer.value1b(hasGlbAttachmentName);
    if (hasGlbAttachmentName) {
        std::string glbAttachmentName;
        serializer.text1b(
            glbAttachmentName,
            std::numeric_limits<uint32_t>::max());
        result.glbAttachmentName_ = std::move(glbAttachmentName);
    }

    if (serializer.adapter().error() != bitsery::ReaderError::NoError) {
        raise("Failed to read TileSubsetLayer metadata.");
    }

    result.dependencies_.reserve(dependencyWire.size());
    for (auto&& dependency : dependencyWire) {
        result.dependencies_.push_back({
            MapTileKey(dependency.sourceTileKey_),
            dependency.sourceFeatureCount_,
        });
    }
    result.issues_.reserve(issueWire.size());
    for (auto&& issue : issueWire) {
        result.issues_.push_back({
            std::move(issue.channelId_),
            std::move(issue.expression_),
            issue.scope_,
            std::move(issue.message_),
            issue.occurrenceCount_,
        });
    }
    if (bytesRead) {
        *bytesRead = offset + serializer.adapter().currentReadPos();
    }
    return result;
}

std::string const& TileSubsetLayer::filterId() const
{
    return filterId_;
}

uint64_t TileSubsetLayer::generation() const
{
    return generation_;
}

void TileSubsetLayer::adoptSourceInfo(TileFeatureLayer const& source)
{
    setTimestamp(source.timestamp());
    setTtl(source.ttl());
    setInfo(source.info());
}

void TileSubsetLayer::setDependencies(
    std::vector<TileSubsetDependency> dependencies)
{
    std::sort(
        dependencies.begin(),
        dependencies.end(),
        [](auto const& left, auto const& right) {
            return left.sourceTileKey_ < right.sourceTileKey_;
        });
    std::vector<TileSubsetDependency> normalized;
    normalized.reserve(dependencies.size());
    for (auto&& dependency : dependencies) {
        if (!normalized.empty() &&
            normalized.back().sourceTileKey_ == dependency.sourceTileKey_)
        {
            if (normalized.back().sourceFeatureCount_ !=
                dependency.sourceFeatureCount_)
            {
                raiseFmt(
                    "Conflicting source feature counts for dependency {}.",
                    dependency.sourceTileKey_.toString());
            }
            continue;
        }
        normalized.push_back(std::move(dependency));
    }
    dependencies_ = std::move(normalized);
}

void TileSubsetLayer::addDependency(
    MapTileKey sourceTileKey,
    uint32_t sourceFeatureCount)
{
    auto dependencies = dependencies_;
    dependencies.push_back({std::move(sourceTileKey), sourceFeatureCount});
    setDependencies(std::move(dependencies));
}

std::vector<TileSubsetDependency> const& TileSubsetLayer::dependencies() const
{
    return dependencies_;
}

std::optional<uint32_t> TileSubsetLayer::localSourceFeatureCount() const
{
    auto const outputKey = id();
    auto found = std::find_if(
        dependencies_.begin(),
        dependencies_.end(),
        [&](auto const& dependency) {
            return dependency.sourceTileKey_ == outputKey;
        });
    if (found == dependencies_.end()) {
        return std::nullopt;
    }
    return found->sourceFeatureCount_;
}

void TileSubsetLayer::addIssue(FilterIssue issue)
{
    issues_.push_back(std::move(issue));
}

std::vector<FilterIssue> const& TileSubsetLayer::issues() const
{
    return issues_;
}

void TileSubsetLayer::setDiagnostics(simfil::Diagnostics const& diagnostics)
{
    diagnostics_.exprIndex_.clear();
    diagnostics_.fieldData_.clear();
    diagnostics_.comparisonData_.clear();
    diagnostics_.append(diagnostics);
}

simfil::Diagnostics const& TileSubsetLayer::diagnostics() const
{
    return diagnostics_;
}

void TileSubsetLayer::setTraces(std::map<std::string, simfil::Trace> traces)
{
    traces_.clear();
    for (auto&& [name, trace] : traces) {
        auto values = newArray(std::max<size_t>(1, trace.values.size()), true);
        for (auto const& value : trace.values) {
            values->append(materializeValue(value));
        }
        auto nameNode = newValue(name);
        traces_.emplace_back(FilterTrace::Data{
            nameNode->addr(),
            values->addr(),
            static_cast<uint64_t>(trace.calls),
            static_cast<int64_t>(trace.totalus.count()),
        });
    }
}

size_t TileSubsetLayer::traceCount() const
{
    return traces_.size();
}

model_ptr<FilterTrace> TileSubsetLayer::traceAt(size_t index) const
{
    if (index >= traces_.size()) {
        return {};
    }
    return resolve<FilterTrace>({
        ColumnId::FilterTraces,
        static_cast<uint32_t>(index),
    });
}

void TileSubsetLayer::setGlbAttachmentName(std::optional<std::string> name)
{
    if (name && name->empty()) {
        raise("A GLB attachment name must not be empty.");
    }
    glbAttachmentName_ = std::move(name);
}

std::optional<std::string> const& TileSubsetLayer::glbAttachmentName() const
{
    return glbAttachmentName_;
}

simfil::ModelNode::Ptr TileSubsetLayer::materializeValue(
    simfil::Value const& value)
{
    switch (value.type) {
    case simfil::ValueType::Undef:
    case simfil::ValueType::Null:
        return resolve<simfil::ModelNode>(
            {simfil::Model::Null, 1},
            simfil::ScalarValueType{});
    case simfil::ValueType::Bool:
        return newSmallValue(value.as<simfil::ValueType::Bool>());
    case simfil::ValueType::Int:
        return newValue(value.as<simfil::ValueType::Int>());
    case simfil::ValueType::Float:
        return newValue(value.as<simfil::ValueType::Float>());
    case simfil::ValueType::String:
        return newValue(value.as<simfil::ValueType::String>());
    case simfil::ValueType::Bytes:
    case simfil::ValueType::TransientObject:
    case simfil::ValueType::Object:
    case simfil::ValueType::Array:
        raiseFmt(
            "TileSubsetLayer fields support only scalar values, not {}.",
            value.toString());
    case simfil::ValueType::LAST_:
        break;
    }
    raise("Unsupported SIMFIL value type for TileSubsetLayer.");
    return {};
}

model_ptr<Array> TileSubsetLayer::newValueArray(
    std::span<simfil::ModelNode::Ptr const> values)
{
    if (values.empty()) {
        return sharedEmptyArray();
    }
    auto array = newArray(values.size(), true);
    for (auto const& value : values) {
        if (!value) {
            array->append(resolve<simfil::ModelNode>(
                {simfil::Model::Null, 1},
                simfil::ScalarValueType{}));
            continue;
        }
        validateOwnedNode(value, "projected value");
        array->append(value);
    }
    return array;
}

model_ptr<Array> TileSubsetLayer::newStringArray(
    std::span<std::string const> values)
{
    if (values.empty()) {
        return sharedEmptyArray();
    }
    auto array = newArray(values.size(), true);
    for (auto const& value : values) {
        array->append(newValue(value));
    }
    return array;
}

model_ptr<Array> TileSubsetLayer::sharedEmptyArray()
{
    if (!sharedEmptyArrayAddress_) {
        sharedEmptyArrayAddress_ = newArray(1, true)->addr();
    }
    return resolve<Array>(sharedEmptyArrayAddress_);
}

model_ptr<TileSubsetChannel> TileSubsetLayer::newChannel(
    std::string_view channelId,
    Scope scope,
    uint32_t geometryTypes,
    std::optional<std::string_view> geometryName,
    std::span<std::string const> featureFields,
    std::span<std::string const> entryFields)
{
    if (channelId.empty()) {
        raise("A subset channel requires a non-empty channelId.");
    }
    for (size_t index = 0; index < size(); ++index) {
        if (at(index)->channelId() == channelId) {
            raiseFmt("Duplicate subset channelId '{}'.", channelId);
        }
    }
    if (scope == Scope::Feature && !entryFields.empty()) {
        raise("Feature channels store all projections in featureFields.");
    }
    if (scope == Scope::Group && !featureFields.empty()) {
        raise("Group channels store all projections in entryFields.");
    }
    if (geometryName && geometryName->empty()) {
        raise("A concrete geometry-name selector must not be empty.");
    }

    auto channelIdNode = newValue(channelId);
    simfil::ModelNodeAddress geometryNameAddress;
    if (geometryName) {
        geometryNameAddress = newValue(*geometryName)->addr();
    }
    auto featureFieldArray = newStringArray(featureFields);
    auto entryFieldArray = newStringArray(entryFields);
    auto featureEntries = newArray(1);
    auto attributeEntries = newArray(1);
    auto relationEntries = newArray(1);
    auto groupEntries = newArray(1);

    auto const index = static_cast<uint32_t>(channels_.size());
    channels_.emplace_back(TileSubsetChannel::Data{
        channelIdNode->addr(),
        geometryNameAddress,
        static_cast<simfil::ArrayIndex>(featureFieldArray->addr().index()),
        static_cast<simfil::ArrayIndex>(entryFieldArray->addr().index()),
        static_cast<simfil::ArrayIndex>(featureEntries->addr().index()),
        static_cast<simfil::ArrayIndex>(attributeEntries->addr().index()),
        static_cast<simfil::ArrayIndex>(relationEntries->addr().index()),
        static_cast<simfil::ArrayIndex>(groupEntries->addr().index()),
        geometryTypes,
        scope,
    });
    auto channel = TileSubsetChannel(
        &channels_.back(),
        shared_from_this(),
        {ColumnId::SubsetChannels, index},
        mpKey_);
    addRoot(simfil::ModelNode::Ptr(channel));
    updateEntryStatistics();
    return channel;
}

model_ptr<FeatureEntry> TileSubsetLayer::newFeatureEntry(
    model_ptr<FeatureId> const& featureId,
    model_ptr<GeometryCollection> const& geometry,
    std::span<simfil::ModelNode::Ptr const> values)
{
    validateOwnedNode(featureId, "feature entry id");
    validateOwnedNode(geometry, "feature entry geometry");
    auto valueArray = newValueArray(values);
    auto const index = static_cast<uint32_t>(featureEntries_.size());
    featureEntries_.emplace_back(FeatureEntry::Data{
        featureId->addr(),
        geometry->addr(),
        valueArray->addr(),
    });
    return FeatureEntry(
        &featureEntries_.back(),
        shared_from_this(),
        {ColumnId::FeatureEntries, index},
        mpKey_);
}

model_ptr<AttributeValidityEntry> TileSubsetLayer::newAttributeValidityEntry(
    model_ptr<FeatureId> const& featureId,
    model_ptr<GeometryCollection> const& geometry,
    uint32_t attributeIndex,
    bool hasValidity,
    uint32_t validityIndex,
    uint32_t validityCount,
    std::span<simfil::ModelNode::Ptr const> hostValues,
    std::span<simfil::ModelNode::Ptr const> values,
    std::optional<std::string_view> attributeLayer,
    std::optional<std::string_view> attributeName,
    AttributeValidityEntry::GeometryDescriptionType geometryDescriptionType,
    model_ptr<FeatureId> const& transitionFromFeatureId,
    AttributeValidityEntry::TransitionEnd transitionFromConnectedEnd,
    model_ptr<FeatureId> const& transitionToFeatureId,
    AttributeValidityEntry::TransitionEnd transitionToConnectedEnd,
    uint32_t transitionPivotIndex)
{
    validateOwnedNode(featureId, "attribute-validity feature id");
    validateOwnedNode(geometry, "attribute-validity geometry");
    if (validityCount == 0 || validityIndex >= validityCount) {
        raiseFmt(
            "Validity index {} is invalid for count {}.",
            validityIndex,
            validityCount);
    }
    simfil::ModelNodeAddress transitionFromAddress;
    simfil::ModelNodeAddress transitionToAddress;
    uint8_t transitionConnectedEnds = 0;
    if (geometryDescriptionType == ValidityData::FeatureTransition) {
        validateOwnedNode(
            transitionFromFeatureId,
            "attribute-validity transition source feature id");
        validateOwnedNode(
            transitionToFeatureId,
            "attribute-validity transition target feature id");
        if (!transitionFromFeatureId || !transitionToFeatureId ||
            transitionPivotIndex ==
                AttributeValidityEntry::InvalidTransitionPivotIndex)
        {
            raise(
                "Feature-transition entries require source/target IDs and a pivot index.");
        }
        bool validPivot = false;
        uint32_t lineCount = 0;
        geometry->forEachGeometry([&](auto const& candidate) {
            if (candidate && candidate->geomType() == GeomType::Line) {
                ++lineCount;
                validPivot = transitionPivotIndex < candidate->numPoints();
            }
            return true;
        });
        if (lineCount != 1 || !validPivot) {
            raise(
                "Feature-transition entries require one line and an in-range pivot index.");
        }
        transitionFromAddress = transitionFromFeatureId->addr();
        transitionToAddress = transitionToFeatureId->addr();
        transitionConnectedEnds = static_cast<uint8_t>(
            static_cast<uint8_t>(transitionFromConnectedEnd) |
            (static_cast<uint8_t>(transitionToConnectedEnd) << 1U));
    }
    else {
        transitionPivotIndex =
            AttributeValidityEntry::InvalidTransitionPivotIndex;
    }
    auto hostValueArray = newValueArray(hostValues);
    auto valueArray = newValueArray(values);
    simfil::ModelNodeAddress layerAddress;
    simfil::ModelNodeAddress nameAddress;
    if (attributeLayer) layerAddress = newValue(*attributeLayer)->addr();
    if (attributeName) nameAddress = newValue(*attributeName)->addr();

    auto const index = static_cast<uint32_t>(attributeValidityEntries_.size());
    attributeValidityEntries_.emplace_back(AttributeValidityEntry::Data{
        featureId->addr(),
        geometry->addr(),
        hostValueArray->addr(),
        valueArray->addr(),
        layerAddress,
        nameAddress,
        transitionFromAddress,
        transitionToAddress,
        attributeIndex,
        validityIndex,
        validityCount,
        transitionPivotIndex,
        geometryDescriptionType,
        transitionConnectedEnds,
        hasValidity,
    });
    return AttributeValidityEntry(
        &attributeValidityEntries_.back(),
        shared_from_this(),
        {ColumnId::AttributeValidityEntries, index},
        mpKey_);
}

model_ptr<RelationEntry> TileSubsetLayer::newRelationEntry(
    std::string_view relationId,
    std::string_view name,
    std::string_view provenance,
    RelationDirection direction,
    bool twoway,
    model_ptr<FeatureEntry> const& source,
    model_ptr<FeatureEntry> const& target,
    model_ptr<GeometryCollection> const& sourceGeometry,
    model_ptr<GeometryCollection> const& targetGeometry,
    std::span<simfil::ModelNode::Ptr const> values)
{
    if (relationId.empty()) {
        raise("A relation entry requires a stable relationId.");
    }
    validateOwnedNode(source, "relation source");
    validateOwnedNode(target, "relation target");
    validateOwnedNode(sourceGeometry, "relation source geometry");
    validateOwnedNode(targetGeometry, "relation target geometry");
    auto relationIdNode = newValue(relationId);
    auto nameNode = newValue(name);
    auto provenanceNode = newValue(provenance);
    auto valueArray = newValueArray(values);

    auto const index = static_cast<uint32_t>(relationEntries_.size());
    relationEntries_.emplace_back(RelationEntry::Data{
        relationIdNode->addr(),
        nameNode->addr(),
        provenanceNode->addr(),
        source->addr(),
        target->addr(),
        sourceGeometry->addr(),
        targetGeometry->addr(),
        valueArray->addr(),
        direction,
        twoway,
    });
    return RelationEntry(
        &relationEntries_.back(),
        shared_from_this(),
        {ColumnId::RelationEntries, index},
        mpKey_);
}

model_ptr<GroupEntry> TileSubsetLayer::newGroupEntry(
    simfil::ModelNode::Ptr const& groupKey,
    model_ptr<FeatureId> const& representativeFeatureId,
    model_ptr<GeometryCollection> const& geometry,
    std::span<simfil::ModelNode::Ptr const> values,
    std::span<model_ptr<FeatureId> const> memberFeatureIds)
{
    validateOwnedNode(groupKey, "group key");
    validateOwnedNode(representativeFeatureId, "group representative feature id");
    validateOwnedNode(geometry, "group geometry");
    auto valueArray = newValueArray(values);

    std::vector<model_ptr<FeatureId>> sortedMembers(
        memberFeatureIds.begin(),
        memberFeatureIds.end());
    for (auto const& member : sortedMembers) {
        validateOwnedNode(member, "group member feature id");
    }
    std::stable_sort(
        sortedMembers.begin(),
        sortedMembers.end(),
        [](auto const& left, auto const& right) {
            return left->toString() < right->toString();
        });
    auto members = newArray(std::max<size_t>(1, sortedMembers.size()), true);
    for (auto const& member : sortedMembers) {
        members->append(member);
    }

    auto const index = static_cast<uint32_t>(groupEntries_.size());
    groupEntries_.emplace_back(GroupEntry::Data{
        groupKey->addr(),
        representativeFeatureId->addr(),
        geometry->addr(),
        valueArray->addr(),
        members->addr(),
    });
    return GroupEntry(
        &groupEntries_.back(),
        shared_from_this(),
        {ColumnId::GroupEntries, index},
        mpKey_);
}

model_ptr<FeatureId> TileSubsetLayer::newFeatureId(
    std::string_view const& typeId,
    KeyValueViewPairs const& featureIdParts,
    std::optional<std::string_view> externalMapId)
{
    if (!layerInfo_->validFeatureId(typeId, featureIdParts, false)) {
        raiseFmt(
            "Could not find a matching ID composition of type {} for subset result.",
            typeId);
    }
    auto typeIdString = strings()->emplace(typeId);
    if (!typeIdString) {
        raise(typeIdString.error().message);
    }
    auto const compositionIndex =
        *layerInfo_->matchingFeatureIdCompositionIndex(
            typeId,
            featureIdParts,
            false);
    auto const& composition =
        layerInfo_->getTypeInfo(typeId)->uniqueIdCompositions_[compositionIndex];

    simfil::StringId externalMapIdString = simfil::StringPool::Empty;
    if (externalMapId && *externalMapId != mapId()) {
        auto storedMapId = strings()->emplace(*externalMapId);
        if (!storedMapId) {
            raise(storedMapId.error().message);
        }
        externalMapIdString = *storedMapId;
    }
    auto const address = appendFeatureId(FeatureId::Data{
        false,
        compositionIndex,
        *typeIdString,
        idPartValuesToArrayIndex(*this, composition, featureIdParts),
        externalMapIdString,
    });
    return FeatureId(
        featureIds_.at(address.index()),
        shared_from_this(),
        address,
        mpKey_);
}

model_ptr<GeometryCollection> TileSubsetLayer::newGeometryCollection(
    size_t initialCapacity,
    bool fixedSize)
{
    auto index = arrayMemberStorage().new_array(initialCapacity, fixedSize);
    return GeometryCollection(
        shared_from_this(),
        {ColumnId::GeometryCollections, static_cast<uint32_t>(index)},
        mpKey_);
}

model_ptr<Geometry> TileSubsetLayer::newGeometry(
    GeomType geomType,
    size_t initialCapacity,
    bool fixedSize)
{
    initialCapacity = std::max<size_t>(1, initialCapacity);
    auto makeGeometry = [this](uint8_t column, simfil::ArrayIndex index) {
        return Geometry(
            shared_from_this(),
            {column, static_cast<uint32_t>(index)},
            mpKey_);
    };
    switch (geomType) {
    case GeomType::Points:
        return makeGeometry(
            ColumnId::PointGeometries,
            vertexBufferStorage().new_array(initialCapacity, fixedSize));
    case GeomType::Line:
        return makeGeometry(
            ColumnId::LineGeometries,
            vertexBufferStorage().new_array(initialCapacity, fixedSize));
    case GeomType::Polygon:
        return makeGeometry(
            ColumnId::PolygonGeometries,
            vertexBufferStorage().new_array(initialCapacity, fixedSize));
    case GeomType::Mesh:
        return makeGeometry(
            ColumnId::MeshGeometries,
            vertexBufferStorage().new_array(initialCapacity, fixedSize));
    case GeomType::AABB:
        return makeGeometry(
            ColumnId::AabbGeometries,
            vertexBufferStorage().new_array(2, true));
    case GeomType::GltfNodeIndex:
        return makeGeometry(
            ColumnId::GltfNodeIndexGeometries,
            vertexBufferStorage().new_array(3, true));
    }
    raise("Unsupported geometry type.");
    return {};
}

model_ptr<Geometry> TileSubsetLayer::newGeometryView(
    GeomType geomType,
    uint32_t offset,
    uint32_t size,
    model_ptr<Geometry> const& base)
{
    validateOwnedNode(base, "geometry-view base");
    if (geomType == GeomType::AABB || geomType == GeomType::GltfNodeIndex ||
        base->geomType() == GeomType::AABB ||
        base->geomType() == GeomType::GltfNodeIndex)
    {
        raise("Geometry views require point-buffer-backed geometries.");
    }
    auto const address = appendGeometryView({
        geomType,
        offset,
        size,
        base->addr(),
    });
    return Geometry(
        &geomViews_.at(address.index()),
        shared_from_this(),
        address,
        mpKey_);
}

model_ptr<SourceDataReferenceCollection>
TileSubsetLayer::newSourceDataReferenceCollection(
    std::span<QualifiedSourceDataReference> list)
{
    auto const index = static_cast<uint32_t>(sourceDataReferences_.size());
    auto const size = static_cast<uint32_t>(list.size());
    auto const address = appendSourceDataReferences(list);
    return SourceDataReferenceCollection(
        index,
        size,
        shared_from_this(),
        address,
        mpKey_);
}

tl::expected<void, simfil::Error> TileSubsetLayer::write(
    std::ostream& outputStream)
{
    updateEntryStatistics();
    setInfo("Filter/Geometry/Vertices#count", numVertices());
    if (auto result = TileLayer::write(outputStream); !result) {
        return result;
    }
    bitsery::Serializer<bitsery::OutputStreamAdapter> serializer(outputStream);
    serializer.text1b(filterId_, std::numeric_limits<uint32_t>::max());
    serializer.value8b(generation_);
    serializer.value8b(geometryAnchor_.x);
    serializer.value8b(geometryAnchor_.y);
    serializer.value8b(geometryAnchor_.z);

    std::vector<DependencyWire> dependencyWire;
    dependencyWire.reserve(dependencies_.size());
    for (auto const& dependency : dependencies_) {
        dependencyWire.push_back({
            dependency.sourceTileKey_.toString(),
            dependency.sourceFeatureCount_,
        });
    }
    serializer.container(
        dependencyWire,
        std::numeric_limits<uint32_t>::max(),
        [](auto& nested, DependencyWire& dependency) {
            readWriteDependencyWire(nested, dependency);
        });
    std::vector<IssueWire> issueWire;
    issueWire.reserve(issues_.size());
    for (auto const& issue : issues_) {
        issueWire.push_back({
            issue.channelId_,
            issue.expression_,
            issue.scope_,
            issue.message_,
            issue.occurrenceCount_,
        });
    }
    serializer.container(
        issueWire,
        std::numeric_limits<uint32_t>::max(),
        [](auto& nested, IssueWire& issue) {
            readWriteIssueWire(nested, issue);
        });
    bool hasGlbAttachmentName = glbAttachmentName_.has_value();
    serializer.value1b(hasGlbAttachmentName);
    if (hasGlbAttachmentName) {
        serializer.text1b(
            *glbAttachmentName_,
            std::numeric_limits<uint32_t>::max());
    }
    readWriteDiagnostics(serializer, diagnostics_);
    serializer.object(channels_);
    serializer.object(featureEntries_);
    serializer.object(attributeValidityEntries_);
    serializer.object(relationEntries_);
    serializer.object(groupEntries_);
    serializer.object(traces_);
    readWriteCommonColumns(serializer);
    return ModelPool::write(outputStream);
}

nlohmann::json TileSubsetLayer::toJson() const
{
    auto result = nlohmann::json::object({
        {"type", "TileSubsetLayer"},
        {"mapgetTileId", tileId_.value()},
        {"mapId", mapId_},
        {"mapgetLayerId", layerInfo_->layerId_},
        {"filterId", filterId_},
        {"generation", generation_},
        {"geometryAnchor", {
            geometryAnchor_.x,
            geometryAnchor_.y,
            geometryAnchor_.z,
        }},
        {"info", info_},
        {"dependencies", nlohmann::json::array()},
        {"issues", nlohmann::json::array()},
        {"channels", nlohmann::json::array()},
    });
    if (glbAttachmentName_) {
        result["glbAttachmentName"] = *glbAttachmentName_;
    }
    for (auto const& dependency : dependencies_) {
        result["dependencies"].push_back({
            {"sourceTileKey", dependency.sourceTileKey_.toString()},
            {"sourceFeatureCount", dependency.sourceFeatureCount_},
        });
    }
    for (auto const& issue : issues_) {
        result["issues"].push_back({
            {"channelId", issue.channelId_},
            {"expression", issue.expression_},
            {"scope", issue.scope_},
            {"message", issue.message_},
            {"occurrenceCount", issue.occurrenceCount_},
        });
    }
    auto diagnosticsJson = diagnosticsToJson(diagnostics_);
    if (!diagnosticsJson.empty()) {
        result["diagnostics"] = std::move(diagnosticsJson);
    }
    if (traceCount() > 0) {
        result["traces"] = nlohmann::json::array();
        for (size_t index = 0; index < traceCount(); ++index) {
            result["traces"].push_back(traceAt(index)->toJson());
        }
    }
    forEachChannel([&](auto const& channel) {
        result["channels"].push_back(channel->toJson());
        return true;
    });
    return result;
}

MemoryUsageBreakdown TileSubsetLayer::memoryUsage() const
{
    auto result = TileFeatureModelLayerBase::memoryUsage();
    result.add("subset-layer-object", {
        sizeof(TileSubsetLayer) - sizeof(TileFeatureModelLayerBase),
        sizeof(TileSubsetLayer) - sizeof(TileFeatureModelLayerBase),
    });
    result.add("subset.filter-id", stringMemoryUsage(filterId_));
    result.add("subset.channels", channels_.memory_usage());
    result.add("subset.feature-entries", featureEntries_.memory_usage());
    result.add("subset.attribute-validity-entries", attributeValidityEntries_.memory_usage());
    result.add("subset.relation-entries", relationEntries_.memory_usage());
    result.add("subset.group-entries", groupEntries_.memory_usage());
    result.add("subset.traces", traces_.memory_usage());
    result.add("subset.dependencies", vectorMemoryUsage(dependencies_));
    for (auto const& dependency : dependencies_) {
        result.add("subset.dependency-strings", stringMemoryUsage(dependency.sourceTileKey_.mapId_));
        result.add("subset.dependency-strings", stringMemoryUsage(dependency.sourceTileKey_.layerId_));
    }
    result.add("subset.issues", vectorMemoryUsage(issues_));
    for (auto const& issue : issues_) {
        result.add("subset.issue-strings", stringMemoryUsage(issue.channelId_));
        result.add("subset.issue-strings", stringMemoryUsage(issue.expression_));
        result.add("subset.issue-strings", stringMemoryUsage(issue.message_));
    }
    result.add("subset.diagnostics", diagnosticsMemoryUsage(diagnostics_));
    if (glbAttachmentName_) {
        result.add("subset.glb-attachment-name", stringMemoryUsage(*glbAttachmentName_));
    }
    return result;
}

size_t TileSubsetLayer::size() const
{
    return numRoots();
}

model_ptr<TileSubsetChannel> TileSubsetLayer::at(size_t index) const
{
    auto rootAddress = root(index);
    if (!rootAddress || !*rootAddress) {
        return {};
    }
    return resolve<TileSubsetChannel>(**rootAddress);
}

bool TileSubsetLayer::forEachChannel(
    std::function<bool(model_ptr<TileSubsetChannel> const&)> const& callback) const
{
    if (!callback) {
        return true;
    }
    for (size_t index = 0; index < size(); ++index) {
        auto channel = at(index);
        if (channel && !callback(channel)) {
            return false;
        }
    }
    return true;
}

uint64_t TileSubsetLayer::numVertices() const
{
    return geometryVertexCount();
}

Point TileSubsetLayer::geometryAnchor() const
{
    return geometryAnchor_;
}

void TileSubsetLayer::setGeometryAnchor(Point const& anchor)
{
    geometryAnchor_ = anchor;
}

std::string TileSubsetLayer::nodeString(simfil::ModelNode::Ptr const& node)
{
    return nodeStringValue(node);
}

void TileSubsetLayer::validateOwnedNode(
    simfil::ModelNode::Ptr const& node,
    std::string_view role) const
{
    if (!node) {
        raiseFmt("TileSubsetLayer requires a non-null {}.", role);
    }
    auto owner = node->owningModel();
    if (!owner || owner.get() != this) {
        raiseFmt("TileSubsetLayer {} must belong to the same model.", role);
    }
}

void TileSubsetLayer::updateEntryStatistics()
{
    setInfo("Filter/Channels#count", channels_.size());
    size_t terminalEntries = 0;
    for (size_t index = 0; index < size(); ++index) {
        terminalEntries += at(index)->entryCount();
    }
    setInfo("Filter/Entries/Total#count", terminalEntries);
    setInfo("Filter/Entries/Features#count", featureEntries_.size());
    setInfo(
        "Filter/Entries/Attribute-Validities#count",
        attributeValidityEntries_.size());
    setInfo("Filter/Entries/Relations#count", relationEntries_.size());
    setInfo("Filter/Entries/Groups#count", groupEntries_.size());
}

tl::expected<void, simfil::Error> TileSubsetLayer::resolve(
    simfil::ModelNode const& node,
    ResolveFn const& callback) const
{
    if (auto owner = node.owningModel(); owner && owner.get() != this) {
        return owner->resolve(node, callback);
    }
    switch (node.addr().column()) {
    case ColumnId::SubsetChannels:
        callback(*resolve<TileSubsetChannel>(node));
        return {};
    case ColumnId::FeatureEntries:
        callback(*resolve<FeatureEntry>(node));
        return {};
    case ColumnId::AttributeValidityEntries:
        callback(*resolve<AttributeValidityEntry>(node));
        return {};
    case ColumnId::RelationEntries:
        callback(*resolve<RelationEntry>(node));
        return {};
    case ColumnId::GroupEntries:
        callback(*resolve<GroupEntry>(node));
        return {};
    case ColumnId::FilterTraces:
        callback(*resolve<FilterTrace>(node));
        return {};
    case ColumnId::FeatureIds:
    case ColumnId::ExternalFeatureIds:
        callback(*resolve<FeatureId>(node));
        return {};
    case ColumnId::Points:
    case ColumnId::GeometryPointView:
        callback(*resolve<PointNode>(node));
        return {};
    case ColumnId::PointBuffers:
    case ColumnId::PointBuffersView:
        callback(*resolve<PointBufferNode>(node));
        return {};
    case ColumnId::PointGeometries:
    case ColumnId::LineGeometries:
    case ColumnId::PolygonGeometries:
    case ColumnId::MeshGeometries:
    case ColumnId::AabbGeometries:
    case ColumnId::GltfNodeIndexGeometries:
    case ColumnId::GeometryViews:
        callback(*resolve<Geometry>(node));
        return {};
    case ColumnId::GeometryCollections:
        callback(*resolve<GeometryCollection>(node));
        return {};
    case ColumnId::GeometryArrayView:
        callback(*resolve<GeometryArrayView>(node));
        return {};
    case ColumnId::GeometryBoundsInfoView:
        callback(*resolve<BoundsInfoNode>(node));
        return {};
    case ColumnId::GeometryBoundsPolygonCoordinatesView:
        callback(*resolve<BoundsPolygonCoordinatesNode>(node));
        return {};
    case ColumnId::GeometryBoundsRingView:
        callback(*resolve<BoundsRingNode>(node));
        return {};
    case ColumnId::Polygon:
        callback(*resolve<PolygonNode>(node));
        return {};
    case ColumnId::Mesh:
        callback(*resolve<MeshNode>(node));
        return {};
    case ColumnId::MeshTriangleCollection:
        callback(*resolve<MeshTriangleCollectionNode>(node));
        return {};
    case ColumnId::MeshTriangleLinearRing:
    case ColumnId::LinearRing:
        callback(*resolve<LinearRingNode>(node));
        return {};
    case ColumnId::SourceDataReferenceCollections:
        callback(*resolve<SourceDataReferenceCollection>(node));
        return {};
    case ColumnId::SourceDataReferences:
        callback(*resolve<SourceDataReferenceItem>(node));
        return {};
    default:
        return ModelPool::resolve(node, callback);
    }
}

using simfil::ModelNode;
using simfil::res::tag;

template<>
model_ptr<TileSubsetChannel> resolveInternal(
    tag<TileSubsetChannel>,
    TileSubsetLayer const& model,
    ModelNode const& node)
{
    if (node.addr().column() != TileSubsetLayer::ColumnId::SubsetChannels) {
        raise("Cannot cast this node to a TileSubsetChannel.");
    }
    return TileSubsetChannel(
        const_cast<TileSubsetChannel::Data*>(
            &model.channels_.at(node.addr().index())),
        model.shared_from_this(),
        node.addr(),
        model.mpKey_);
}

template<>
model_ptr<FeatureEntry> resolveInternal(
    tag<FeatureEntry>,
    TileSubsetLayer const& model,
    ModelNode const& node)
{
    if (node.addr().column() != TileSubsetLayer::ColumnId::FeatureEntries) {
        raise("Cannot cast this node to a FeatureEntry.");
    }
    return FeatureEntry(
        const_cast<FeatureEntry::Data*>(
            &model.featureEntries_.at(node.addr().index())),
        model.shared_from_this(),
        node.addr(),
        model.mpKey_);
}

template<>
model_ptr<AttributeValidityEntry> resolveInternal(
    tag<AttributeValidityEntry>,
    TileSubsetLayer const& model,
    ModelNode const& node)
{
    if (node.addr().column() !=
        TileSubsetLayer::ColumnId::AttributeValidityEntries)
    {
        raise("Cannot cast this node to an AttributeValidityEntry.");
    }
    return AttributeValidityEntry(
        const_cast<AttributeValidityEntry::Data*>(
            &model.attributeValidityEntries_.at(node.addr().index())),
        model.shared_from_this(),
        node.addr(),
        model.mpKey_);
}

template<>
model_ptr<RelationEntry> resolveInternal(
    tag<RelationEntry>,
    TileSubsetLayer const& model,
    ModelNode const& node)
{
    if (node.addr().column() != TileSubsetLayer::ColumnId::RelationEntries) {
        raise("Cannot cast this node to a RelationEntry.");
    }
    return RelationEntry(
        const_cast<RelationEntry::Data*>(
            &model.relationEntries_.at(node.addr().index())),
        model.shared_from_this(),
        node.addr(),
        model.mpKey_);
}

template<>
model_ptr<GroupEntry> resolveInternal(
    tag<GroupEntry>,
    TileSubsetLayer const& model,
    ModelNode const& node)
{
    if (node.addr().column() != TileSubsetLayer::ColumnId::GroupEntries) {
        raise("Cannot cast this node to a GroupEntry.");
    }
    return GroupEntry(
        const_cast<GroupEntry::Data*>(
            &model.groupEntries_.at(node.addr().index())),
        model.shared_from_this(),
        node.addr(),
        model.mpKey_);
}

template<>
model_ptr<FilterTrace> resolveInternal(
    tag<FilterTrace>,
    TileSubsetLayer const& model,
    ModelNode const& node)
{
    if (node.addr().column() != TileSubsetLayer::ColumnId::FilterTraces) {
        raise("Cannot cast this node to a FilterTrace.");
    }
    return FilterTrace(
        const_cast<FilterTrace::Data*>(
            &model.traces_.at(node.addr().index())),
        model.shared_from_this(),
        node.addr(),
        model.mpKey_);
}

} // namespace mapget
