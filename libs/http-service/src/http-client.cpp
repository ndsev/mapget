#include "http-client.h"
#include "httplib.h"

namespace mapget
{

struct HttpClient::Impl {
    httplib::Client client_;
    std::map<std::string, DataSourceInfo> sources_;
    std::shared_ptr<TileLayerStream::CachedFieldsProvider> fieldsProvider_;

    Impl(std::string const& host, uint16_t port) : client_(host, port)
    {
        fieldsProvider_ = std::make_shared<TileLayerStream::CachedFieldsProvider>();
        client_.set_keep_alive(false);
        auto sourcesJson = client_.Get("/sources");
        if (!sourcesJson || sourcesJson->status != 200)
            throw std::runtime_error("Failed to fetch sources.");
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
            throw std::runtime_error("Could nto find map data source info");
        return mapIt->second.getLayer(std::string(layer));
    }
};

HttpClient::HttpClient(const std::string& host, uint16_t port) : impl_(std::make_unique<Impl>(host, port)) {}

HttpClient::~HttpClient() = default;

std::vector<DataSourceInfo> HttpClient::sources() const
{
    std::vector<DataSourceInfo> result;
    for (auto const& [_, ds] : impl_->sources_)
        result.emplace_back(ds);
    return result;
}

Request::Ptr HttpClient::request(const Request::Ptr& request)
{
    if (request->isDone())
        request->notifyDone();

    auto reader = std::make_unique<TileLayerStream::Reader>(
        [this](auto&& mapId, auto&& layerId){return impl_->resolve(mapId, layerId);},
        [request](auto&& result) { request->notifyResult(result); },
        impl_->fieldsProvider_);

    using namespace nlohmann;

    // TODO: Currently, cpp-httplib POST does not support async responses.
    //  Those are only supported by GET.
    auto tileResponse = impl_->client_.Post(
        "/tiles",
        json::object({{"requests", json::array({request->toJson()})}}).dump(),
        "application/json");

    if (tileResponse && tileResponse->status == 200)
        reader->read(tileResponse->body);

    return request;
}

}
