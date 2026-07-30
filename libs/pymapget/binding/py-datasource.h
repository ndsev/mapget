#pragma once

#include "mapget/http-datasource/datasource-server.h"

#include <chrono>
#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;
using namespace py::literals;

namespace
{
py::object datasourceJsonToPython(nlohmann::json const& j)
{
    switch (j.type()) {
    case nlohmann::json::value_t::null:
        return py::none();
    case nlohmann::json::value_t::boolean:
        return py::bool_(j.get<bool>());
    case nlohmann::json::value_t::number_integer:
        return py::int_(j.get<int64_t>());
    case nlohmann::json::value_t::number_unsigned:
        return py::int_(j.get<uint64_t>());
    case nlohmann::json::value_t::number_float:
        return py::float_(j.get<double>());
    case nlohmann::json::value_t::string:
        return py::str(j.get<std::string>());
    case nlohmann::json::value_t::array: {
        py::list result;
        for (auto const& item : j)
            result.append(datasourceJsonToPython(item));
        return result;
    }
    case nlohmann::json::value_t::object: {
        py::dict result;
        for (auto const& [key, value] : j.items())
            result[py::str(key)] = datasourceJsonToPython(value);
        return result;
    }
    default:
        mapget::raise("Unsupported JSON value type.");
    }
}
}

