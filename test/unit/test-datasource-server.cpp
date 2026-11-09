#include "mapget/http-datasource/datasource-server.h"
#include "mapget/log.h"
#include "mapget/model/mapschema.h"
#include "mapget/model/stream.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"

using namespace mapget;
using namespace nlohmann;

namespace
{

/** Isolated protocol peer with malformed catalogs and controllable schema-response delivery. */
class SchemaHttpPeer final : public HttpServer
{
public:
    explicit SchemaHttpPeer(bool holdSchema = false) : holdSchema_(holdSchema)
    {
        printPortToStdOut(true);
        for (auto field : {"speed", "width"}) {
            auto metadata = json::parse(R"({
                "mapId": "SchemaPeerMap",
                "layers": {"Road": {
                    "featureTypes": [],
                    "featureModelSchema": {"type": "object"}
                }}
            })");
            if (holdSchema)
                metadata["mapFormat"] = {{"name", "schema-waiter-cancellation"}, {"version", "1"}};
            metadata["stringPoolId"] = std::string("schema-peer-") + field;
            metadata["layers"]["Road"]["featureModelSchema"]["properties"] = {{field, {{"type", "integer"}}}};
            infos_.push_back(DataSourceInfo::fromJson(metadata));
        }
    }

    ~SchemaHttpPeer() override { stop(); }

private:
    void setup(drogon::HttpAppFramework& app) override
    {
        app.registerHandler("/info", [this](drogon::HttpRequestPtr const&,
            std::function<void(drogon::HttpResponsePtr const&)>&& callback) {
            auto response = drogon::HttpResponse::newHttpResponse();
            response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            response->setBody(infos_.front().toJson().dump());
            callback(response);
        }, {drogon::Get});
        app.registerHandler("/sources", [this](drogon::HttpRequestPtr const& request,
            std::function<void(drogon::HttpResponsePtr const&)>&& callback) {
            auto body = json::array();
            auto count = request->getHeader("x-schema-test-mode") == "ambiguous" ? 2 : 1;
            for (int index = 0; index < count; ++index) {
                auto source = infos_[index].toJson();
                source["sourceId"] = "schema-peer-" + std::to_string(index);
                body.push_back(std::move(source));
            }
            auto response = drogon::HttpResponse::newHttpResponse();
            response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            response->setBody(body.dump());
            callback(response);
        }, {drogon::Get});
        app.registerHandler("/schema", [this](drogon::HttpRequestPtr const& request,
            std::function<void(drogon::HttpResponsePtr const&)>&& callback) {
            auto sourceId = request->getParameter("sourceId");
            auto index = sourceId.empty() || sourceId == "schema-peer-0" ? 0 : sourceId == "schema-peer-1" ? 1 : -1;
            auto response = drogon::HttpResponse::newHttpResponse();
            if (index < 0 || request->getParameter("schemaId") != infos_[index].schemaId_)
                response->setStatusCode(drogon::k404NotFound);
            else if (request->getHeader("if-none-match") == "*") {
                response->setStatusCode(drogon::k304NotModified);
                std::lock_guard lock(schemaMutex_);
                ++schemaProbes_;
            }
            else {
                auto mode = request->getHeader("x-schema-test-mode");
                if (mode == "wrong-artifact")
                    index = 1 - index;
                std::ostringstream output(std::ios::out | std::ios::binary);
                if (!infos_[index].mapSchema_->write(output))
                    throw std::runtime_error("Schema test peer serialization failed.");
                auto body = std::move(output).str();
                if (mode == "corrupt-artifact")
                    body.back() ^= 1;
                response->setContentTypeCode(drogon::CT_APPLICATION_OCTET_STREAM);
                response->setBody(std::move(body));
                std::lock_guard lock(schemaMutex_);
                ++schemaFetches_;
                if (holdSchema_) {
                    heldSchemas_.emplace_back([callback = std::move(callback), response] {
                        callback(response);
                    });
                    return;
                }
            }
            callback(response);
        }, {drogon::Get});
        // Retain callbacks rather than blocking the I/O loop: probes/control must remain live.
        app.registerHandler("/schema-test", [this](drogon::HttpRequestPtr const& request,
            std::function<void(drogon::HttpResponsePtr const&)>&& callback) {
            std::vector<std::function<void()>> ready;
            json state;
            {
                std::lock_guard lock(schemaMutex_);
                if (request->method() == drogon::Post) {
                    holdSchema_ = false;
                    ready.swap(heldSchemas_);
                }
                state = {{"probes", schemaProbes_}, {"fetches", schemaFetches_},
                    {"pending", heldSchemas_.size()}, {"schemaId", infos_.front().schemaId_}};
            }
            for (auto const& send : ready)
                send();
            auto response = drogon::HttpResponse::newHttpResponse();
            response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            response->setBody(state.dump());
            callback(response);
        }, {drogon::Get, drogon::Post});
        app.registerHandler("/tiles", [this](drogon::HttpRequestPtr const& request,
            std::function<void(drogon::HttpResponsePtr const&)>&& callback) {
            auto sourceId = json::parse(request->body()).at("requests").at(0).value("sourceId", "schema-peer-0");
            auto const& info = infos_[sourceId == "schema-peer-1" ? 1 : 0];
            auto tile = std::make_shared<PartitionFeatureLayer>(
                TileId::fromValue(131073), info.stringPoolId_, info.mapId_, info.getLayer("Road"),
                std::make_shared<StringPool>(info.stringPoolId_));
            std::string body;
            TileLayerStream::StringPoolOffsetMap offsets;
            TileLayerStream::Writer writer(
                [&](std::string const& bytes, auto) { body.append(bytes); }, offsets);
            writer.write(tile);
            auto response = drogon::HttpResponse::newHttpResponse();
            response->setContentTypeString("application/binary");
            response->setBody(std::move(body));
            callback(response);
        }, {drogon::Post});
    }

    std::vector<DataSourceInfo> infos_;
    std::mutex schemaMutex_;
    bool holdSchema_ = false;
    size_t schemaProbes_ = 0, schemaFetches_ = 0;
    std::vector<std::function<void()>> heldSchemas_;
};

} // namespace

int main(int argc, char** argv)
{
    setLogLevel("trace", log());

    if ((argc == 2 || argc == 3) && std::string_view(argv[1]) == "--schema-http-fixture") {
        SchemaHttpPeer peer(argc == 3 && std::string_view(argv[2]) == "--hold-schema");
        peer.go("127.0.0.1");
        peer.waitForSignal();
        return 0;
    }

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
                MapPartitionKey{
                    LayerType::Features,
                    "Tropico",
                    "WayLayer",
                    TileId::fromValue(tileId)},
                fmt::format("Way.Area42.{}", wayId))};
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
