#pragma once

#include "layer.h"
#include "stringpool.h"

#include <map>
#include <sstream>
#include <shared_mutex>

namespace mapget
{

/**
 * Protocol for binary streaming of TileLayer and associated
 * StringPool dictionary objects. The general stream encoding is a simple
 * Version-Type-Length-Value one:
 * - The version (6b) indicates the protocol version which was used to
 *   serialise the blob. This must be compatible with the current version
 *   which is used by the mapget library.
 * - The type (1B) must be one fo the MessageType enum values.
 * - The length (4b)  indicates the byte-length of the serialized object,
 *   which is stored in the value.
 */
class TileLayerStream
{
public:
    enum class MessageType : uint8_t {
        None = 0,
        StringPool = 1,
        TileFeatureLayer = 2,
        TileSourceDataLayer = 3,
        EndOfStream = 128
    };

    struct StringPoolCache;

    /** Protocol Version which parsed blobs must be compatible with. */
    static constexpr Version CurrentProtocolVersion{0, 1, 1};

    /** Map to keep track of the highest sent string id per datasource node. */
    using StringPoolOffsetMap = std::unordered_map<std::string, simfil::StringId>;

    /** The Reader turns bytes into TileLayer objects. */
    struct Reader
    {
        /**
         * Construct a Reader with a callback for parsed result layers,
         * a StringPoolCache which can supply and receive Fields
         * dictionaries for a node id, and a layerInfoProvider which
         * can provide LayerInfo objects for a (map-id, layer-id) combination.
         */
        Reader(
            LayerInfoResolveFun layerInfoProvider,
            std::function<void(TileLayer::Ptr)> onParsedLayer,
            std::shared_ptr<StringPoolCache> stringPoolProvider = nullptr);

        /**
         * Add some bytes to parse. The next object will be parsed once
         * sufficient bytes are available.
         */
        void read(std::string_view const& bytes);

        /** end-of-stream: Returns true if the internal buffer is exhausted. */
        [[nodiscard]] bool eos();

        /** Obtain the string pool cache used by this Reader. */
        std::shared_ptr<StringPoolCache> stringPoolCache();

        /**
         * Read a message header from a stream. Returns true and the next message's type and
         * size, or false, if no sufficient bytes are available. Throws if the protocol version
         * in the header does not match the version currently used by mapget.
         */
        static bool readMessageHeader(std::stringstream& stream, MessageType& outType, uint32_t& outSize);

    private:
        enum class Phase { ReadHeader, ReadValue };

        Phase currentPhase_ = Phase::ReadHeader;
        MessageType nextValueType_ = MessageType::None;
        uint32_t nextValueSize_ = 0;

        /**
         * Reads messages from the current available data.
         * @return True if it can continue and should be called again, false otherwise.
         */
        bool continueReading();

        std::stringstream buffer_;
        LayerInfoResolveFun layerInfoProvider_;
        std::shared_ptr<StringPoolCache> stringPoolProvider_;
        std::function<void(TileLayer::Ptr)> onParsedLayer_;
    };

    /**
     * The Writer turns TileLayer objects and associated StringPools into bytes.
     */
    struct Writer
    {
        /**
         * Construct a Writer with a callback for serialized messages,
         * and a reference to a dict which stores the current highest
         * sent StringId offset per data source node id. Note, that this
         * dictionary will be updated as string poll updates are sent.
         * Using the same StringId offset map for two Writer objects will
         * lead to undefined behavior.
         *
         * Setting differentialStringUpdates=false is necessary when using
         * the Writer with a Cache database, because it is not desirable
         * to store partial StringPool dicts in the database.
         */
        Writer(
            std::function<void(std::string, MessageType)> onMessage,
            StringPoolOffsetMap& stringPoolOffsets,
            bool differentialStringUpdates = true);

        /** Serialize a tile layer and the required part of a StringPool. */
        void write(TileLayer::Ptr const& tileLayer);

        /** Send an EndOfStream message. */
        void sendEndOfStream();

    private:
        void sendMessage(std::string&& bytes, MessageType msgType);

        std::function<void(std::string, MessageType)> onMessage_;
        StringPoolOffsetMap& stringPoolOffsets_;
        bool differentialStringUpdates_ = true;
    };

    /**
     * Cache for string pools. String pools are unique per data source node,
     * since each data source node may have a uniquely-filled string-set.
     * The default implementation just places an empty string pool into the cache
     * if there is no registered one for the given node id.
     * Derived StringPoolCaches may handle uncached StringPools differently,
     * e.g. by initializing the StringPool object from a cache database.
     */
    struct StringPoolCache
    {
        /** Virtual destructor for memory-safe inheritance */
        virtual ~StringPoolCache() = default;

        /**
         * This operator is called by the Reader to obtain the string pool
         * dictionary for a particular node id.
         */
        virtual std::shared_ptr<StringPool> getStringPool(const std::string_view& nodeId);

        /**
         * Obtain the highest known string id for each data source node id,
         * as currently present in the cache. The resulting dict may be
         * used by a mapget http client to set the `stringPoolOffsets` info.
         */
        [[nodiscard]] virtual StringPoolOffsetMap stringPoolOffsets() const;

    protected:
        std::shared_mutex stringPoolCacheMutex_;
        std::map<std::string, std::shared_ptr<StringPool>, std::less<void>> stringPoolPerNodeId_;
    };
};

}
