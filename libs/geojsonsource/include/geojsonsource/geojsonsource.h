#pragma once

#include <unordered_set>
#include <string>

#include "mapget/model/featurelayer.h"
#include "mapget/model/sourcedatalayer.h"
#include "mapget/service/datasource.h"

namespace mapget::geojsonsource
{

/**
 * Data Source which may be used to test a directory which
 * contains feature tiles as legacy DBI/LiveLab GeoJSON exports.
 * Each tile in the designated directory must be named `<packed-tile-id>.geojson`.
 *
 * You may use the script docs/export-classic-routing-tiles.py, to
 * generate an export of NDS.Classic tiles which can be used
 * with this class.
 *
 * Note: This data source was mainly developed as a scalability test
 *  scenario for erdblick, and should NOT BE USED IN PRODUCTION.
 *  In the future, the DBI will export the same GeoJSON feature model that
 *  is understood by mapget, and a GeoJSON data source will be part
 *  of the mapget code base.
 */
class GeoJsonSource : public mapget::DataSource
{
public:
    /**
     * Construct a GeoJSON data source from a directory containing
     * GeoJSON tiles like <packed-tile-id>.geojson.
     * @param inputDir The directory with the GeoJSON tiles.
     * @param withAttrLayers Flag indicating, whether compound GeoJSON
     *  properties shall be converted to mapget attribute layers.
     */
    GeoJsonSource(const std::string& inputDir, bool withAttrLayers, const std::string& mapId="");

    /** DataSource Interface */
    mapget::DataSourceInfo info() override;
    void fill(mapget::TileFeatureLayer::Ptr const&) override;
    void fill(mapget::TileSourceDataLayer::Ptr const&) override;

private:
    mapget::DataSourceInfo info_;
    std::unordered_set<uint64_t> coveredMapgetTileIds_;
    std::string inputDir_;
    bool withAttrLayers_ = true;
};

}  // namespace mapget::geojsonsource
