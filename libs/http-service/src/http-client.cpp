#include "http-client.h"
#include "httplib.h"
#include "mapget/log.h"

namespace mapget
{

struct HttpClient::Impl {
    httplib::Client client_;
    std::unordered_map<std::string, DataSourceInfo> sources_;
    std::shared_ptr<TileLayerStream::StringPoolCache> stringPoolProvider_;
    httplib::Headers headers_;

    Impl(std::string const& host, uint16_t port, httplib::Headers headers) :
        client_(host, port),
        headers_(std::move(headers))
    {
        stringPoolProvider_ = std::make_shared<TileLayerStream::StringPoolCache>();
        client_.set_keep_alive(false);
        auto sourcesJson = client_.Get("/sources", headers_);
        if (!sourcesJson || sourcesJson->status != 200)
            raise(
                fmt::format("Failed to fetch sources: [{}]", sourcesJson->status));
        for (auto const& info : nlohmann::json::parse(sourcesJson->body)) {
            auto parsedInfo = DataSourceInfo::fromJson(info);
            sources_.emplace(parsedInfo.mapId_, parsedInfo);
        }
    }

    [[nodiscard]] std::shared_ptr<LayerInfo>
    resolve(std::string_view const& map, std::string_view const& layer) const
    {
        auto mapIt = sources_.find(std::string(map));
        if (mapIt == sources_.end())
            raise("Could not find map data source info");
        return mapIt->second.getLayer(std::string(layer));
    }
};

HttpClient::HttpClient(const std::string& host, uint16_t port, httplib::Headers headers) : impl_(
    std::make_unique<Impl>(host, port, std::move(headers))) {}

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
        [this](auto&& mapId, auto&& layerId){return impl_->resolve(mapId, layerId);},
        [request](auto&& result) { request->notifyResult(result); },
        impl_->stringPoolProvider_);

    using namespace nlohmann;

    // TODO: Currently, cpp-httplib client-POST does not support async responses.
    //  Those are only supported by GET. So, currently, this HttpClient
    //  does not profit from the streaming response. However, erdblick is
    //  is fully able to process async responses as it uses the browser fetch()-API.
    auto tileResponse = impl_->client_.Post(
        "/tiles",
        impl_->headers_,
        json::object({
            {"requests", json::array({request->toJson()})},
            {"stringPoolOffsets", reader->stringPoolCache()->stringPoolOffsets()}
        }).dump(),
        "application/json");

    if (tileResponse) {
        if (tileResponse->status == 200) {
            reader->read(tileResponse->body);
        }
        else if (tileResponse->status == 400) {
            request->setStatus(RequestStatus::NoDataSource);
        }
        else if (tileResponse->status == 403) {
            request->setStatus(RequestStatus::Unauthorized);
        }
        // TODO if multiple LayerTileRequests are ever sent by this client,
        //  additionally handle RequestStatus::Aborted.
    }

    return request;
}

}
