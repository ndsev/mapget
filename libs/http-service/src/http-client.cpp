#include "http-client.h"
#include "tiles-request-json.h"

#include "mapget/log.h"
#include "mapget/model/mapschema.h"

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/utils/Utilities.h>
#include <trantor/net/EventLoopThread.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
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

}  // namespace

struct HttpClient::Impl {
    std::string baseUrl_;
    std::shared_ptr<trantor::EventLoopThread> loopThread_;
    drogon::HttpClientPtr client_;
    /** Ordered (sourceId, metadata) entries, including add-ons sharing the primary mapId. */
    std::vector<std::pair<std::string, DataSourceInfo>> sources_;
    /** Installed graphs for all authorized source entries, including shared/cross-layer projections. */
    std::unordered_map<std::string, std::shared_ptr<MapSchema const>> schemas_;
    std::shared_ptr<TileLayerStream::StringPoolCache> stringPoolProvider_;
    AuthHeaders headers_;

    Impl(std::string const& host, uint16_t port, AuthHeaders headers, bool enableCompression)
        : baseUrl_(fmt::format("http://{}:{}/", host, port)), headers_(std::move(headers))
    {
        if (enableCompression && !(headers_.contains("Accept-Encoding") || headers_.contains("accept-encoding"))) {
            headers_.emplace("Accept-Encoding", "gzip");
        }

        loopThread_ = std::make_shared<trantor::EventLoopThread>("MapgetHttpClient");
        loopThread_->run();

        client_ = drogon::HttpClient::newHttpClient(baseUrl_, loopThread_->getLoop());

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
            if (info.value("status", "ready") != "ready")
                continue;
            auto sourceId = info.at("sourceId").get<std::string>();
            if (sourceId.empty() || std::ranges::any_of(sources_, [&](auto const& source) {
                    return source.first == sourceId;
                }))
                throw std::runtime_error("Sources catalog contains an empty or duplicate sourceId.");
            auto parsedInfo = DataSourceInfo::fromJson(info);
            if (!parsedInfo.mapSchema_) {
                parsedInfo.setMapSchema(getSchema(sourceId, parsedInfo.schemaId_));
            }
            parsedInfo.validateSchema();
            schemas_.emplace(parsedInfo.schemaId_, parsedInfo.mapSchema_);
            sources_.emplace_back(std::move(sourceId), std::move(parsedInfo));
        }
    }

    /** Keep authorization source-specific, but share validated immutable content across clients. */
    std::shared_ptr<MapSchema const> getSchema(std::string const& sourceId, std::string const& schemaId)
    {
        if (sourceId.empty() || schemaId.empty())
            throw std::invalid_argument("Schema retrieval requires sourceId and schemaId.");

        auto makeRequest = [&] {
            auto request = drogon::HttpRequest::newHttpRequest();
            request->setMethod(drogon::Get);
            request->setPath("/schema");
            request->setParameter("sourceId", sourceId);
            request->setParameter("schemaId", schemaId);
            applyHeaders(request, headers_);
            // The binary codec already bounds decoded size. Avoid an additional unbounded inflate.
            request->addHeader("Accept-Encoding", "identity");
            return request;
        };

        auto probe = makeRequest();
        probe->addHeader("If-None-Match", "*");
        auto [probeStatus, probeResponse] = client_->sendRequest(probe, 30.0);
        if (probeStatus != drogon::ReqResult::Ok || !probeResponse ||
            probeResponse->statusCode() != drogon::k304NotModified)
            throw std::runtime_error("Schema binding is unavailable for this source.");

        return MapSchema::getOrBuild(
            "binary:" + schemaId,
            [loop = loopThread_, baseUrl = baseUrl_, request = makeRequest(), schemaId] {
                // The copied request owns its auth headers; the connection cannot outlive this loop.
                auto client = drogon::HttpClient::newHttpClient(baseUrl, loop->getLoop());
                auto [status, response] = client->sendRequest(request, 30.0);
                if (status != drogon::ReqResult::Ok || !response ||
                    response->statusCode() != drogon::k200OK)
                    throw std::runtime_error("Failed to fetch schema artifact.");
                auto body = response->body();
                return MapSchema::read(
                    {reinterpret_cast<uint8_t const*>(body.data()), body.size()}, schemaId);
            });
    }

    [[nodiscard]] std::shared_ptr<LayerInfo> resolve(
        std::string_view map,
        std::string_view layer,
        std::optional<std::string> const& sourceId = {}) const
    {
        std::shared_ptr<LayerInfo> result;
        for (auto const& [id, source] : sources_) {
            if (source.isAddOn_ || (sourceId && id != *sourceId) || source.mapId_ != map)
                continue;
            auto candidate = source.getLayer(std::string(layer), false);
            if (!candidate)
                continue;
            if (result)
                throw std::runtime_error("Ambiguous datasource for map/layer; specify sourceId.");
            result = std::move(candidate);
        }
        if (!result)
            throw std::runtime_error("Could not resolve the requested source/map/layer metadata.");
        return result;
    }

    /** Binary readers only consult installed owners; parsing must never trigger network I/O. */
    std::shared_ptr<MapSchema const> resolveSchema(std::string_view id) const
    {
        auto found = schemas_.find(std::string(id));
        return found == schemas_.end() ? nullptr : found->second;
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

std::shared_ptr<MapSchema const> HttpClient::getSchema(
    std::string const& sourceId, std::string const& schemaId)
{
    return impl_->getSchema(sourceId, schemaId);
}

nlohmann::json HttpClient::sourceCatalog() const
{
    auto result = nlohmann::json::array();
    for (auto const& [sourceId, info] : impl_->sources_) {
        auto source = info.toJson();
        source["sourceId"] = sourceId;
        result.push_back(std::move(source));
    }
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
        [this, sourceId = request->sourceId_](auto&& mapId, auto&& layerId) {
            return impl_->resolve(mapId, layerId, sourceId);
        },
        [request](auto&& result) { request->notifyResult(result); },
        impl_->stringPoolProvider_,
        std::function<void(TileLayerStream::MessageType, std::string_view)>{},
        [this](std::string_view id) { return impl_->resolveSchema(id); });

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
                auto const layerInfo = impl_->resolve(request->mapId_, request->layerId_, request->sourceId_);
                request->prepareResolvedLayer(layerInfo->type_);
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

FeatureLayerFilterTilesRequest::Ptr HttpClient::filter(
    FeatureLayerFilterTilesRequest::Ptr const& request)
{
    if (request->isDone()) {
        request->notifyStatus();
        return request;
    }

    const bool incoming = std::ranges::any_of(request->filter_.channels_, [](auto const& channel) {
        return channel.relation_ && channel.relation_->incoming_;
    });
    bool sawEnd = false;
    bool sawSuccess = false;
    nlohmann::json incomingSuccess;
    std::vector<PartitionFeatureLayer::Ptr> snapshots;
    std::vector<IncomingRelationCollection> collections;
    auto reader = std::make_unique<TileLayerStream::Reader>(
        [this, request](auto&& mapId, auto&& layerId) {
            // Related snapshots can belong to another layer/map. Never apply the root's selector there.
            return impl_->resolve(mapId, layerId,
                mapId == request->mapId_ && layerId == request->layerId_ ? request->sourceId_ : std::nullopt);
        },
        [&](auto&& result)
        {
            if (incoming) {
                if (sawSuccess || sawEnd) throw std::runtime_error("Incoming snapshot arrived after terminal status");
                auto snapshot = std::dynamic_pointer_cast<PartitionFeatureLayer>(result);
                if (!snapshot || snapshots.size() >= IncomingRelationCollection::MaxSnapshots)
                    throw std::runtime_error("Invalid incoming feature snapshot stream");
                snapshots.push_back(std::move(snapshot));
                return;
            }
            auto subset = std::dynamic_pointer_cast<PartitionSubsetLayer>(result);
            if (!subset) {
                log().warn("HttpClient /filter ignored non-subset tile layer result");
                return;
            }
            request->notifyResult(std::move(subset));
        },
        impl_->stringPoolProvider_,
        [&](TileLayerStream::MessageType type, std::string_view payload)
        {
            if (incoming && sawEnd)
                throw std::runtime_error("Incoming control frame arrived after EndOfStream");
            if (incoming && type == TileLayerStream::MessageType::EndOfStream) {
                if (!sawSuccess && !request->isDone())
                    throw std::runtime_error("Incoming stream ended without terminal status");
                sawEnd = true;
                return;
            }
            if (incoming && type == TileLayerStream::MessageType::IncomingRelations) {
                if (sawSuccess || sawEnd) throw std::runtime_error("Incoming manifest arrived after terminal status");
                if (collections.size() >= 16)
                    throw std::runtime_error("Too many incoming manifests");
                collections.push_back(IncomingRelationCollection::fromJson(nlohmann::json::parse(payload)));
                return;
            }
            if (type != TileLayerStream::MessageType::Status || payload.empty()) {
                return;
            }
            try {
                auto status = nlohmann::json::parse(std::string(payload));
                const auto state = status.value("state", std::string{});
                if (incoming && state == "Success") incomingSuccess = status;
                else request->notifyProgress(status);
                if (state == "Failed" || state == "Aborted") {
                    request->setStatus(RequestStatus::Aborted);
                }
                else if (state == "Success") {
                    sawSuccess = true;
                    if (!incoming) request->setStatus(RequestStatus::Success);
                }
            }
            catch (const std::exception& e) {
                if (incoming) throw;
                log().warn("HttpClient /filter ignored invalid status payload: {}", e.what());
            }
        },
        [this](std::string_view id) { return impl_->resolveSchema(id); });

    using namespace nlohmann;

    auto tileIds = json::array();
    for (auto const& tileId : request->tileIds_) {
        tileIds.emplace_back(tileId.toJson());
    }
    auto requestJson = json::object({
        {"mapId", request->mapId_},
        {"layerId", request->layerId_},
        {"partitions", std::move(tileIds)},
    });
    if (request->sourceId_) {
        requestJson["sourceId"] = *request->sourceId_;
    }
    if (!request->priorityPartitionIds_.empty()) {
        auto priorityPartitionIds = json::array();
        for (auto const& tileId : request->priorityPartitionIds_) {
            priorityPartitionIds.emplace_back(tileId.toJson());
        }
        requestJson["priorityPartitions"] = std::move(priorityPartitionIds);
    }
    if (!request->exactRoots_.empty()) {
        auto roots = json::array();
        for (auto const& root :
             request->exactRoots_)
        {
            if (!root.canonicalFeatureId_.empty()) {
                roots.push_back({
                    {"partition", root.partitionId_.toJson()},
                    {"featureId", root.canonicalFeatureId_},
                });
                continue;
            }
            auto featureId = json::array();
            for (auto const& [key, value] :
                 root.featureId_)
            {
                featureId.push_back(key);
                std::visit(
                    [&](auto const& item) {
                        featureId.push_back(item);
                    },
                    value);
            }
            roots.push_back({
                {"partition", root.partitionId_.toJson()},
                {"typeId", root.typeId_},
                {"featureId", std::move(featureId)},
            });
        }
        requestJson["roots"] = std::move(roots);
    }

    auto envelope = detail::filterRequestToJson(
        request->filter_,
        false);
    envelope["requests"] =
        json::array({std::move(requestJson)});
    envelope["stringPoolOffsets"] =
        reader->stringPoolCache()->stringPoolOffsets();
    auto body = envelope.dump();

    auto httpReq = drogon::HttpRequest::newHttpRequest();
    httpReq->setMethod(drogon::Post);
    httpReq->setPath("/filter");
    httpReq->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    httpReq->addHeader("Accept", "application/binary");
    httpReq->setBody(std::move(body));
    applyHeaders(httpReq, impl_->headers_);

    auto [result, resp] = impl_->client_->sendRequest(httpReq);
    if (result == drogon::ReqResult::Ok && resp) {
        if (resp->statusCode() == drogon::k200OK) {
            auto decodedBody = decodeResponseBody(resp);
            if (!decodedBody) {
                log().error("HttpClient /filter decode failed");
                request->setStatus(RequestStatus::Aborted);
                return request;
            }

            try {
                reader->read(*decodedBody);
                if (incoming && !request->isDone()) {
                    if (!sawEnd || !sawSuccess || !reader->eos() || collections.empty())
                        throw std::runtime_error("Incomplete incoming inspection stream");
                    std::set<std::pair<MapPartitionKey, std::string>> targets;
                    for (auto const& collection : collections) {
                        FeatureInspectionView view(collection, snapshots);
                        if (!targets.emplace(collection.target_, collection.featureId_).second)
                            throw std::runtime_error("Duplicate incoming target manifest");
                    }
                    std::set<std::pair<MapPartitionKey, std::string>> expected;
                    for (auto const& root : request->exactRoots_) {
                        auto key = MapPartitionKey(LayerType::Features, request->mapId_, request->layerId_, root.partitionId_);
                        auto layer = std::ranges::find_if(snapshots, [&](auto const& value) { return value->id() == key; });
                        if (layer == snapshots.end()) throw std::runtime_error("Missing requested incoming target snapshot");
                        auto feature = root.canonicalFeatureId_.empty() ? (*layer)->find(root.typeId_, root.featureId_)
                            : (*layer)->find(root.canonicalFeatureId_);
                        if (!feature) throw std::runtime_error("Missing requested incoming target feature");
                        expected.emplace(key, feature->id()->toString());
                    }
                    if (targets != expected) throw std::runtime_error("Incoming manifests do not cover the requested roots");
                    for (auto const& layer : snapshots) request->notifyFeatureSnapshot(layer);
                    for (auto const& collection : collections) request->notifyIncomingRelations(collection);
                    request->notifyProgress(incomingSuccess);
                }
            }
            catch (const std::exception& e) {
                log().error("Failed to parse /filter response: {}", e.what());
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

ObjectDiscoveryResult HttpClient::discoverObjects(ObjectDiscoveryRequest const& request)
{
    auto item = nlohmann::json{
        {"mapId", request.mapId_},
        {"layerId", request.layerId_},
        {"tileIds", {request.tileId_.value()}}};
    if (request.sourceId_)
        item["sourceId"] = *request.sourceId_;
    auto httpRequest = drogon::HttpRequest::newHttpRequest();
    httpRequest->setMethod(drogon::Post);
    httpRequest->setPath("/objects/discover");
    httpRequest->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    httpRequest->setBody(nlohmann::json{{"requests", nlohmann::json::array({item})}}.dump());
    applyHeaders(httpRequest, impl_->headers_);
    auto [result, response] = impl_->client_->sendRequest(httpRequest);
    if (result != drogon::ReqResult::Ok || !response || response->statusCode() != drogon::k200OK)
        throw std::runtime_error("Object discovery HTTP request failed.");
    auto body = decodeResponseBody(response);
    if (!body)
        throw std::runtime_error("Unable to decode object discovery response.");
    return ObjectDiscoveryResult::fromJson(nlohmann::json::parse(*body).at("responses").at(0));
}

std::optional<AttachmentResponse>
HttpClient::attachment(
    AttachmentRequest const& request)
{
    auto httpRequest =
        drogon::HttpRequest::newHttpRequest();
    httpRequest->setMethod(drogon::Get);
    httpRequest->setPath("/attachment");
    httpRequest->setParameter("mapId", request.tileKey_.mapId_);
    httpRequest->setParameter("layerId", request.tileKey_.layerId_);
    httpRequest->setParameter("partition", request.tileKey_.partitionId_.toJson().dump());
    httpRequest->setParameter("name", request.name_);
    if (request.sourceId_)
        httpRequest->setParameter("sourceId", *request.sourceId_);
    applyHeaders(
        httpRequest,
        impl_->headers_);

    auto [result, response] =
        impl_->client_->sendRequest(
            httpRequest);
    if (result != drogon::ReqResult::Ok ||
        !response ||
        response->statusCode() !=
            drogon::k200OK)
    {
        return {};
    }
    auto decoded =
        decodeResponseBody(response);
    if (!decoded) {
        return {};
    }

    auto attachment =
        AttachmentResponse{
            .name_ = request.name_,
            .mimeType_ =
                response->contentTypeString(),
            .bytes_ =
                std::make_shared<
                    std::vector<
                        uint8_t> const>(
                    decoded->begin(),
                    decoded->end()),
        };
    if (auto etag =
            response->getHeader("etag");
        !etag.empty())
    {
        attachment.etag_ =
            std::move(etag);
    }
    return attachment;
}

}  // namespace mapget
