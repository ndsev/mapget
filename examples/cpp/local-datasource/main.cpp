#include "mapget/service/service.h"
#include "mapget/log.h"

using namespace mapget;

class MyLocalDataSource : public mapget::DataSource
{
public:
    DataSourceInfo info() override {
        return DataSourceInfo::fromJson(R"(
        {
            "mapId": "Tropico",
            "layers": {
                "WayLayer": {
                    "featureTypes":
                    [
                        {
                            "name": "Way",
                            "uniqueIdCompositions":
                            [
                                [
                                    {
                                        "partId": "areaId",
                                        "description": "String which identifies the map area.",
                                        "datatype": "STR"
                                    },
                                    {
                                        "partId": "wayId",
                                        "description": "Globally Unique 32b integer.",
                                        "datatype": "U32"
                                    }
                                ]
                            ]
                        }
                    ]
                }
            }
        }
        )"_json);
    }

    void fill(TileFeatureLayer::Ptr const& tile) override {
        // Add some ID parts that are shared by all features in the tile.
        tile->setPrefix({{"areadId", "BestArea"}});

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
    }
};

int main(int argc, char** argv)
{
    // Create a local mapget service instance
    mapget::Service service;

    // Add a local datasource
    service.add(std::make_shared<MyLocalDataSource>());

    // Create a request. The request will immediately start to be worked on.
    service
        .request(std::make_shared<Request>(
            "Tropico",
            "WayLayer",
            std::vector<TileId>{TileId(12345), TileId(67689)},
            [](auto&& result)
            { log().info("Got {}", MapTileKey(*result).toString()); }))
        ->wait();
    return 0;
}
