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
            auto const wayId =
                tile->tileId().value() ==
                    131076
                ? int64_t{80}
                : tile->tileId().value() ==
                        131077
                    ? int64_t{81}
                    : int64_t{0};
            auto f = tile->newFeature(
                "Way",
                {
                    {"areaId", "Area42"},
                    {"wayId", wayId}});
            auto g = f->geom()->newGeometry(GeomType::Line);
            g->setName("centerline");
            g->append({42., 11});
            g->append({42., 12});
            if (wayId == 80) {
                f->addRelation(
                    "connected",
                    "Way",
                    {
                        {"areaId", "Area42"},
                        {"wayId", int64_t{81}},
                    });
            }
        });
    ds.onTileSourceDataRequest([&](const auto&) {});
    ds.onLocateRequest(
        [&](LocateRequest const& request) ->
            std::vector<LocateCandidate>
        {
            auto const wayId =
                request.getIntIdPart(
                    "wayId")
                    .value_or(0);
            auto const tileId =
                wayId == 80
                ? 131076
                : wayId == 81
                    ? 131077
                    : 131073;
            return {LocateCandidate(
                MapTileKey{
                    LayerType::Features,
                    "Tropico",
                    "WayLayer",
                    TileId::fromValue(
                        tileId)},
                fmt::format(
                    "Way.Area42.{}",
                    wayId))};
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

    ds.go("127.0.0.1");
    ds.waitForSignal();
    return 0;
}
