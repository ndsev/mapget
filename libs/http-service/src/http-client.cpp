#include "http-client.h"

#include "mapget/log.h"

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <trantor/net/EventLoopThread.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "fmt/format.h"

#include <zlib.h>

namespace mapget
{

namespace
{

void applyHeaders(drogon::HttpRequestPtr const& req, AuthHeaders const& headers)
{
    for (auto const& [k, v] : headers) {
        req->addHeader(k, v);
    }
}

[[nodiscard]] bool hasGzipContentEncoding(std::string_view contentEncoding)
{
    if (contentEncoding.empty()) {
        return false;
    }

    std::string normalized(contentEncoding);
    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return normalized.find("gzip") != std::string::npos;
}

[[nodiscard]] bool looksLikeGzip(std::string_view bytes)
{
    return bytes.size() >= 2 &&
           static_cast<unsigned char>(bytes[0]) == 0x1f &&
           static_cast<unsigned char>(bytes[1]) == 0x8b;
}

[[nodiscard]] std::optional<std::string> gunzip(std::string_view input)
{
    if (input.empty()) {
        return std::string{};
    }
    if (input.size() > static_cast<size_t>(std::numeric_limits<uInt>::max())) {
        return std::nullopt;
    }

    z_stream stream{};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());

    // 16 + MAX_WBITS enables gzip container decoding.
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
        return std::nullopt;
    }

    std::string output;
    output.reserve(input.size() * 2);

    char outBuffer[8192];
    int inflateResult = Z_OK;
    do {
        stream.next_out = reinterpret_cast<Bytef*>(outBuffer);
        stream.avail_out = sizeof(outBuffer);
        inflateResult = inflate(&stream, Z_NO_FLUSH);
        if (inflateResult != Z_OK && inflateResult != Z_STREAM_END) {
            inflateEnd(&stream);
            return std::nullopt;
        }
        output.append(outBuffer, sizeof(outBuffer) - stream.avail_out);
    } while (inflateResult != Z_STREAM_END);

    inflateEnd(&stream);
    return output;
}

[[nodiscard]] std::optional<std::string> decodeResponseBody(const drogon::HttpResponsePtr& resp)
{
    if (!resp) {
        return std::nullopt;
    }

    auto body = std::string_view(resp->body().data(), resp->body().size());
    auto contentEncoding = resp->getHeader("Content-Encoding");
    if (contentEncoding.empty()) {
        contentEncoding = resp->getHeader("content-encoding");
    }

    const bool headerSaysGzip = hasGzipContentEncoding(contentEncoding);
    const bool bodyLooksGzip = looksLikeGzip(body);

    // Drogon may already have decompressed the payload before exposing body().
    // If the header says gzip but bytes are not gzip-framed, treat body as decoded.
    if (headerSaysGzip && !bodyLooksGzip) {
        return std::string(body);
    }

    if (!headerSaysGzip && !bodyLooksGzip) {
        return std::string(body);
    }

    return gunzip(body);
}

[[nodiscard]] std::string searchScopeToJsonValue(FeatureLayerSearchScope scope)
{
    return scope == FeatureLayerSearchScope::Attribute ? "attribute" : "feature";
}

}  // namespace

struct HttpClient::Impl {
    std::unique_ptr<trantor::EventLoopThread> loopThread_;
    drogon::HttpClientPtr client_;
    std::unordered_map<std::string, DataSourceInfo> sources_;
    std::shared_ptr<TileLayerStream::StringPoolCache> stringPoolProvider_;
    AuthHeaders headers_;

    Impl(std::string const& host, uint16_t port, AuthHeaders headers, bool enableCompression) : headers_(std::move(headers))
    {
        if (enableCompression && !(headers_.contains("Accept-Encoding") || headers_.contains("accept-encoding"))) {
            headers_.emplace("Accept-Encoding", "gzip");
        }

        loopThread_ = std::make_unique<trantor::EventLoopThread>("MapgetHttpClient");
        loopThread_->run();

        const auto hostString = fmt::format("http://{}:{}/", host, port);
        client_ = drogon::HttpClient::newHttpClient(hostString, loopThread_->getLoop());

        stringPoolProvider_ = std::make_shared<TileLayerStream::StringPoolCache>();

        // Fetch data sources (/sources).
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Get);
        req->setPath("/sources");
        applyHeaders(req, headers_);

