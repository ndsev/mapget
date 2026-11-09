#include "mapget/http-datasource/detail/schema-response.h"
#include "mapget/model/mapschema.h"

#include <drogon/utils/Utilities.h>

#include <algorithm>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace mapget::detail
{
namespace
{

/** GET uses weak comparison, including lists and wildcard validators. */
bool schemaEtagMatches(std::string_view condition, std::string_view etag)
{
    if (etag.starts_with("W/"))
        etag.remove_prefix(2);
    while (!condition.empty()) {
        auto comma = std::string_view::npos;
        bool quoted = false;
        for (size_t index = 0; index < condition.size(); ++index) {
            if (condition[index] == '"')
                quoted = !quoted;
            else if (condition[index] == ',' && !quoted) {
                comma = index;
                break;
            }
        }
        auto candidate = condition.substr(0, comma);
        auto first = candidate.find_first_not_of(" \t");
        auto last = candidate.find_last_not_of(" \t");
        if (first != std::string_view::npos) {
            candidate = candidate.substr(first, last - first + 1);
            if (candidate.starts_with("W/"))
                candidate.remove_prefix(2);
            if (candidate == "*" || candidate == etag)
                return true;
        }
        if (comma == std::string_view::npos)
            break;
        condition.remove_prefix(comma + 1);
    }
    return false;
}

} // namespace

drogon::HttpResponsePtr SchemaResponseCache::response(
    std::shared_ptr<MapSchema const> const& schema, std::string_view ifNoneMatch)
{
    if (!schema || !schema->finalized())
        throw std::invalid_argument("Schema responses require a finalized owner.");
    std::shared_ptr<std::string const> bytes;
    std::string etag;
    {
        std::lock_guard lock(mutex_);
        if (owner_.lock() != schema || !bytes_) {
            std::ostringstream output(std::ios::out | std::ios::binary);
            if (!schema->write(output))
                throw std::runtime_error("Schema serialization failed.");
            auto encoded = std::move(output).str();
            if (encoded.size() > MaxArtifactBytes)
                throw std::runtime_error("Schema artifact exceeds the transport limit.");
            // A weak digest also remains valid if HTTP middleware changes content encoding.
            auto digest = "W/\"sha256-" + drogon::utils::getSha256(encoded) + "\"";
            auto* stored = new std::string(std::move(encoded));
            allocatedCapacity_->fetch_add(stored->capacity(), std::memory_order_relaxed);
            bytes_ = std::shared_ptr<std::string const>(stored,
                [allocated = allocatedCapacity_](std::string const* value) {
                    allocated->fetch_sub(value->capacity(), std::memory_order_relaxed);
                    delete value;
                });
            retainedCapacity_.store(bytes_->capacity(), std::memory_order_relaxed);
            etag_ = std::move(digest);
            owner_ = schema;
        }
        bytes = bytes_;
        etag = etag_;
    }

    drogon::HttpResponsePtr result;
    if (schemaEtagMatches(ifNoneMatch, etag)) {
        result = drogon::HttpResponse::newHttpResponse();
        result->setStatusCode(drogon::k304NotModified);
    }
    else {
        auto size = bytes->size();
        result = drogon::HttpResponse::newStreamResponse(
            [bytes = std::move(bytes), offset = size_t{0}](char* buffer, size_t available) mutable {
                if (!buffer) {
                    bytes.reset();
                    return size_t{0};
                }
                if (!bytes)
                    return size_t{0};
                auto count = std::min(available, bytes->size() - offset);
                std::memcpy(buffer, bytes->data() + offset, count);
                offset += count;
                return count;
            }, {}, drogon::CT_APPLICATION_OCTET_STREAM);
        result->addHeader("Content-Length", std::to_string(size));
    }
    result->addHeader("ETag", etag);
    // Immutable content is not public authorization. Always revalidate with the current source.
    result->addHeader("Cache-Control", "private, no-cache, no-transform");
    result->addHeader("Vary", "*");
    return result;
}

size_t SchemaResponseCache::allocatedCapacity() const noexcept
{
    return allocatedCapacity_->load(std::memory_order_relaxed);
}

size_t SchemaResponseCache::retainedCapacity() const noexcept
{
    return retainedCapacity_.load(std::memory_order_relaxed);
}

} // namespace mapget::detail
