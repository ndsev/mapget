#include "featurelayer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

#include <picosha2.h>
#include "simfil/model/schema-pool.h"

namespace mapget
{
namespace
{

/**
 * Fingerprint the published feature graph in source order. The same logical
 * tile can have different dictionary IDs, allocator addresses, index buckets
 * and expiry times on a cold load. None of those participate in this encoding.
 * Length-prefixed strings and fixed little-endian numbers make the stream
 * unambiguous and reproducible in native and WASM builds.
 */
class SourceSnapshotDigest
{
public:
    explicit SourceSnapshotDigest(PartitionFeatureLayer const& layer) : layer_(layer) {}

    std::string calculate()
    {
        text("mapget-feature-snapshot-v1");
        text(layer_.id().toString());
        text(layer_.schemaId());
        point(layer_.geometryAnchor());
        text(layer_.glbAttachmentName().value_or(""));
        number(layer_.size());
        for (auto const& feature : layer_)
            host(*feature);
        number(layer_.numAttrPointSequences());
        for (uint32_t i = 0; i < layer_.numAttrPointSequences(); ++i)
            node(*layer_.attrPointSequenceAt(i));
        flush();
        hash_.finish();
        return "sha256:" + picosha2::get_hash_hex_string(hash_);
    }

private:
    /** Batch small fields and feed large payloads in chunks with bounded scratch storage. */
    void flush()
    {
        hash_.process(buffer_.begin(), buffer_.begin() + used_);
        used_ = 0;
    }

    void bytes(std::string_view data)
    {
        while (!data.empty()) {
            auto count = std::min(data.size(), buffer_.size() - used_);
            std::memcpy(buffer_.data() + used_, data.data(), count);
            used_ += count;
            data.remove_prefix(count);
            if (used_ == buffer_.size())
                flush();
        }
    }

    void byte(uint8_t value)
    {
        buffer_[used_++] = value;
        if (used_ == buffer_.size())
            flush();
    }

    void number(uint64_t value)
    {
        for (unsigned i = 0; i < 8; ++i) {
            byte(static_cast<uint8_t>(value));
            value >>= 8;
        }
    }

    void text(std::string_view value)
    {
        number(value.size());
        bytes(value);
    }

    void point(Point const& p)
    {
        for (auto value : {p.x, p.y, p.z})
            number(std::bit_cast<uint64_t>(value));
    }

    void optionalNode(simfil::ModelNode::Ptr const& value)
    {
        byte(bool(value));
        if (value)
            node(*value);
    }

    /** Keep the map and typed parts of a reference, including secondary and foreign IDs. */
    void identity(FeatureId const& id)
    {
        text(id.typeId());
        auto map = id.externalMapId();
        byte(map.has_value());
        if (map)
            text(*map);
        auto parts = id.keyValuePairs();
        number(parts.size());
        for (auto const& [key, value] : parts) {
            text(key);
            byte(static_cast<uint8_t>(value.index()));
            if (auto integer = std::get_if<int64_t>(&value))
                number(static_cast<uint64_t>(*integer));
            else
                text(std::get<std::string_view>(value));
        }
    }

    /** The source feature owns geometry slots, including repeated or coincident lines. */
    void host(Feature const& feature)
    {
        identity(*feature.id());
        number(feature.schemaType().id());
        optionalNode(feature.sourceDataReferences());
        auto geometries = feature.geomOrNull();
        number(geometries ? geometries->numGeometries() : 0);
        if (geometries) {
            geometries->forEachGeometry([&](model_ptr<Geometry> const& item) {
                geometry(*item);
                return true;
            });
        }
        optionalNode(feature.attributesOrNull());
        optionalNode(feature.attributeLayersOrNull());
        number(feature.numRelations());
        feature.forEachRelation([&](model_ptr<Relation> const& relation) {
            node(*relation);
            return true;
        });
    }

    /** Include the original buffer slice and polygon rings before GeoJSON closes or triangulates them. */
    void geometry(Geometry const& value)
    {
        byte(static_cast<uint8_t>(value.geomType()));
        text(value.name().value_or(""));
        optionalNode(value.sourceDataReferences());
        if (value.geomType() == GeomType::GltfNodeIndex) {
            number(value.gltfNodeIndex());
            point(value.gltfNodeAabbOrigin());
            point(value.gltfNodeAabbSize());
        } else if (value.geomType() == GeomType::AABB) {
            point(value.aabbOrigin());
            point(value.aabbSize());
        } else {
            auto const rings = value.numPolygonRings();
            number(rings);
            for (uint32_t i = 0; i < rings; ++i)
                number(value.polygonRingStart(i));
            number(value.numPoints());
            value.forEachPoint([&](Point const& p) {
                point(p);
                return true;
            });
        }
    }