        auto [result, resp] = client_->sendRequest(req);
        if (result != drogon::ReqResult::Ok || !resp) {
            raise(fmt::format("Failed to fetch sources: [{}]", drogon::to_string_view(result)));
        }
        if (resp->statusCode() != drogon::k200OK) {
            raise(fmt::format("Failed to fetch sources: [{}]", (int)resp->statusCode()));
        }

        auto decodedBody = decodeResponseBody(resp);
        if (!decodedBody) {
            raise("Failed to decode /sources response body");
        }

        for (auto const& info : nlohmann::json::parse(*decodedBody)) {
            auto parsedInfo = DataSourceInfo::fromJson(info);
            sources_.emplace(parsedInfo.mapId_, parsedInfo);
        }
    }

    [[nodiscard]] std::shared_ptr<LayerInfo> resolve(std::string_view const& map, std::string_view const& layer) const
    {
        auto mapIt = sources_.find(std::string(map));
        if (mapIt == sources_.end())
            raise("Could not find map data source info");
        return mapIt->second.getLayer(std::string(layer));
    }
};

HttpClient::HttpClient(const std::string& host, uint16_t port, AuthHeaders headers, bool enableCompression)
    : impl_(std::make_unique<Impl>(host, port, std::move(headers), enableCompression))
{
}

HttpClient::~HttpClient() = default;

std::vector<DataSourceInfo> HttpClient::sources() const
{
    std::vector<DataSourceInfo> result;
    for (auto const& [_, ds] : impl_->sources_)
        result.emplace_back(ds);
    return result;
}

LayerTilesRequest::Ptr HttpClient::request(const LayerTilesRequest::Ptr& request)
{
    // Finalize requests that did not contain any tiles.
    if (request->isDone()) {
        request->notifyStatus();
        return request;
    }

    auto reader = std::make_unique<TileLayerStream::Reader>(
        [this](auto&& mapId, auto&& layerId) { return impl_->resolve(mapId, layerId); },
        [request](auto&& result) { request->notifyResult(result); },
        impl_->stringPoolProvider_);

    using namespace nlohmann;

    auto body = json::object({
        {"requests", json::array({request->toJson()})},
        {"stringPoolOffsets", reader->stringPoolCache()->stringPoolOffsets()},
    }).dump();

    auto httpReq = drogon::HttpRequest::newHttpRequest();
    httpReq->setMethod(drogon::Post);
    httpReq->setPath("/tiles");
    httpReq->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    httpReq->addHeader("Accept", "application/binary");
    httpReq->setBody(std::move(body));
    applyHeaders(httpReq, impl_->headers_);

    auto [result, resp] = impl_->client_->sendRequest(httpReq);
    if (result == drogon::ReqResult::Ok && resp) {
        if (resp->statusCode() == drogon::k200OK) {
            try {
                auto const layerInfo = impl_->resolve(request->mapId_, request->layerId_);
                request->prepareResolvedLayer(layerInfo->type_, layerInfo->stages_);
            }
            catch (const std::exception& e) {
                log().error("Failed to resolve request layer context: {}", e.what());
                request->setStatus(RequestStatus::Aborted);
                return request;
            }

            auto decodedBody = decodeResponseBody(resp);
            if (!decodedBody) {
                log().error("HttpClient /tiles decode failed");
                request->setStatus(RequestStatus::Aborted);
                return request;
            }

            // TODO: Support streamed/chunked tile responses.
            //  Drogon's `HttpClient` API only provides the full buffered body.
            //  True streaming would require a custom client built on
            //  `trantor::TcpClient` (still within the Drogon dependency).
            try {
                reader->read(*decodedBody);
            }
            catch (const std::exception& e) {
                log().error("Failed to parse /tiles response: {}", e.what());
                request->setStatus(RequestStatus::Aborted);
                return request;
            }

            if (!request->isDone()) {
                // HttpClient performs one fully-buffered request. If parsing did
                // not resolve request status by now, no more bytes will arrive.
                request->setStatus(RequestStatus::Aborted);
            }
        } else if (resp->statusCode() == drogon::k400BadRequest) {
            request->setStatus(RequestStatus::NoDataSource);
        } else if (resp->statusCode() == drogon::k403Forbidden) {
            request->setStatus(RequestStatus::Unauthorized);
        } else {
            request->setStatus(RequestStatus::Aborted);
        }
    } else {
        request->setStatus(RequestStatus::Aborted);
    }

    return request;
}

