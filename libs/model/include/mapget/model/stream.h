#pragma once

#include "featurelayer.h"

#include <map>
#include <sstream>

namespace mapget
{

/**
 * Protocol for binary streaming of TileFeatureLayer and associated
 * Fields dictionary objects. The general stream encoding is a simple
 * Version-Type-Length-Value one:
 * - The version (6b) indicates the protocol version which was used to
 *   serialise the blob.
 * - The type (1B) must be either Fields (1) or TileFeatureLayer (2).
 * - The length (4b)  indicates the byte-length of the serialized object,
 *   which is stored in the value.
 */
class TileLayerStream
{
    enum class MessageType : uint8_t {
        None = 0,
        Fields = 1,
        TileFeatureLayer = 2,
    };

public:
    struct CachedFieldsProvider;

    /** Protocol Version which parsed blobs must be compatible with. */
    static constexpr Version CurrentProtocolVersion{0, 1, 0};

    /** The Reader turns bytes into TileFeatureLayer objects. */
    struct Reader
    {
        /**
         * Construct a Reader with a callback for parsed result layers,
         * a CachedFieldsProvider which can supply and receive Fields
         * dictionaries for a node id, and a layerInfoProvider which
         * can provide LayerInfo objects for a (map-id, layer-id) combination.
         */
        Reader(
            LayerInfoResolveFun layerInfoProvider,
            std::function<void(std::shared_ptr<TileFeatureLayer>)> onParsedLayer,
            std::shared_ptr<CachedFieldsProvider> fieldCacheProvider);

        /**
         * Add some bytes to parse. The next object will be parsed once
         * sufficient bytes are available.
         */
        void read(std::vector<uint8_t> const& bytes);

        /** Returns true if the internal buffer is exhausted. */
        [[nodiscard]] bool eol();

    private:
        enum class Phase { ReadHeader, ReadValue };

        Phase currentPhase_ = Phase::ReadHeader;
        MessageType nextValueType_ = MessageType::None;
        uint32_t nextValueSize_ = 0;

        bool continueReading();

        std::stringstream buffer_;
        LayerInfoResolveFun layerInfoProvider_;
        std::shared_ptr<CachedFieldsProvider> fieldCacheProvider_;
        std::function<void(std::shared_ptr<TileFeatureLayer>)> onParsedLayer_;
    };

    /**
     * The Writer turns TileFeatureLayer objects and associated Fields
     * dictionaries into bytes.
     */
    struct Writer
    {
        /**
         * Construct a Writer with a callback for serialized result bytes,
         * and an oracle function which can tell the writer how much of
         * a Fields dictionary needs to be re-sent for a certain node id.
         */
        Writer(
            std::function<void(std::string)> onSerializedBytes,
            std::function<simfil::FieldId(std::string)> fieldsOffsetProvider);

        /** Serialize a tile feature layer and the required part of a Fields cache. */
        void write(std::shared_ptr<TileFeatureLayer> tileFeatureLayer);

    private:
        std::function<void(std::string)> onSerializedBytes_;
        std::function<simfil::FieldId(std::string)> fieldsOffsetProvider_;
    };

    /**
     * Cache for Fields-dictionaries. Fields dictionaries are unique per node,
     * since each data source node may have a uniquely-filled field cache.
     * The reader will insert parsed Fields objects into the fieldsPerNodeId_ map.
     */
    struct CachedFieldsProvider
    {
        virtual std::shared_ptr<Fields> operator() (std::string_view const&);
        std::map<std::string, std::shared_ptr<Fields>> fieldsPerNodeId_;
    };
};

}