void bindDataSourceServer(py::module_& m)
{
    using namespace mapget;
    using namespace simfil;

    py::enum_<LayerType>(m, "LayerType", R"pbdoc(
        Mapget layer category.

        Feature and source-data layers are currently the primary layer types
        exposed through the Python tile-building API.
    )pbdoc")
        .value("FEATURES", LayerType::Features)
        .value("HEIGHTMAP", LayerType::Heightmap)
        .value("ORTHO_IMAGE", LayerType::OrthoImage)
        .value("GLTF", LayerType::GLTF)
        .value("SOURCE_DATA", LayerType::SourceData);

    py::class_<MapTileKey>(m, "MapTileKey", R"pbdoc(
        Fully qualified tile address used by datasource, cache and locate APIs.

        A key contains the layer type, map id, layer id, and
        ndslive.math.PackedTileId.
    )pbdoc")
        .def(py::init<>(), "Construct an empty map tile key.")
        .def(py::init<std::string const&>(), py::arg("value"), "Parse a map tile key from its string form.")
        .def(py::init<LayerType, std::string, std::string, TileId>(),
            py::arg("layer_type"),
            py::arg("map_id"),
            py::arg("layer_id"),
            py::arg("tile_id"),
            "Construct a map tile key from individual components.")
        .def_readwrite("layer_type", &MapTileKey::layer_, "Layer category addressed by this key.")
        .def_readwrite("map_id", &MapTileKey::mapId_, "Map identifier addressed by this key.")
        .def_readwrite("layer_id", &MapTileKey::layerId_, "Layer identifier addressed by this key.")
        .def_readwrite("tile_id", &MapTileKey::tileId_, "Packed tile id addressed by this key.")
        .def("to_string", &MapTileKey::toString, "Convert this key to its stable string form.")
        .def("__str__", &MapTileKey::toString);

    py::class_<LocateRequest>(m, "LocateRequest", R"pbdoc(
        Request asking a datasource to locate a feature id in map tiles.

        Datasources return cheap tile candidates with portable selectors;
        mapget resolves secondary ids against normally loaded complete tiles.
    )pbdoc")
        .def(py::init([](std::string mapId, std::string typeId, KeyValuePairVec const& featureIdParts) {
                return LocateRequest(std::move(mapId), std::move(typeId), castToKeyValue(castToKeyValueView(featureIdParts)));
            }),
            py::arg("map_id"),
            py::arg("type_id"),
            py::arg("feature_id_parts"),
            "Construct a locate request for a feature id.")
        .def(py::init([](py::dict const& dict) {
                py::module jsonModule = py::module::import("json");
                auto jsonString = jsonModule.attr("dumps")(dict).cast<std::string>();
                return LocateRequest(nlohmann::json::parse(jsonString));
            }),
            py::arg("dict"),
            "Construct a locate request from a Python dictionary.")
        .def_readwrite("map_id", &LocateRequest::mapId_, "Map in which the feature should be located.")
        .def_readwrite("type_id", &LocateRequest::typeId_, "Feature type id to locate.")
        .def_property("feature_id_parts",
            [](LocateRequest const& self) {
                KeyValuePairVec result;
                for (auto const& [key, value] : self.featureId_) {
                    std::visit(
                        [&result, &key](auto&& vv) {
                            result.emplace_back(key, vv);
                        },
                        value);
                }
                return result;
            },
            [](LocateRequest& self, KeyValuePairVec const& parts) {
                self.setFeatureId(castToKeyValueView(parts));
            },
            "Feature-id parts as `(part_id, value)` pairs.")
        .def("set_feature_id", [](LocateRequest& self, KeyValuePairVec const& parts) {
                self.setFeatureId(castToKeyValueView(parts));
            },
            py::arg("feature_id_parts"),
            "Replace the feature-id parts.")
        .def("get_int_id_part", &LocateRequest::getIntIdPart,
            py::arg("part_id"),
            "Get an integer id part by name.")
        .def("get_str_id_part", &LocateRequest::getStrIdPart,
            py::arg("part_id"),
            "Get a string id part by name.")
        .def("to_dict", [](LocateRequest const& self) {
                return datasourceJsonToPython(self.serialize());
            },
            "Serialize the request to a Python dictionary.")
        .def("to_json", [](LocateRequest const& self) { return self.serialize().dump(); },
            "Serialize the request to a JSON string.");

    py::class_<LocateResponse, LocateRequest>(m, "LocateResponse", R"pbdoc(
        Canonical result of the complete mapget Service locate operation.

        Inherits the resolved feature id fields and adds the tile key where the
        feature was found.
    )pbdoc")
        .def(py::init<LocateRequest const&>(),
            py::arg("request"),
            "Construct a response initialized from a request.")
        .def(py::init([](py::dict const& dict) {
                py::module jsonModule = py::module::import("json");
                auto jsonString = jsonModule.attr("dumps")(dict).cast<std::string>();
                return LocateResponse(nlohmann::json::parse(jsonString));
            }),
            py::arg("dict"),
            "Construct a locate response from a Python dictionary.")
        .def_readwrite("tile_key", &LocateResponse::tileKey_, "Tile key that may contain the located feature.")
        .def("to_dict", [](LocateResponse const& self) {
                return datasourceJsonToPython(self.serialize());
            },
            "Serialize the response to a Python dictionary.")
        .def("to_json", [](LocateResponse const& self) { return self.serialize().dump(); },
            "Serialize the response to a JSON string.");

    py::class_<LocateCandidate>(
        m,
        "LocateCandidate",
        R"pbdoc(
        Cheap datasource locate candidate. The service loads `tile_key`
        normally and applies the portable selector inside that tile.
    )pbdoc")
        .def(
            py::init<
                MapTileKey,
                std::string>(),
            py::arg("tile_key"),
            py::arg("canonical_feature_id"),
            "Construct an exact-primary-id candidate.")
        .def(
            py::init(
                [](MapTileKey tileKey,
                   std::string typeId,
                   std::string featureFilter,
                   py::dict const& bindings)
                {
                    py::module jsonModule =
                        py::module::import("json");
                    auto bindingsJson =
                        nlohmann::json::parse(
                            jsonModule
                                .attr("dumps")(
                                    bindings)
                                .cast<std::string>());
                    return LocateCandidate(
                        nlohmann::json{
                            {
                                "tileId",
                                tileKey.toString()},
                            {
                                "selector",
                                {
                                    {
                                        "typeId",
                                        std::move(
                                            typeId)},
                                    {
                                        "featureFilter",
                                        std::move(
                                            featureFilter)},
                                    {
                                        "bindings",
                                        std::move(
                                            bindingsJson)},
                                }},
                        });
                }),
            py::arg("tile_key"),
            py::arg("type_id"),
            py::arg("feature_filter"),
            py::arg("bindings") = py::dict{},
            "Construct a typed SIMFIL candidate with scalar bindings.")
        .def(py::init([](py::dict const& dict) {
                py::module jsonModule =
                    py::module::import("json");
                auto jsonString =
                    jsonModule.attr("dumps")(dict)
                        .cast<std::string>();
                return LocateCandidate(
                    nlohmann::json::parse(
                        jsonString));
            }),
            py::arg("dict"),
            "Construct an exact or filtered candidate from its wire dictionary.")
        .def_readwrite(
            "tile_key",
            &LocateCandidate::tileKey_)
        .def("to_dict", [](LocateCandidate const& self) {
                return datasourceJsonToPython(
                    self.serialize());
            })
        .def("to_json", [](LocateCandidate const& self) {
                return self.serialize().dump();
            });

    py::class_<AttachmentRequest>(
        m,
        "AttachmentRequest",
        "Request for one named feature-tile side payload.")
        .def(py::init<>())
        .def_readwrite(
            "tile_key",
            &AttachmentRequest::tileKey_)
        .def_readwrite(
            "name",
            &AttachmentRequest::name_)
        .def_readwrite(
            "source_id",
            &AttachmentRequest::sourceId_);

    py::class_<AttachmentResponse>(
        m,
        "AttachmentResponse",
        "Immutable bytes and HTTP metadata for a named tile attachment.")
        .def(
            py::init(
                [](std::string name,
                   std::string mimeType,
                   py::bytes bytes,
                   std::optional<std::string> etag)
                {
                    auto value =
                        bytes.cast<std::string>();
                    return AttachmentResponse{
                        .name_ =
                            std::move(name),
                        .mimeType_ =
                            std::move(mimeType),
                        .bytes_ =
                            std::make_shared<
                                std::vector<
                                    uint8_t> const>(
                                value.begin(),
                                value.end()),
                        .etag_ =
                            std::move(etag),
                    };
                }),
            py::arg("name"),
            py::arg("mime_type") =
                "application/octet-stream",
            py::arg("bytes") = py::bytes{},
            py::arg("etag") = std::nullopt)
        .def_readwrite(
            "name",
            &AttachmentResponse::name_)
        .def_readwrite(
            "mime_type",
            &AttachmentResponse::mimeType_)
        .def_property(
            "bytes",
            [](AttachmentResponse const& self)
            {
                if (!self.bytes_) {
                    return py::bytes{};
                }
                return py::bytes(
                    reinterpret_cast<
                        char const*>(
                        self.bytes_->data()),
                    self.bytes_->size());
            },
            [](AttachmentResponse& self,
               py::bytes bytes)
            {
                auto value =
                    bytes.cast<std::string>();
                self.bytes_ =
                    std::make_shared<
                        std::vector<
                            uint8_t> const>(
                        value.begin(),
                        value.end());
            })
        .def_readwrite(
            "etag",
            &AttachmentResponse::etag_);

    py::class_<DataSourceServer, std::shared_ptr<DataSourceServer>>(m, "DataSourceServer", R"pbdoc(
        Small HTTP datasource server implemented from Python callbacks.

        Construct it with a `DataSourceInfo` dictionary, attach tile/locate
        callbacks, then call `go()` so a mapget service can connect through
        `RemoteDataSource`.
    )pbdoc")
        .def(
            py::init(
                [](py::dict const& dict)
                {
                    // import json.dumps
                    py::module json_module = py::module::import("json");
                    py::function json_dumps = json_module.attr("dumps");

                    // convert py::dict to JSON string
                    auto json_str = json_dumps(dict).cast<std::string>();

                    // parse JSON string into nlohmann::json
                    nlohmann::json j = nlohmann::json::parse(json_str);

                    // construct DataSource
                    return std::make_unique<DataSourceServer>(DataSourceInfo::fromJson(j));
                }),
            R"pbdoc(
                Construct a DataSource with a DataSourceInfo metadata instance.
        )pbdoc",
            py::arg("info_dict"))
        .def(
            "on_tile_feature_request",
            [](DataSourceServer& self, py::function callback) -> DataSourceServer& {
                return self.onTileFeatureRequest(
                    [callback = std::move(callback)](TileFeatureLayer::Ptr tile) {
                        py::gil_scoped_acquire gil;
                        callback(std::move(tile));
                    });
            },
            py::arg("callback"),
            R"pbdoc(
            Set the Callback which will be invoked when a `/tile`-request for a
            feature layer is received.
            The callback argument is a fresh TileFeatureLayer, which the callback must
            fill according to the set TileFeatureLayer's layer info and tile id. If an
            error occurs while filling the tile, the callback can use
            TileFeatureLayer::setError(...) to signal the error downstream.
        )pbdoc")
        .def(
            "on_tile_sourcedata_request",
            [](DataSourceServer& self, py::function callback) -> DataSourceServer& {
                return self.onTileSourceDataRequest(
                    [callback = std::move(callback)](TileSourceDataLayer::Ptr tile) {
                        py::gil_scoped_acquire gil;
                        callback(std::move(tile));
                    });
            },
            py::arg("callback"),
            R"pbdoc(
            Set the Callback which will be invoked when a `/tile`-request for a
            source-data layer is received.
            The callback argument is a fresh TileSourceDataLayer, which the callback must
            fill according to the set TileSourceDataLayer's layer info and tile id. If an
            error occurs while filling the tile, the callback can use
            TileSourceDataLayer::setError(...) to signal the error downstream.
        )pbdoc")
        .def(
            "on_locate_request",
            [](DataSourceServer& self, py::function callback) -> DataSourceServer& {
                return self.onLocateRequest(
                    [callback = std::move(callback)](LocateRequest const& request) {
                        py::gil_scoped_acquire gil;
                        return callback(request).cast<
                            std::vector<LocateCandidate>>();
                    });
            },
            py::arg("callback"),
            R"pbdoc(
            Set the Callback which will be invoked when a `/locate` request is received.
            The callback receives a LocateRequest and must return a list of
            cheap LocateCandidate objects. It must not fill or fetch tile data.
        )pbdoc")
        .def(
            "on_attachment_request",
            [](DataSourceServer& self,
               py::function callback)
                -> DataSourceServer&
            {
                return self.onAttachmentRequest(
                    [callback =
                         std::move(callback)](
                        AttachmentRequest const&
                            request)
                        -> std::optional<
                            AttachmentResponse>
                    {
                        py::gil_scoped_acquire gil;
                        auto result =
                            callback(request);
                        if (result.is_none()) {
                            return {};
                        }
                        return result.cast<
                            AttachmentResponse>();
                    });
            },
            py::arg("callback"),
            R"pbdoc(
            Set the callback invoked for `/attachment`. It receives an
            AttachmentRequest and returns AttachmentResponse or None.
        )pbdoc")
        .def(
            "on_cache_expired",
            [](DataSourceServer& self, py::function callback) -> DataSourceServer& {
                return self.onCacheExpired(
                    [callback = std::move(callback)](
                        MapTileKey const& tileKey,
                        std::chrono::system_clock::time_point expiredAt) {
                        py::gil_scoped_acquire gil;
                        auto expiredAtUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            expiredAt.time_since_epoch()).count();
                        callback(tileKey, expiredAtUs);
                    });
            },
            py::arg("callback"),
            R"pbdoc(
            Set the callback invoked when a service reports that an expired cached
            tile for this datasource is being refreshed. The callback receives
            (MapTileKey, expired_at_unix_microseconds).
        )pbdoc")
        .def(
            "go",
            &DataSourceServer::go,
            py::arg("interfaceAddr") = "0.0.0.0",
            py::arg("port") = 0,
            py::arg("waitMs") = 100,
            R"pbdoc(
            Launch the DataSource server in its own thread. Use the stop-function to
            stop the thread. The server will also be stopped automatically, if the
            DataSource object is destroyed. An exception will be thrown if this
            instance is already running, or if the server fails to launch within waitMs.
        )pbdoc")
        .def(
            "is_running",
            &DataSourceServer::isRunning,
            R"pbdoc(
            Returns true if this instance is currently running (go() was called and not stopped).
        )pbdoc")
        .def(
            "stop",
            &DataSourceServer::stop,
            R"pbdoc(
            Stop this instance. Will be a no-op if this instance is not running.
        )pbdoc")
        .def(
            "wait_for_signal",
            &DataSourceServer::waitForSignal,
            py::call_guard<py::gil_scoped_release>(),
            R"pbdoc(
            Blocks until SIGINT or SIGTERM is received, then shuts down the server.
            Note: You can never run this function in parallel for multiple sources
            within the same process!
        )pbdoc")
        .def(
            "port",
            &DataSourceServer::port,
            R"pbdoc(
            Get the port currently used by the instance, or 0 if go() has never been called.
        )pbdoc")
        .def(
            "info",
            [](DataSourceServer& self) { return datasourceJsonToPython(self.info().toJson()); },
            R"pbdoc(
            Get the DataSourceInfo metadata which this instance was constructed with.
        )pbdoc");
}
