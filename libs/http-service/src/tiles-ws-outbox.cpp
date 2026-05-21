#include "tiles-ws-outbox.h"

#include <bitsery/adapter/stream.h>
#include <bitsery/bitsery.h>

#include <cstdint>
#include <sstream>

#include <zlib.h>

namespace mapget::detail
{

std::string encodeStreamMessage(TileLayerStream::MessageType type, std::string_view payload)
{
    std::ostringstream headerStream;
    bitsery::Serializer<bitsery::OutputStreamAdapter> s(headerStream);
    s.object(TileLayerStream::CurrentProtocolVersion);
    s.value1b(type);
    s.value4b(static_cast<uint32_t>(payload.size()));

    auto message = headerStream.str();
    message.append(payload);
    return message;
}

bool containsGzip(std::string_view acceptEncoding)
{
    return !acceptEncoding.empty() && acceptEncoding.find("gzip") != std::string_view::npos;
}

std::optional<std::string> gzipCompress(std::string_view input)
{
    z_stream stream{};
    stream.zalloc = Z_NULL;
    stream.zfree = Z_NULL;
    stream.opaque = Z_NULL;

    // 16 + MAX_WBITS enables gzip framing instead of raw deflate.
    const int initResult = deflateInit2(
        &stream,
        Z_DEFAULT_COMPRESSION,
        Z_DEFLATED,
        16 + MAX_WBITS,
        8,
        Z_DEFAULT_STRATEGY);
    if (initResult != Z_OK) {
        return std::nullopt;
    }

    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());

    std::string compressed;
    compressed.reserve(input.size() / 2 + 128);

    char outBuffer[8192];
    int deflateResult = Z_OK;
    do {
        stream.next_out = reinterpret_cast<Bytef*>(outBuffer);
        stream.avail_out = sizeof(outBuffer);
        deflateResult = deflate(&stream, Z_FINISH);
        if (deflateResult != Z_OK && deflateResult != Z_STREAM_END) {
            deflateEnd(&stream);
            return std::nullopt;
        }
        compressed.append(outBuffer, sizeof(outBuffer) - stream.avail_out);
    } while (deflateResult != Z_STREAM_END);

    deflateEnd(&stream);
    return compressed;
}

} // namespace mapget::detail
