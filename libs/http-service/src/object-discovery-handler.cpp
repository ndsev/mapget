#include <mutex>
#include "http-service-impl.h"

namespace mapget
{
namespace
{
/** Own a bounded HTTP batch until all scheduler callbacks have supplied one tile's associations. */
class ObjectDiscoveryBatch
{
public:
    /** Reserve stable result slots so asynchronous completion preserves request order. */
    ObjectDiscoveryBatch(size_t count, std::function<void(drogon::HttpResponsePtr const&)> callback)
        : remaining_(count), callback_(std::move(callback)), results_(count)
    {
    }

    /** Publish exactly one result; no datasource work runs on the HTTP event loop. */
    void complete(size_t index, ObjectDiscoveryRequest const& request, ObjectDiscoveryResult result)
    {
        auto item = result.toJson();
        item["mapId"] = request.mapId_;
        item["layerId"] = request.layerId_;
        item["tileId"] = request.tileId_.value();
        if (request.sourceId_)
            item["sourceId"] = *request.sourceId_;
        std::unique_lock lock(mutex_);
        results_.at(index) = std::move(item);
        if (--remaining_ != 0)
            return;
        auto body = nlohmann::json{{"responses", std::move(results_)}}.dump();
        lock.unlock();
        auto response = drogon::HttpResponse::newHttpResponse();
        response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        response->addHeader("Cache-Control", "no-store");
        response->setBody(std::move(body));
        callback_(response);
    }

private:
    std::mutex mutex_;
    size_t remaining_;
    std::function<void(drogon::HttpResponsePtr const&)> callback_;
    std::vector<nlohmann::json> results_;
};
}  // namespace

void HttpService::Impl::handleObjectDiscoveryRequest(
    drogon::HttpRequestPtr const& req,
    std::function<void(drogon::HttpResponsePtr const&)>&& callback) const
{
    try {
        auto const json = nlohmann::json::parse(req->body());
        auto const& requests = json.at("requests");
        if (!requests.is_array())
            throw std::invalid_argument("requests must be an array.");
        std::vector<ObjectDiscoveryRequest> work;
        for (auto const& item : requests) {
            auto const& tiles = item.at("tileIds");
            if (!tiles.is_array() || tiles.empty() || tiles.size() > 256 - work.size())
                throw std::invalid_argument("Discovery batches require 1..256 tiles in total.");
            for (auto const& tile : tiles) {
                auto id = PartitionId::fromJson({{"kind", "tile"}, {"id", tile}}).tileId();
                if (!id.isValid())
                    throw std::invalid_argument("Discovery requires valid spatial tiles.");
                ObjectDiscoveryRequest request{
                    item.at("mapId").get<std::string>(),
                    item.at("layerId").get<std::string>(),
                    id};
                if (item.contains("sourceId"))
                    request.sourceId_ = item.at("sourceId").get<std::string>();
                work.push_back(std::move(request));
            }
        }
        if (work.empty())
            throw std::invalid_argument("Discovery requires at least one tile.");
        auto batch = std::make_shared<ObjectDiscoveryBatch>(work.size(), callback);
        auto headers = detail::authHeadersFromRequest(req);
        for (size_t index = 0; index < work.size(); ++index) {
            auto request = work[index];
            self_.discoverObjects(
                request,
                [batch, index, request](ObjectDiscoveryResult result)
                { batch->complete(index, request, std::move(result)); },
                headers);
        }
    }
    catch (std::exception const& error) {
        auto response = drogon::HttpResponse::newHttpResponse();
        response->setStatusCode(drogon::k400BadRequest);
        response->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        response->setBody(error.what());
        callback(response);
    }
}
}  // namespace mapget
