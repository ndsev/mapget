#pragma once

#include "mapget/model/stream.h"

#include <optional>
#include <string>
#include <string_view>

namespace mapget::detail
{

/** Encode one mapget VTLV frame with protocol header plus payload bytes.
 * Payload size must fit the 32-bit wire length.
 */
[[nodiscard]] std::string encodeStreamMessage(TileLayerStream::MessageType type, std::string_view payload);

/** Find the case-sensitive substring "gzip"; this does not parse tokens or quality values. */
[[nodiscard]] bool containsGzip(std::string_view acceptEncoding);

/** Compress one payload as gzip (RFC 1952). Input size must fit zlib uInt;
 * return nullopt on zlib failure.
 */
[[nodiscard]] std::optional<std::string> gzipCompress(std::string_view input);

} // namespace mapget::detail
