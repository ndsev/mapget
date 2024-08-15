#include "stream.h"
#include "sourcedatalayer.h"
#include "info.h"
#include "mapget/log.h"
#include "simfil/model/nodes.h"

#include <bitsery/bitsery.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/traits/string.h>
#include <memory>

#include "featurelayer.h"
#include "sourcedatalayer.h"

namespace mapget
{

TileLayerStream::Reader::Reader(
    LayerInfoResolveFun layerInfoProvider,
    std::function<void(TileLayer::Ptr)> onParsedLayer,
    std::shared_ptr<StringPoolCache> stringPoolProvider)
    : layerInfoProvider_(std::move(layerInfoProvider)),
      stringPoolProvider_(
          stringPoolProvider ? std::move(stringPoolProvider) :
                               std::make_shared<TileLayerStream::StringPoolCache>()),
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
        auto layer = std::make_shared<TileFeatureLayer>(buffer_, layerInfoProvider_, [this](auto&& nodeId) {
            return stringPoolProvider_->getStringPool(nodeId);
        });

        // Calculate duration.
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - start);
        log().trace("Reading {} kB took {} ms.", nextValueSize_/1000, elapsed.count());
        onParsedLayer_(layer);
    }
    else if (nextValueType_ == MessageType::TileSourceDataLayer)
    {
        auto layer = std::make_shared<TileSourceDataLayer>(buffer_, layerInfoProvider_, [this](auto&& nodeId) {
            return stringPoolProvider_->getStringPool(nodeId);
        });
        onParsedLayer_(layer);
    }
    else if (nextValueType_ == MessageType::StringPool)
    {
        // Read the node id which identifies the string pool.
        std::string stringPoolNodeId = StringPool::readDataSourceNodeId(buffer_);
        stringPoolProvider_->getStringPool(stringPoolNodeId)->read(buffer_);
    }

    currentPhase_ = Phase::ReadHeader;
    return true;
}

std::shared_ptr<TileLayerStream::StringPoolCache> TileLayerStream::Reader::stringPoolCache()
{
    return stringPoolProvider_;
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
        raise(fmt::format(
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
    StringPoolOffsetMap& stringPoolOffsets,
    bool differentialStringUpdates)
    : onMessage_(std::move(onMessage)),
      stringPoolOffsets_(stringPoolOffsets),
      differentialStringUpdates_(differentialStringUpdates)
{
}

void TileLayerStream::Writer::write(TileLayer::Ptr const& tileLayer)
{
    if (auto modelPool = std::dynamic_pointer_cast<simfil::ModelPool>(tileLayer)) {
        if (auto strings = modelPool->strings()) {
            auto& highestStringKnownToClient = stringPoolOffsets_[tileLayer->nodeId()];
            auto highestString = strings->highest();

            if (highestStringKnownToClient < highestString)
            {
                // Need to send the client an update for the string pool.
                std::stringstream serializedStrings;
                auto stringUpdateOffset = 0;
                if (differentialStringUpdates_)
                    stringUpdateOffset = highestStringKnownToClient + 1;
                strings->write(serializedStrings, stringUpdateOffset);
                sendMessage(serializedStrings.str(), MessageType::StringPool);
                highestStringKnownToClient = highestString;
            }
        }
    }

    // Send the actual layer
    std::stringstream serializedLayer;
    auto start = std::chrono::system_clock::now();
    tileLayer->write(serializedLayer);
    auto bytes = serializedLayer.str();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - start);
    log().trace("Writing {} kB took {} ms.", bytes.size()/1000, elapsed.count());

    const auto layerType = tileLayer->layerInfo()->type_;
    const auto messageType = [&layerType]() {
        switch (layerType) {
        case mapget::LayerType::Features:
            return MessageType::TileFeatureLayer;
        case mapget::LayerType::SourceData:
            return MessageType::TileSourceDataLayer;
        default:
            raiseFmt("Unsupported layer type: {}", static_cast<int>(layerType));
        }
        return MessageType::None;
    }();

    sendMessage(std::move(bytes), messageType);
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

std::shared_ptr<StringPool> TileLayerStream::StringPoolCache::getStringPool(const std::string_view& nodeId)
{
    {
        std::shared_lock stringPoolReadLock(stringPoolCacheMutex_);
        auto it = stringPoolPerNodeId_.find(std::string(nodeId));
        if (it != stringPoolPerNodeId_.end()) {
            return it->second;
        }
    }
    {
        std::unique_lock stringPoolWriteLock(stringPoolCacheMutex_, std::defer_lock);
        // Was it inserted already now?
        auto it = stringPoolPerNodeId_.find(std::string(nodeId));
        if (it != stringPoolPerNodeId_.end())
            return it->second;
        auto [newIt, _] =
            stringPoolPerNodeId_.emplace(nodeId, std::make_shared<StringPool>(std::string(nodeId)));
        return newIt->second;
    }
}

TileLayerStream::StringPoolOffsetMap TileLayerStream::StringPoolCache::stringPoolOffsets() const
{
    auto result = StringPoolOffsetMap();
    for (auto const& [nodeId, stringPool] : stringPoolPerNodeId_)
        result.emplace(nodeId, stringPool->highest());
    return result;
}

}
