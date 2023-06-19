#pragma once

#include "cache.h"

namespace mapget
{

/**
 * Abstract class which defines the behavior of a mapget data source,
 * as expected by the mapget Hub. Any derived data source must implement
 * the info() and fill() methods.
 */
class DataSource
{
public:
    using Ptr = std::shared_ptr<DataSource>;

    /**
     * Method which is called by a hub to determine which map layers
     * can be served by this DataSource, and how many layers this
     * data source can process in parallel (i.e. how many threads may
     * run this->fill(...) in parallel).
     */
    virtual DataSourceInfo info() = 0;

    /**
     * Method which is called up to DataSourceInfo::maxParallelJobs_
     * times in parallel to satisfy data requests for a mapget Hub.
     * @param featureTile A TileFeatureLayer object which this data source
     *  should fill according the available data. If any error occurs
     *  while dong so, the data source may use TileLayer::setError.
     *  To store any extra information of interest such as timings or sizes,
     *  TileLayer::setInfo() may be used.
     */
    virtual void fill(TileFeatureLayer::Ptr const& featureTile) = 0;

    /** Called by mapget::Hub worker. Dispatches to Cache or fill(...) on miss. */
    virtual TileFeatureLayer::Ptr get(MapTileKey const& k, Cache& cache, DataSourceInfo const& info);
};

}
