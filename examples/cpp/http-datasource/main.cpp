#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include "mapget/http-datasource/datasource-server.h"
#include "mapget/log.h"

using namespace mapget;
using json = nlohmann::json;
namespace fs = std::filesystem;

// Class to encapsulate all logic of our data source
class MyRemoteDataSource
{
private:
    DataSourceServer ds;
    int port = 0;
    int servedTiles = 0;

public:
    // The constructor
    MyRemoteDataSource(const std::string& jsonFilename, int port)
        : ds(loadDataSourceInfoFromJson(jsonFilename)), port(port)
    {
        // Handle tile requests
        ds.onTileRequest(
            [this](auto&& tile)
            {
                fill(tile);
            });
    }

    void fill(TileFeatureLayer::Ptr const& tile)
    {
        // Add some ID parts that are shared by all features in the tile.
        tile->setPrefix({{"areaId", "BestArea"}});

        // Create a feature with line geometry
        auto feature1 = tile->newFeature("Way", {{"wayId", 42}});
        auto line = feature1->geom()->newGeometry(Geometry::GeomType::Line, 2);
        line->append({41., 10.});
        line->append({43., 11.});

        // Use high-level geometry API
        feature1->addPoint(tile->tileId().center());
        feature1->addPoints({tile->tileId().ne(), tile->tileId().sw()});
        feature1->addLine({{41.5, 10.5, 0}, {41.6, 10.7}});
        feature1->addMesh({{41.5, 10.5, 0}, {41.6, 10.7}, {41.5, 10.3}});
        feature1->addPoly({{41.5, 10.5, 0}, {41.6, 10.7}, {41.5, 10.3}, {41.8, 10.9}});

        // Add a fixed attribute
        feature1->attributes()->addField("main_ingredient", "Pepper");

        // Add an attribute layer
        auto attrLayer = feature1->attributeLayers()->newLayer("cheese");
        auto attr = attrLayer->newAttribute("mozzarella");
        attr->setDirection(Attribute::Direction::Positive);
        attr->addField("smell", "neutral");

        // Set some additional info on the tile
        tile->setInfo("servedTiles", ++servedTiles);
    }

    // The function to load the DataSourceInfo from a JSON file
    static DataSourceInfo loadDataSourceInfoFromJson(const std::string& filename)
    {
        std::string full_path = fs::current_path().string() + "/" + filename;
        log().info("Reading info from {}", full_path);
        std::ifstream i(full_path);
        json j;
        i >> j;
        return DataSourceInfo::fromJson(j);
    }

    // The function to start the server
    void run()
    {
        ds.go("0.0.0.0", port);
        log().info("Running...");
        ds.waitForSignal();
    }
};

int main(int argc, char** argv)
{
    int port = (argc > 1) ? std::stoi(argv[1]) : 0;  // get port from command line argument
    log().info("Running on port {}", port);
    MyRemoteDataSource ds("sample_datasource_info.json", port);
    ds.run();
    return 0;
}
