#pragma once

#include "layer.h"
#include "stringpool.h"
#include "incoming-relations.h"

#include <map>
#include <set>
#include <shared_mutex>
#include <span>
#include <vector>

namespace mapget
{

/**
 * Protocol for binary streaming of PartitionLayer and associated
 * StringPool dictionary objects. The general stream encoding is a simple
 * Version-Type-Length-Value one:
 * - The version (6 bytes) indicates the protocol version which was used to
 *   serialise the blob. This must be compatible with the current version
 *   which is used by the mapget library.
 * - The type (1 byte) identifies a MessageType enum value.
 * - The length (4 bytes) indicates the byte-length of the serialized object,
 *   which is stored in the value.
 */
class TileLayerStream
{
public:
    enum class MessageType : uint8_t {
        None = 0,
        StringPool = 1,
        PartitionFeatureLayer = 2,
        PartitionSourceDataLayer = 3,
        TileFeatureLayer = PartitionFeatureLayer,
        TileSourceDataLayer = PartitionSourceDataLayer,
        /**
         * JSON-encoded status updates, e.g. for WebSocket /tiles.
         *
         * Payload: UTF-8 JSON bytes (not null-terminated).
         */
        Status = 4,
        /**
         * JSON-encoded load-state updates for individual tiles.
         *
         * Payload: UTF-8 JSON bytes (not null-terminated).
         */
        LoadStateChange = 5,
        /**
         * JSON-encoded request-context marker for WebSocket /tiles streams.
         *
         * Payload: UTF-8 JSON bytes (not null-terminated).
         */
        RequestContext = 6,
        /** Binary PartitionSubsetLayer payload for server-evaluated filter streams. */
        PartitionSubsetLayer = 7,
        TileSubsetLayer = PartitionSubsetLayer,
        /**
         * JSON-encoded datasource catalog invalidation for interactive streams.
         *
         * Payload: UTF-8 JSON bytes (not null-terminated).
         */
        SourceCatalogChange = 8,
        /** UTF-8 JSON browser-action control, independent of tile request and outbox state. */
        ActionControl = 9,
        /** Typed JSON incoming-relation manifest, accompanied by feature snapshot frames. */
        IncomingRelations = 10,
        EndOfStream = 128
    };

    struct StringPoolCache;

    /**
     * Protocol Version which parsed blobs must be compatible with.
     * Version History:
     * - Version 1.0:
     *   + Added PartitionFeatureLayer Message
     *   + Added StringPool Message
     *   + Added PartitionSourceDataLayer Message
     *   + Added EndOfStream Message
     * - Version 1.1:
     *   + Added errorCode field to PartitionLayer
     *   + Added Status Message
     *   + Added LoadStateChange Message
     * - Version 1.2:
     *   - Removed LoadStateChange Message.
     *   + Added tile load stage.
     *   + Feature geometry reference may point directly to a Geometry
     *     (single-geometry fast-path) or to a GeometryCollection.
     * - Version 1.3:
     *   + Added tile-level binary attachments to PartitionFeatureLayer.
     * - Version 1.4:
     *   + Added AABB and GltfNodeIndex geometry kinds.
     *   + Added point-buffer-backed GLTF node-index geometry storage.
     * - Version 1.5:
     *   + Replaced generic tile attachments with one optional tile-level GLB attachment.
     * - Version 1.6:
     *   + Added SIMFIL object/array TypeId columns to ModelPool payloads.
     * - Version 1.7:
     *   + Added TileSearchResultLayer Message.
     * - Version 1.8:
     *   + Added explicit polygon ring metadata for hole-aware polygons.
     * - Version 1.9:
     *   + Added typed SIMFIL trace aggregates to TileSearchResultLayer.
     *   + Added SourceCatalogChange control messages.
     * - Version 2.0:
     *   + Adopted NDS.Live-compatible packed tile IDs.
     * - Version 2.1:
     *   + Added presentation address scopes to SourceData compound nodes.
     * - Version 3.0:
     *   - Removed staged tile loading and feature LOD.
     *   - Replaced geometry stage bytes with layer-local semantic name indices.
     *   - Renamed datasource node identity to string-pool identity.
     *   - Replaced TileSearchResultLayer with multi-channel PartitionSubsetLayer.
     * - Version 3.1:
     *   + Added per-output delivery epochs to PartitionSubsetLayer.
     * - Version 3.2:
     *   + Added shared AttrPointSequence definitions and AttrPoint index/range
     *     validity descriptions.
     * - Version 3.3:
     *   + Added tile-scoped feature-ID expressions to portable locate selectors.
     * - Version 4.0:
     *   - Removed delivery epochs from PartitionSubsetLayer. Interactive delivery
     *     ownership is now represented by pending snapshots and handoff state.
     * - Version 5.0:
     *   + Tagged tile/object partition identities in layers and subset dependencies.
     * - Version 5.1:
     *   - Feature ID index entries no longer require sorted wire order.
     * - Version 5.2:
     *   + Common ordered warnings on partition layers.
     * - Version 5.3:
     *   - Interactive client IDs are opaque UUIDv4 strings instead of integers.
     *   + Added the independent ActionControl message for browser action relays.
     *   + Projection slots contain result sequences and indexed error metadata.
     *   + Native undefined model values; core model column tags shift accordingly.
     * - Version 5.4:
     *   + Combined the released 5.2 warning header with the 5.3 action/projection
     *     format, which previously evolved without that header on the release branch.
     * - Version 5.5:
     *   + Added typed incoming-relation manifests with ordinary feature snapshots.
     * - Version 6.0:
     *   + Exact MapSchema identities in partition headers, separate binary schema artifacts.
     *   + Schema-bound ModelPool values, persistent bitmasks and typed scalar projections.
     * - Version 6.1:
     *   + FeatureId parts use tagged array/object addresses; foreign references retain named parts.
     *   + Shared SchemaPool graphs distinguish contextual edges and composed type remappings.
     * - Version 6.2:
     *   + Schema raw-value metadata extends simfil's static dictionary; mapget field IDs shift.
     *   + MapSchema codec 3 / SchemaPool codec 2 carry native relation contracts and enum ordinals.
     * - Version 6.3:
     *   + Canonical schema field multiplicity extends simfil's static dictionary;
     *     mapget field IDs shift and require matched native/WASM readers.
     *   + MapSchema codec 7 / SchemaPool codec 4 use canonical columns and arenas.
     */
    static constexpr Version CurrentProtocolVersion{6, 3, 0};

