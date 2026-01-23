#include "http-client.h"

#include "mapget/log.h"

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <trantor/net/EventLoopThread.h>

#include <unordered_map>

#include "fmt/format.h"

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

        for (auto const& info : nlohmann::json::parse(std::string(resp->body()))) {
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
    httpReq->setBody(std::move(body));
    applyHeaders(httpReq, impl_->headers_);

    auto [result, resp] = impl_->client_->sendRequest(httpReq);
    if (result == drogon::ReqResult::Ok && resp) {
        if (resp->statusCode() == drogon::k200OK) {
            reader->read(std::string(resp->body()));
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

