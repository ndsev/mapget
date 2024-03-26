#include "stream.h"
#include "mapget/log.h"

#include <bitsery/bitsery.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/traits/string.h>

namespace mapget
{

TileLayerStream::Reader::Reader(
    LayerInfoResolveFun layerInfoProvider,
    std::function<void(TileFeatureLayer::Ptr)> onParsedLayer,
    std::shared_ptr<CachedFieldsProvider> fieldCacheProvider)
    : layerInfoProvider_(std::move(layerInfoProvider)),
      cachedFieldsProvider_(
          fieldCacheProvider ? std::move(fieldCacheProvider) :
                               std::make_shared<TileLayerStream::CachedFieldsProvider>()),
      onParsedLayer_(std::move(onParsedLayer))
{
}

void TileLayerStream::Reader::read(const std::string_view& bytes)
{
    buffer_ << bytes;
    while (continueReading());
}

bool TileLayerStream::Reader::eos()
{
    return (buffer_.tellp() - buffer_.tellg()) == 0;
}

bool TileLayerStream::Reader::continueReading()
{
    if (currentPhase_ == Phase::ReadHeader)
    {
        if (readMessageHeader(buffer_, nextValueType_, nextValueSize_)) {
            currentPhase_ = Phase::ReadValue;
        }
        else {
            return false;
        }
    }

    bitsery::Deserializer<bitsery::InputStreamAdapter> s(buffer_);
    auto numUnreadBytes = buffer_.tellp() - buffer_.tellg();
    if (numUnreadBytes < nextValueSize_)
        return false;

    if (nextValueType_ == MessageType::TileFeatureLayer)
    {
        auto start = std::chrono::system_clock::now();
        auto tileFeatureLayer = std::make_shared<TileFeatureLayer>(
            buffer_,
            layerInfoProvider_,
            [this](auto&& nodeId){return (*cachedFieldsProvider_)(nodeId);});
        // Calculate duration.
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - start);
        log().trace("Reading {} kB took {} ms.", nextValueSize_/1000, elapsed.count());
        onParsedLayer_(tileFeatureLayer);
    }
    else if (nextValueType_ == MessageType::Fields)
    {
        // Read the node id which identifies the fields' dictionary.
        std::string fieldsDictNodeId = Fields::readDataSourceNodeId(buffer_);
        (*cachedFieldsProvider_)(fieldsDictNodeId)->read(buffer_);
    }

    currentPhase_ = Phase::ReadHeader;
    return true;
}

std::shared_ptr<TileLayerStream::CachedFieldsProvider> TileLayerStream::Reader::fieldDictCache()
{
    return cachedFieldsProvider_;
}

bool TileLayerStream::Reader::readMessageHeader(std::stringstream & stream, MessageType& outType, uint32_t& outSize)
{
    bitsery::Deserializer<bitsery::InputStreamAdapter> s(stream);
    auto numUnreadBytes = stream.tellp() - stream.tellg();

    // Version: 6B, Type: 1B, Size: 4B
    constexpr auto headerSize = 6 + 1 + 4;
    if (numUnreadBytes < headerSize)
        return false;

    Version protocolVersion;
    s.object(protocolVersion);
    if (!protocolVersion.isCompatible(CurrentProtocolVersion)) {
        throw logRuntimeError(fmt::format(
            "Unable to read message with version {} using version {}.",
            protocolVersion.toString(),
            CurrentProtocolVersion.toString()));
    }
    s.value1b(outType);
    s.value4b(outSize);

    return true;
}

TileLayerStream::Writer::Writer(
    std::function<void(std::string, MessageType)> onMessage,
    FieldOffsetMap& fieldsOffsets,
    bool differentialFieldUpdates)
    : onMessage_(std::move(onMessage)),
      fieldsOffsets_(fieldsOffsets),
      differentialFieldUpdates_(differentialFieldUpdates)
{
}

void TileLayerStream::Writer::write(TileFeatureLayer::Ptr const& tileFeatureLayer)
{
    auto fields = tileFeatureLayer->fieldNames();
    auto& highestFieldKnownToClient = fieldsOffsets_[tileFeatureLayer->nodeId()];
    auto highestField = fields->highest();

    if (highestFieldKnownToClient < highestField)
    {
        // Need to send the client an update for the Fields dictionary
        std::stringstream serializedFields;
        auto fieldUpdateOffset = 0;
        if (differentialFieldUpdates_)
            fieldUpdateOffset = highestFieldKnownToClient+1;
        fields->write(serializedFields, fieldUpdateOffset);
        sendMessage(serializedFields.str(), MessageType::Fields);
        highestFieldKnownToClient = highestField;
    }

    // Send actual tileFeatureLayer
    std::stringstream serializedFeatureLayer;
    auto start = std::chrono::system_clock::now();
    tileFeatureLayer->write(serializedFeatureLayer);
    auto bytes = serializedFeatureLayer.str();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - start);
    log().trace("Writing {} kB took {} ms.", bytes.size()/1000, elapsed.count());
    sendMessage(std::move(bytes), MessageType::TileFeatureLayer);
}

void TileLayerStream::Writer::sendMessage(std::string&& bytes, TileLayerStream::MessageType msgType)
{
    // TODO refactor the preparation of tile layer & field dicts storage format
    //  such that the encoding logic is not split over multiple functions.

    std::stringstream message;
    bitsery::Serializer<bitsery::OutputStreamAdapter> s(message);

    // Write protocol version
    s.object(CurrentProtocolVersion);

    // Write message type
    s.value1b(msgType);

    // Write content length
    s.value4b((uint32_t)bytes.size());

    // Write content
    message << bytes;

    // Notify result
    onMessage_(message.str(), msgType);
}

void TileLayerStream::Writer::sendEndOfStream()
{
    sendMessage("", MessageType::EndOfStream);
}

std::shared_ptr<Fields> TileLayerStream::CachedFieldsProvider::operator()(const std::string_view& nodeId)
{
    {
        std::shared_lock fieldCacheReadLock(fieldCacheMutex_);
        auto it = fieldsPerNodeId_.find(std::string(nodeId));
        if (it != fieldsPerNodeId_.end()) {
            return it->second;
        }
    }
    {
        std::unique_lock fieldCacheWriteLock(fieldCacheMutex_, std::defer_lock);
        // Was it inserted already now?
        auto it = fieldsPerNodeId_.find(std::string(nodeId));
        if (it != fieldsPerNodeId_.end())
            return it->second;
        auto [newIt, _] =
            fieldsPerNodeId_.emplace(nodeId, std::make_shared<Fields>(std::string(nodeId)));
        return newIt->second;
    }
}

TileLayerStream::FieldOffsetMap TileLayerStream::CachedFieldsProvider::fieldDictOffsets() const
{
    auto result = FieldOffsetMap();
    for (auto const& [nodeId, fieldsDict] : fieldsPerNodeId_)
        result.emplace(nodeId, fieldsDict->highest());
    return result;
}

}