    /**
     * Preserve stored field order and repeats (both distinguish attribute
     * occurrences). A back edge in a datasource-created graph is encoded as
     * the distance to its active ancestor, independent of its memory address.
     */
    void node(simfil::ModelNode const& value)
    {
        using Column = PartitionFeatureLayer::ColumnId;
        auto const column = value.addr().column();
        auto const type = value.type();
        byte(static_cast<uint8_t>(type));
        auto const schema = value.schemaType();
        number(schema.id());
        if (schema)
            text(schema.pool()->identity() == layer_.schemaId() ? std::string_view{} : schema.pool()->identity());

        // A local FeatureId exposes a string to SIMFIL. Its typed identity and
        // an ordinary string with that spelling are distinct inspection data.
        // Resolve every model-specific node in its own pool, including nodes
        // supplied by a different partition through a merged feature view.
        auto const* owner = value.borrowedModel();
        if (column == Column::FeatureIds || column == Column::ExternalFeatureIds) {
            byte('I');
            identity(*owner->resolve<FeatureId>(value));
            return;
        }
        byte('N');
        if (type != simfil::ValueType::Object && type != simfil::ValueType::Array) {
            std::visit([&](auto const& scalar) {
                using T = std::decay_t<decltype(scalar)>;
                if constexpr (std::is_same_v<T, double>)
                    number(std::bit_cast<uint64_t>(scalar));
                else if constexpr (std::is_integral_v<T>)
                    number(static_cast<uint64_t>(scalar));
                else if constexpr (std::is_convertible_v<T, std::string_view>)
                    text(scalar);
                else if constexpr (std::is_same_v<T, simfil::ByteArray>)
                    text(scalar.bytes);
                else if constexpr (std::is_same_v<T, simfil::Bitmask>) {
                    number(scalar.bits);
                    number(scalar.type.id());
                    if (scalar.type)
                        text(scalar.type.pool()->identity());
                }
            }, value.value());
            return;
        }

        auto key = std::pair{owner, value.addr().value_};
        auto ancestor = std::find(ancestors_.begin(), ancestors_.end(), key);
        if (ancestor != ancestors_.end()) {
            byte('R');
            number(ancestors_.end() - ancestor);
            return;
        }
        ancestors_.push_back(key);
        if (column == Column::GeometryCollections || column == Column::FeatureGeometryCollectionView) {
            byte('G');
            auto collection = owner->resolve<GeometryCollection>(value);
            number(collection->numGeometries());
            collection->forEachGeometry([&](model_ptr<Geometry> const& member) {
                geometry(*member);
                return true;
            });
        } else if (column == Column::GeometryViews ||
                   (column >= Column::PointGeometries && column <= Column::GltfNodeIndexGeometries)) {
            byte('g');
            geometry(*owner->resolve<Geometry>(value));
        } else {
            if (column == Column::Attributes) {
                // Source-record links are available to inspection through the
                // native attribute API independently of the query field list.
                byte('a');
                auto attribute = owner->resolve<Attribute>(value);
                text(attribute->name());
                optionalNode(attribute->sourceDataReferences());
            } else if (column == Column::Validities) {
                // Certain validity kinds project only their geometric description.
                // An explicit host selector still determines which source can use it.
                byte('V');
                auto validity = owner->resolve<Validity>(value);
                byte(validity->geometryDescriptionType());
                optionalNode(validity->featureId());
            } else {
                byte('v');
            }
            auto count = value.size();
            number(count);
            for (uint32_t i = 0; i < count; ++i) {
                if (type == simfil::ValueType::Object)
                    text(owner->lookupStringId(value.keyAt(i)).value_or(""));
                optionalNode(value.at(i));
            }
        }
        ancestors_.pop_back();
    }

    PartitionFeatureLayer const& layer_;
    picosha2::hash256_one_by_one hash_;
    std::array<uint8_t, 4096> buffer_{};
    size_t used_ = 0;
    std::vector<std::pair<simfil::Model const*, uint32_t>> ancestors_;
};
}

std::string PartitionFeatureLayer::sourceRevision() const
{
    // An errored conversion can leave incomplete nodes. It can carry diagnostics
    // through the normal tile path, but its ordinals cannot identify a snapshot.
    if (error())
        return {};
    if (!sourceRevision_.empty())
        return sourceRevision_;
    return SourceSnapshotDigest(*this).calculate();
}

void PartitionFeatureLayer::publishSourceSnapshot(std::string sourceId, std::string revision)
{
    if (revision.empty() && !error())
        revision = SourceSnapshotDigest(*this).calculate();
    sourceRevision_ = std::move(revision);
    sourceId_ = std::move(sourceId);
}

} // namespace mapget
