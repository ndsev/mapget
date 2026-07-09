#include "stream.h"
#include "sourcedatalayer.h"
#include "info.h"
#include "mapget/log.h"
#include "simfil/model/nodes.h"

#include <bitsery/bitsery.h>
#include <bitsery/adapter/buffer.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/traits/string.h>
#include <chrono>
#include <memory>

#include "featurelayer.h"
#include "searchresultlayer.h"
#include "sourcedatalayer.h"

namespace mapget
{

TileLayerStream::Reader::Reader(
    LayerInfoResolveFun layerInfoProvider,
    std::function<void(TileLayer::Ptr)> onParsedLayer,
    std::shared_ptr<StringPoolCache> stringPoolProvider,
    std::function<void(MessageType, std::string_view)> onControlMessage)
    : layerInfoProvider_(std::move(layerInfoProvider)),
      stringPoolProvider_(
          stringPoolProvider ? std::move(stringPoolProvider) :
                               std::make_shared<TileLayerStream::StringPoolCache>()),
      onParsedLayer_(std::move(onParsedLayer)),
      onControlMessage_(std::move(onControlMessage))
{
}

void TileLayerStream::Reader::read(const std::string_view& bytes)
{
    buffer_.insert(
        buffer_.end(),
        reinterpret_cast<const uint8_t*>(bytes.data()),
        reinterpret_cast<const uint8_t*>(bytes.data()) + bytes.size());
    while (continueReading()) {}

    if (readOffset_ == buffer_.size()) {
        // Fully consumed buffers are reset eagerly so long-running streams do
        // not retain capacity proportional to peak message size.
        buffer_.clear();
        readOffset_ = 0;
    }
    else if (readOffset_ > 65536 && (readOffset_ * 2 > buffer_.size())) {
        // For partial consumption, compact only once the consumed prefix is
        // large enough to pay for the memmove.
        buffer_.erase(
            buffer_.begin(),
            buffer_.begin() + static_cast<std::ptrdiff_t>(readOffset_));
        readOffset_ = 0;
    }
}

bool TileLayerStream::Reader::eos()
{
    return readOffset_ == buffer_.size();
}

bool TileLayerStream::Reader::continueReading()
{
    if (currentPhase_ == Phase::ReadHeader)
    {
        // The stream is framed, so parsing alternates strictly between header
        // and payload phases until the current message is complete.
        size_t headerBytesRead = 0;
        auto unreadBytes = std::span<const uint8_t>(buffer_).subspan(readOffset_);
        if (readMessageHeader(unreadBytes, nextValueType_, nextValueSize_, &headerBytesRead)) {
            readOffset_ += headerBytesRead;
            currentPhase_ = Phase::ReadValue;
        }
        else {
            return false;
        }
    }

    auto numUnreadBytes = buffer_.size() - readOffset_;
    if (numUnreadBytes < nextValueSize_)
        return false;

    std::vector<uint8_t> payload(
        buffer_.begin() + static_cast<std::ptrdiff_t>(readOffset_),
        buffer_.begin() + static_cast<std::ptrdiff_t>(readOffset_ + nextValueSize_));
    readOffset_ += nextValueSize_;

    if (nextValueType_ == MessageType::TileFeatureLayer)
    {
        auto start = std::chrono::system_clock::now();
        auto layer = std::make_shared<TileFeatureLayer>(payload, layerInfoProvider_, [this](auto&& nodeId) {
            return stringPoolProvider_->getStringPool(nodeId);
        });

        // Calculate duration.
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - start);
        log().trace("Reading {} kB took {} ms.", nextValueSize_/1000, elapsed.count());
        onParsedLayer_(layer);
    }
    else if (nextValueType_ == MessageType::TileSearchResultLayer)
    {
        auto layer = std::make_shared<TileSearchResultLayer>(payload, layerInfoProvider_, [this](auto&& nodeId) {
            return stringPoolProvider_->getStringPool(nodeId);
        });
        onParsedLayer_(layer);
    }
    else if (nextValueType_ == MessageType::TileSourceDataLayer)
    {
        auto layer = std::make_shared<TileSourceDataLayer>(payload, layerInfoProvider_, [this](auto&& nodeId) {
            return stringPoolProvider_->getStringPool(nodeId);
        });
        onParsedLayer_(layer);
    }
    else if (nextValueType_ == MessageType::StringPool)
    {
        // String-pool updates are applied in-band so subsequent layer payloads
        // in the same stream can resolve freshly introduced string ids.
        size_t nodeIdBytesRead = 0;
        auto stringPoolNodeId = StringPool::readDataSourceNodeId(payload, 0, &nodeIdBytesRead);
        auto result = stringPoolProvider_->getStringPool(stringPoolNodeId)->read(payload, nodeIdBytesRead);
        if (!result) {
            raise(result.error().message);
        }
    }
    else if (onControlMessage_
             && (nextValueType_ == MessageType::Status
                 || nextValueType_ == MessageType::LoadStateChange
                 || nextValueType_ == MessageType::RequestContext
                 || nextValueType_ == MessageType::SourceCatalogChange
                 || nextValueType_ == MessageType::EndOfStream))
    {
        onControlMessage_(
            nextValueType_,
            std::string_view(
                reinterpret_cast<const char*>(payload.data()),
                payload.size()));
    }

