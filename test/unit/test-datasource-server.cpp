#include "mapget/http-datasource/datasource-server.h"
#include "mapget/log.h"

#include "nlohmann/json.hpp"

using namespace mapget;
using namespace nlohmann;

int main()
{
    setLogLevel("trace", log());

    auto info = DataSourceInfo::fromJson(R"(
    {
        "stringPoolId": "test-datasource",
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
            },
            "SourceData-WayLayer": {
                "type": "SourceData"
            }
        }
    }
    )"_json);

    DataSourceServer ds(info);
    ds.onTileFeatureRequest(
        [&](const auto& tile)
        {
            tile->setGlbAttachmentName(
                "ways.glb");
            auto f = tile->newFeature("Way", {{"areaId", "Area42"}, {"wayId", 0}});
            auto g = f->geom()->newGeometry(GeomType::Line);
            g->append({42., 11});
            g->append({42., 12});
        });
    ds.onTileSourceDataRequest([&](const auto&) {});
    ds.onLocateRequest(
        [&](LocateRequest const& request) -> std::vector<LocateResponse>
        {
            LocateResponse response(request);
            response.tileKey_.layerId_ = "WayLayer";
            response.tileKey_.tileId_ = TileId::fromValue(131073);
            return {response};
        });
    ds.onAttachmentRequest(
        [](AttachmentRequest const& request)
            -> std::optional<AttachmentResponse>
        {
            if (request.name_ != "ways.glb") {
                return {};
            }
            return AttachmentResponse{
                .name_ = request.name_,
                .mimeType_ =
                    "model/gltf-binary",
                .bytes_ =
                    std::make_shared<
                        std::vector<
                            uint8_t> const>(
                        std::initializer_list<
                            uint8_t>{
                            0x67,
                            0x6c,
                            0x54,
                            0x46}),
                .etag_ = "\"ways-v1\"",
            };
        });

    ds.go("127.0.0.1", 0, 5000);
    ds.waitForSignal();
    return 0;
}