    /** Highest sent dictionary ID per source string-pool namespace. */
    using StringPoolOffsetMap = std::unordered_map<std::string, simfil::StringId>;

    /** The Reader turns bytes into PartitionLayer objects. */
    struct Reader
    {
        /**
         * Construct a Reader with a callback for parsed result layers, a dictionary
         * cache and a provider of installed LayerInfo by (mapId, layerId).
         * Resolve dictionaries by stringPoolId and deliver decoded layers
         * synchronously from read().
         *
         * schemas resolves exact installed artifacts for nested model bindings;
         * it must not fetch missing schemas. An omitted dictionary cache creates
         * a reader-local cache. Control-message string_views last only for the
         * callback; copy them before retaining or dispatching them asynchronously.
         */
        Reader(
            LayerInfoResolveFun layerInfoProvider,
            std::function<void(PartitionLayer::Ptr)> onParsedLayer,
            std::shared_ptr<StringPoolCache> stringPoolProvider = nullptr,
            std::function<void(MessageType, std::string_view)> onControlMessage = {},
            SchemaResolveFun schemas = {});

        /**
         * Add some bytes to parse. The next object will be parsed once
         * sufficient bytes are available. Calls and callbacks are synchronous;
         * serialize access to one Reader. Decode errors propagate to the caller.
         */
        void read(std::string_view const& bytes);

        /**
         * True when no buffered bytes remain. This does not prove that an
         * EndOfStream frame was received or that a pending payload is complete.
         */
        [[nodiscard]] bool eos();

        /** Obtain the string pool cache used by this Reader. */
        std::shared_ptr<StringPoolCache> stringPoolCache();

        /**
         * Read a message header from a stream. Returns true and the next message's type and
         * size, or false, if no sufficient bytes are available. Throws if the protocol version
         * in the header does not match the version currently used by mapget.
         */
        static bool readMessageHeader(
            std::span<const uint8_t> bytes,
            MessageType& outType,
            uint32_t& outSize,
            size_t* bytesRead = nullptr);

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

        std::vector<uint8_t> buffer_;
        size_t readOffset_ = 0;
        LayerInfoResolveFun layerInfoProvider_;
        SchemaResolveFun schemas_;
        std::shared_ptr<StringPoolCache> stringPoolProvider_;
        std::function<void(PartitionLayer::Ptr)> onParsedLayer_;
        std::function<void(MessageType, std::string_view)> onControlMessage_;
    };

    /**
     * The Writer turns PartitionLayer objects and associated StringPools into bytes.
     */
    struct Writer
    {
        /**
         * Construct a Writer with a callback for serialized messages and a reference
         * to the sent-string offset map. Complete framed messages are delivered
         * synchronously through onMessage; dictionary updates advance the offsets.
         * The map must outlive the writer and records the highest dictionary ID already
         * sent for each stringPoolId. Writers sharing it must serialize access and
         * target the same receiver state.
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
        void write(PartitionLayer::Ptr const& tileLayer);

        /** Send a JSON status message through the binary stream. */
        void sendStatus(std::string statusJson);

        /** Send a backend-associated incoming inspection manifest. */
        void sendIncomingRelations(IncomingRelationCollection const& collection);

        /** Send an EndOfStream message. */
        void sendEndOfStream();

    private:
        /** Prefix a payload with the fixed wire header and deliver one contiguous frame. */
        void sendMessage(std::string&& bytes, MessageType msgType);

        std::function<void(std::string, MessageType)> onMessage_;
        StringPoolOffsetMap& stringPoolOffsets_;
        bool differentialStringUpdates_ = true;
    };

    /**
     * Dictionaries keyed by the producer's stringPoolId namespace. Multiple
     * tiles from that producer share IDs, while unrelated namespaces must stay
     * separate. First access creates a pool containing mapget's static keys;
     * later in-band dictionary messages add producer-defined strings.
     * Derived StringPoolCaches may handle uncached StringPools differently,
     * e.g. by initializing the StringPool object from a cache database.
     */
    struct StringPoolCache
    {
        /** Virtual destructor for memory-safe inheritance */
        virtual ~StringPoolCache() = default;

        /**
         * This operator is called by the Reader to obtain the string pool
         * dictionary for a particular stringPoolId.
         */
        virtual std::shared_ptr<StringPool> getStringPool(const std::string_view& stringPoolId);

        /**
         * Obtain the highest known string ID for each stringPoolId,
         * as currently present in the cache. The resulting dict may be
         * used by a mapget http client to set the `stringPoolOffsets` info.
         */
        [[nodiscard]] virtual StringPoolOffsetMap stringPoolOffsets() const;

        /**
         * Release cache ownership of obsolete dictionaries after a catalog edit.
         * Decoded layers retain their shared dictionaries until their own release.
         */
        void retainStringPools(std::set<std::string> const& stringPoolIds);

    protected:
        mutable std::shared_mutex stringPoolCacheMutex_;
        std::map<std::string, std::shared_ptr<StringPool>, std::less<void>> stringPoolPerStringPoolId_;
    };
};

}