FeatureLayerSearchTilesRequest::Ptr HttpClient::search(const FeatureLayerSearchTilesRequest::Ptr& request)
{
    if (request->isDone()) {
        request->notifyStatus();
        return request;
    }

    auto reader = std::make_unique<TileLayerStream::Reader>(
        [this](auto&& mapId, auto&& layerId) { return impl_->resolve(mapId, layerId); },
        [request](auto&& result) {
            auto searchResult = std::dynamic_pointer_cast<TileSearchResultLayer>(result);
            if (!searchResult) {
                log().warn("HttpClient /search ignored non-search tile layer result");
                return;
            }
            request->notifyResult(std::move(searchResult));
        },
        impl_->stringPoolProvider_,
        [request](TileLayerStream::MessageType type, std::string_view payload) {
            if (type != TileLayerStream::MessageType::Status || payload.empty()) {
                return;
            }
            try {
                auto status = nlohmann::json::parse(std::string(payload));
                request->notifyProgress(status);
                const auto state = status.value("state", std::string{});
                if (state == "Failed" || state == "Aborted") {
                    request->setStatus(RequestStatus::Aborted);
                } else if (state == "Success") {
                    request->setStatus(RequestStatus::Success);
                }
            }
            catch (const std::exception& e) {
                log().warn("HttpClient /search ignored invalid status payload: {}", e.what());
            }
        });

    using namespace nlohmann;

    auto tileIds = json::array();
    for (auto const& tileId : request->tileIds_) {
        tileIds.emplace_back(tileId.value_);
    }
    auto requestJson = json::object({
        {"mapId", request->mapId_},
        {"layerId", request->layerId_},
        {"tileIds", std::move(tileIds)},
    });
    if (!request->priorityTileIds_.empty()) {
        auto priorityTileIds = json::array();
        for (auto const& tileId : request->priorityTileIds_) {
            priorityTileIds.emplace_back(tileId.value_);
        }
        requestJson["priorityTileIds"] = std::move(priorityTileIds);
    }

    auto body = json::object({
        {"query", request->search_.query_},
        {"scope", searchScopeToJsonValue(request->search_.scope_)},
        {"withFields", request->search_.withFields_},
        {"requests", json::array({std::move(requestJson)})},
        {"stringPoolOffsets", reader->stringPoolCache()->stringPoolOffsets()},
    }).dump();

    auto httpReq = drogon::HttpRequest::newHttpRequest();
    httpReq->setMethod(drogon::Post);
    httpReq->setPath("/search");
    httpReq->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    httpReq->addHeader("Accept", "application/binary");
    httpReq->setBody(std::move(body));
    applyHeaders(httpReq, impl_->headers_);

    auto [result, resp] = impl_->client_->sendRequest(httpReq);
    if (result == drogon::ReqResult::Ok && resp) {
        if (resp->statusCode() == drogon::k200OK) {
            auto decodedBody = decodeResponseBody(resp);
            if (!decodedBody) {
                log().error("HttpClient /search decode failed");
                request->setStatus(RequestStatus::Aborted);
                return request;
            }

            try {
                reader->read(*decodedBody);
            }
            catch (const std::exception& e) {
                log().error("Failed to parse /search response: {}", e.what());
                request->setStatus(RequestStatus::Aborted);
                return request;
            }

            if (!request->isDone()) {
                request->setStatus(RequestStatus::Success);
            }
        } else if (resp->statusCode() == drogon::k400BadRequest) {
            request->setStatus(RequestStatus::NoDataSource);
        } else if (resp->statusCode() == drogon::k403Forbidden) {
            request->setStatus(RequestStatus::Unauthorized);
        } else {
            request->setStatus(RequestStatus::Aborted);
        }
    } else {
        request->setStatus(RequestStatus::Aborted);
    }

    return request;
}

}  // namespace mapget
