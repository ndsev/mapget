#pragma once

#include "mapget/model/stream.h"

#include <optional>
#include <string>
#include <string_view>

namespace mapget::detail
{

/** Encode one mapget VTLV frame with protocol header plus payload bytes. */
[[nodiscard]] std::string encodeStreamMessage(TileLayerStream::MessageType type, std::string_view payload);

/** Check whether the HTTP Accept-Encoding header allows gzip responses. */
[[nodiscard]] bool containsGzip(std::string_view acceptEncoding);

/** Compress one payload as gzip (RFC 1952). Returns nullopt on zlib failure. */
[[nodiscard]] std::optional<std::string> gzipCompress(std::string_view input);

} // namespace mapget::detail