    currentPhase_ = Phase::ReadHeader;
    return true;
}

std::shared_ptr<TileLayerStream::StringPoolCache> TileLayerStream::Reader::stringPoolCache()
{
    return stringPoolProvider_;
}

bool TileLayerStream::Reader::readMessageHeader(
    std::span<const uint8_t> bytes,
    MessageType& outType,
    uint32_t& outSize,
    size_t* bytesRead)
{
    // Version: 6B, Type: 1B, Size: 4B
    constexpr size_t headerSize = 6 + 1 + 4;
    if (bytes.size() < headerSize)
        return false;

    std::vector<uint8_t> header(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(headerSize));
    using Adapter = bitsery::InputBufferAdapter<std::vector<uint8_t>>;
    bitsery::Deserializer<Adapter> s(Adapter(header.begin(), header.end()));

    Version protocolVersion;
    s.object(protocolVersion);
    s.value1b(outType);
    s.value4b(outSize);
    if (s.adapter().error() != bitsery::ReaderError::NoError) {
        raise(fmt::format(
            "Failed to read stream message header: Error {}",
            static_cast<std::underlying_type_t<bitsery::ReaderError>>(s.adapter().error())));
    }
    if (!protocolVersion.isCompatible(CurrentProtocolVersion)) {
        // Stream compatibility is defined at the Version major/minor level.
        // Patch releases may still exchange the same wire format.
        raise(fmt::format(
            "Unable to read message with version {} using version {}.",
            protocolVersion.toString(),
            CurrentProtocolVersion.toString()));
    }
    if (bytesRead != nullptr) {
        *bytesRead = s.adapter().currentReadPos();
    }

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
                // String pools are streamed ahead of tile payloads so ids inside
                // the upcoming layer bytes are immediately resolvable client-side.
                std::string serializedStrings;
                serializedStrings.reserve(1024); // Pre-allocate for typical string pool update
                {
                    std::ostringstream stringsStream;
                    auto stringUpdateOffset = 0;
                    if (differentialStringUpdates_)
                        // Differential mode sends only strings the client has
                        // not acknowledged yet; caches must disable this and
                        // store complete pools instead.
                        stringUpdateOffset = highestStringKnownToClient + 1;
                    strings->write(stringsStream, stringUpdateOffset);
                    serializedStrings = stringsStream.str();
                }
                sendMessage(std::move(serializedStrings), MessageType::StringPool);
                highestStringKnownToClient = highestString;
            }
        }
    }

    // Send the actual layer
    std::string serializedLayer;
    serializedLayer.reserve(65536); // Pre-allocate 64KB for typical tile size
    auto start = std::chrono::system_clock::now();
    {
        std::ostringstream layerStream;
        tileLayer->write(layerStream);
        serializedLayer = layerStream.str();
    }
    auto& bytes = serializedLayer;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - start);
    log().trace("Writing {} kB took {} ms.", bytes.size()/1000, elapsed.count());

    if (std::dynamic_pointer_cast<TileSearchResultLayer>(tileLayer)) {
        sendMessage(std::move(bytes), MessageType::TileSearchResultLayer);
        return;
    }

    const auto layerType = tileLayer->layerInfo()->type_;
    const auto messageType = [&layerType]() {
        switch (layerType) {
        case mapget::LayerType::Features:
            return MessageType::TileFeatureLayer;
        case mapget::LayerType::SourceData:
            return MessageType::TileSourceDataLayer;
        default:
            // Other layer types currently have no binary stream encoding.
            raiseFmt("Unsupported layer type: {}", static_cast<int>(layerType));
        }
        return MessageType::None;
    }();

    sendMessage(std::move(bytes), messageType);
}

void TileLayerStream::Writer::sendMessage(std::string&& bytes, TileLayerStream::MessageType msgType)
{
    // TODO refactor the preparation of tile layer & field dicts storage format
    // such that the encoding logic is not split over multiple functions.

    // Calculate actual header size
    // Protocol version: ~10 bytes (depending on version object size)
    // Message type: 1 byte  
    // Content length: 4 bytes
    // Bitsery overhead: ~5 bytes
    constexpr size_t estimatedHeaderSize = 20;
    
    // Build the complete message efficiently
    std::string message;
    message.reserve(estimatedHeaderSize + bytes.size());
    
    // Serialize header directly into the message string
    {
        std::ostringstream headerStream;
        bitsery::Serializer<bitsery::OutputStreamAdapter> s(headerStream);
        s.object(CurrentProtocolVersion);
        s.value1b(msgType);
        s.value4b(static_cast<uint32_t>(bytes.size()));
        
        message = headerStream.str();
    }
    
    // Append the framed payload bytes after the header to produce one contiguous
    // message for the caller's transport layer.
    message.append(std::move(bytes));
    
    // Send with move semantics
    onMessage_(std::move(message), msgType);
}

void TileLayerStream::Writer::sendStatus(std::string statusJson)
{
    sendMessage(std::move(statusJson), MessageType::Status);
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
        std::unique_lock stringPoolWriteLock(stringPoolCacheMutex_);
        // Another thread may have populated the cache between the shared-read
        // miss and taking the write lock, so check again before inserting.
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
