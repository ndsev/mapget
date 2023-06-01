#include "stream.h"

#include <bitsery/bitsery.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/traits/string.h>

namespace mapget
{

TileLayerStream::Reader::Reader(
    LayerInfoResolveFun layerInfoProvider,
    std::function<void(std::shared_ptr<TileFeatureLayer>)> onParsedLayer,
    std::shared_ptr<CachedFieldsProvider> fieldCacheProvider)
    : layerInfoProvider_(std::move(layerInfoProvider)),
      fieldCacheProvider_(std::move(fieldCacheProvider)),
      onParsedLayer_(std::move(onParsedLayer))
{
}

void TileLayerStream::Reader::read(const std::vector<uint8_t>& bytes)
{
    std::string_view strBytes((char const*)bytes.data(), bytes.size());
    buffer_ << strBytes;
    while (continueReading());
}

bool TileLayerStream::Reader::eol()
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
        std::string fieldsDictNodeId;
        s.text1b(fieldsDictNodeId, std::numeric_limits<uint32_t>::max());
        (*fieldCacheProvider_)(fieldsDictNodeId)->read(buffer_);
    }

    currentPhase_ = Phase::ReadHeader;
    return true;
}

TileLayerStream::Writer::Writer(
    std::function<void(std::string)> onSerializedBytes,
    std::function<simfil::FieldId(std::string)> fieldsOffsetProvider)
{
}

void TileLayerStream::Writer::write(std::shared_ptr<TileFeatureLayer> tileFeatureLayer)
{

}

std::shared_ptr<Fields> TileLayerStream::CachedFieldsProvider::operator()(const std::string_view&)
{
    return std::shared_ptr<Fields>();
}

}