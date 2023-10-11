#pragma once

#include "featurelayer.h"

#include <map>
#include <sstream>
#include <shared_mutex>

namespace mapget
{

/**
 * Protocol for binary streaming of TileFeatureLayer and associated
 * Fields dictionary objects. The general stream encoding is a simple
 * Version-Type-Length-Value one:
 * - The version (6b) indicates the protocol version which was used to
 *   serialise the blob. This must be compatible with the current version
 *   which is used by the mapget library.
 * - The type (1B) must be either Fields (1) or TileFeatureLayer (2).
 * - The length (4b)  indicates the byte-length of the serialized object,
 *   which is stored in the value.
 */
class TileLayerStream
{
public:
    enum class MessageType : uint8_t {
        None = 0,
        Fields = 1,
        TileFeatureLayer = 2,
    };

    struct CachedFieldsProvider;

    /** Protocol Version which parsed blobs must be compatible with. */
    static constexpr Version CurrentProtocolVersion{0, 1, 0};

    /** Map to keep track of the highest sent field id per datasource node. */
    using FieldOffsetMap = std::map<std::string, simfil::FieldId>;

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
            std::function<void(TileFeatureLayer::Ptr)> onParsedLayer,
            std::shared_ptr<CachedFieldsProvider> fieldCacheProvider = nullptr);

        /**
         * Add some bytes to parse. The next object will be parsed once
         * sufficient bytes are available.
         */
        void read(std::string_view const& bytes);

        /** end-of-stream: Returns true if the internal buffer is exhausted. */
        [[nodiscard]] bool eos();

        /** Obtain the fields dict provider used by this Reader. */
        std::shared_ptr<CachedFieldsProvider> fieldDictCache();

    private:
        enum class Phase { ReadHeader, ReadValue };

        Phase currentPhase_ = Phase::ReadHeader;
        MessageType nextValueType_ = MessageType::None;
        uint32_t nextValueSize_ = 0;

        bool continueReading();

        std::stringstream buffer_;
        LayerInfoResolveFun layerInfoProvider_;
        std::shared_ptr<CachedFieldsProvider> fieldCacheProvider_;
        std::function<void(TileFeatureLayer::Ptr)> onParsedLayer_;
    };

    /**
     * The Writer turns TileFeatureLayer objects and associated Fields
     * dictionaries into bytes.
     */
    struct Writer
    {
        /**
         * Construct a Writer with a callback for serialized messages,
         * and a reference to a dict which stores the current highest
         * sent FieldId offset per data source node id. Note, that this
         * dictionary will be updated as Fields dictionary updates are sent.
         * Using the same FieldId offset map for two Writer objects will
         * lead to undefined behavior.
         */
        Writer(
            std::function<void(std::string, MessageType)> onMessage,
            FieldOffsetMap& fieldsOffsets);

        /** Serialize a tile feature layer and the required part of a Fields cache. */
        void write(TileFeatureLayer::Ptr const& tileFeatureLayer);

    private:
        void sendMessage(std::string&& bytes, MessageType msgType);

        std::function<void(std::string, MessageType)> onMessage_;
        FieldOffsetMap& fieldsOffsets_;
    };

    /**
     * Cache for Fields-dictionaries. Fields-dicts are unique per data source node,
     * since each data source node may have a uniquely-filled field cache.
     * The default implementation just places an empty Fields-dict into the cache
     * if there is no registered one for the given node id.
     * Derived CachedFieldsProviders may handle uncached Fields dicts differently,
     * e.g. by initializing the Fields object from a cache database.
     */
    struct CachedFieldsProvider
    {
        /** Virtual destructor for memory-safe inheritance */
        virtual ~CachedFieldsProvider() = default;

        /**
         * This operator is called by the Reader to obtain the fields
         * dictionary for a particular node id.
         */
        virtual std::shared_ptr<Fields> operator() (std::string_view const& nodeIf);

        /**
         * Obtain the highest known field id for each data source node id,
         * as currently present in the cache. The resulting dict may be
         * used by a mapget http client to set the `maxKnownFieldIds` info.
         */
        virtual FieldOffsetMap fieldDictOffsets() const;

    protected:
        std::shared_mutex fieldCacheMutex_;
        std::map<std::string, std::shared_ptr<Fields>, std::less<void>> fieldsPerNodeId_;
    };
};

}