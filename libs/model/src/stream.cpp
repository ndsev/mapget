#include "stream.h"

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
      fieldCacheProvider_(
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
    bitsery::Deserializer<bitsery::InputStreamAdapter> s(buffer_);
    auto numUnreadBytes = buffer_.tellp() - buffer_.tellg();

    if (currentPhase_ == Phase::ReadHeader)
    {
        // Version: 6B, Type: 1B, Size: 4B
        constexpr auto headerSize = 6 + 1 + 4;
        if (numUnreadBytes < headerSize)
            return false;

        Version protocolVersion;
        s.object(protocolVersion);
        if (!protocolVersion.isCompatible(CurrentProtocolVersion)) {
            throw std::runtime_error(stx::format(
                "Unable to read message with version {} using version {}.",
                protocolVersion.toString(),
                CurrentProtocolVersion.toString()));
        }
        s.value1b(nextValueType_);
        s.value4b(nextValueSize_);
        currentPhase_ = Phase::ReadValue;
        return true;
    }

    if (numUnreadBytes < nextValueSize_)
        return false;

    if (nextValueType_ == MessageType::TileFeatureLayer)
    {
        onParsedLayer_(std::make_shared<TileFeatureLayer>(
            buffer_,
            layerInfoProvider_,
            *fieldCacheProvider_));
    }
    else if (nextValueType_ == MessageType::Fields)
    {
        // Read the node id which identifies the fields dictionary
        std::string fieldsDictNodeId = Fields::readDataSourceNodeId(buffer_);
        (*fieldCacheProvider_)(fieldsDictNodeId)->read(buffer_);
    }

    currentPhase_ = Phase::ReadHeader;
    return true;
}

TileLayerStream::Writer::Writer(
    std::function<void(std::string, MessageType)> onMessage,
    FieldOffsetMap& fieldsOffsets)
    : onMessage_(std::move(onMessage)),
      fieldsOffsets_(fieldsOffsets)
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
        fields->write(serializedFields, highestFieldKnownToClient+1);
        sendMessage(serializedFields.str(), MessageType::Fields);
        highestFieldKnownToClient = highestField;
    }

    // Send actual tileFeatureLayer
    std::stringstream serializedFeatureLayer;
    tileFeatureLayer->write(serializedFeatureLayer);
    sendMessage(serializedFeatureLayer.str(), MessageType::TileFeatureLayer);
}

void TileLayerStream::Writer::sendMessage(std::string const& bytes, TileLayerStream::MessageType msgType)
{
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

std::shared_ptr<Fields> TileLayerStream::CachedFieldsProvider::operator()(const std::string_view& nodeId)
{
    auto it = fieldsPerNodeId_.find(std::string(nodeId));
    if (it != fieldsPerNodeId_.end()) {
        return it->second;
    }
    auto [newIt, _] = fieldsPerNodeId_.emplace(nodeId, std::make_shared<Fields>(std::string(nodeId)));
    return newIt->second;
}

}