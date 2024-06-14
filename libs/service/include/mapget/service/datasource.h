#pragma once

#include "cache.h"
#include "locate.h"

namespace mapget
{

/**
 * Abstract class which defines the behavior of a mapget data source,
 * as expected by the mapget Service. Any derived data source must implement
 * the info() and fill() methods.
 */
class DataSource
{
public:
    using Ptr = std::shared_ptr<DataSource>;

    /**
     * Method which is called by a service to determine which map layers
     * can be served by this DataSource, and how many layers this
     * data source can process in parallel (i.e. how many threads may
     * run this->fill(...) in parallel).
     */
    virtual DataSourceInfo info() = 0;

    /**
     * Method which is called up to DataSourceInfo::maxParallelJobs_
     * times in parallel to satisfy data requests for a mapget Service.
     * @param featureTile A TileFeatureLayer object which this data source
     *  should fill according the available data. If any error occurs
     *  while doing so, the data source may use TileLayer::setError.
     *  To store any extra information of interest such as timings or sizes,
     *  TileLayer::setInfo() may be used.
     */
    virtual void fill(TileFeatureLayer::Ptr const& featureTile) = 0;

    /**
     * Obtain map tile keys where the feature with the specified ID may be found.
     * The implementation is completely datasource-specific. Note, that the returned
     * resulting LocateResponses may have resolved to a different typeId and featureId
     * than the requested one. This is useful when locate is used to resolve a
     * secondary to a primary feature ID.
     */
    virtual std::vector<LocateResponse> locate(LocateRequest const& req);

    /** Called by mapget::Service worker. Dispatches to Cache or fill(...) on miss. */
    virtual TileFeatureLayer::Ptr get(MapTileKey const& k, Cache::Ptr& cache, DataSourceInfo const& info);

protected:
    static simfil::FieldId cachedFieldsOffset(std::string const& nodeId, Cache::Ptr const& cache);
};

}
