#include "datasource-client.h"

namespace mapget
{

RemoteDataSource::RemoteDataSource(const std::string& host, uint16_t port)
    : httpClient_(host, port)
{
}

DataSourceInfo RemoteDataSource::info()
{
    auto fetchedInfoJson = httpClient_.Get("/info");
    auto fetchedInfo = DataSourceInfo::fromJson(nlohmann::json::parse(fetchedInfoJson->body));
    return fetchedInfo;
}

void RemoteDataSource::fill(const TileFeatureLayer::Ptr& featureTile)
{
    // If we get here, an error occurred.
    featureTile->setError("Error while contacting remote data source.");
}

TileFeatureLayer::Ptr
RemoteDataSource::get(const MapTileKey& k, Cache::Ptr& cache, const DataSourceInfo& info)
{
    // Send a GET tile request
    auto tileResponse = httpClient_.Get(stx::format(
        "/tile?layer={}&tileId={}&fieldsOffset={}",
        k.layerId_,
        k.tileId_.value_,
        cachedFieldsOffset(info.nodeId_, cache)));

    // Check that the response is OK
    if (!tileResponse || tileResponse->status >= 300) {
        // Forward to base class get(). This will instantiate a
        // default TileFeatureLayer and call fill(). In our implementation
        // of fill, we set an error.
        // TODO: Read HTTPLIB_ERROR header, more log output.
        return DataSource::get(k, cache, info);
    }

    // Check the response body for expected content
    TileFeatureLayer::Ptr result;
    TileLayerStream::Reader reader(
        [&](auto&& mapId, auto&& layerId) { return info.getLayer(std::string(layerId)); },
        [&](auto&& tile) { result = tile; },
        cache);
    reader.read(tileResponse->body);

    return result;
}

}