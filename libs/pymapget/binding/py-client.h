#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <queue>

#include "mapget/http-service/http-client.h"

namespace py = pybind11;

namespace {
using json = nlohmann::json;

py::object json_to_py_value(const json& j)
{
    switch (j.type()) {
    case json::value_t::null: {
        return py::none();
    }
    case json::value_t::boolean: {
        return py::bool_(j.get<bool>());
    }
    case json::value_t::number_integer: {
        return py::int_(j.get<int>());
    }
    case json::value_t::number_unsigned: {
        return py::int_(j.get<unsigned int>());
    }
    case json::value_t::number_float: {
        return py::float_(j.get<double>());
    }
    case json::value_t::string: {
        return py::str(j.get<std::string>());
    }
    case json::value_t::array: {
        py::list py_list;
        for (const auto& element : j) {
            py_list.append(json_to_py_value(element));
        }
        return py_list;
    }
    case json::value_t::object: {
        py::dict py_dict;
        for (json::const_iterator it = j.begin(); it != j.end(); ++it) {
            py_dict[py::str(it.key())] = json_to_py_value(it.value());
        }
        return py_dict;
    }
    default: {
        mapget::raise("Invalid JSON value type");
    }
    }
}

mapget::FeatureLayerFilterBinding filter_binding_from_python(
    py::handle value,
    std::string const& name)
{
    if (value.is_none()) {
        return std::monostate{};
    }
    if (py::isinstance<py::bool_>(value)) {
        return value.cast<bool>();
    }
    if (py::isinstance<py::int_>(value)) {
        try {
            return value.cast<int64_t>();
        }
        catch (py::cast_error const&) {
            throw py::value_error(
                "filter binding '" + name +
                "' exceeds the signed 64-bit integer range");
        }
    }
    if (py::isinstance<py::float_>(value)) {
        auto const result = value.cast<double>();
        if (!std::isfinite(result)) {
            throw py::value_error(
                "filter binding '" + name + "' must be finite");
        }
        return result;
    }
    if (py::isinstance<py::str>(value)) {
        return value.cast<std::string>();
    }
    throw py::value_error(
        "filter binding '" + name +
        "' must be None, bool, int, float, or str");
}

std::map<std::string, mapget::FeatureLayerFilterBinding>
filter_bindings_from_python(py::dict const& bindings)
{
    std::map<std::string, mapget::FeatureLayerFilterBinding> result;
    for (auto const& [key, value] : bindings) {
        if (!py::isinstance<py::str>(key)) {
            throw py::value_error("filter binding names must be strings");
        }
        auto name = key.cast<std::string>();
        if (name.empty()) {
            throw py::value_error("filter binding names must not be empty");
        }
        result.emplace(
            name,
            filter_binding_from_python(value, name));
    }
    return result;
}
}

namespace mapget
{

namespace py = pybind11;

class PyRequest : public LayerTilesRequest
{
public:
    using LayerTilesRequest::LayerTilesRequest;

    void notifyResult(TileLayer::Ptr result) override {
        std::unique_lock lock(bufferMutex_);
        buffer_.push(result);
        bufferSignal_.notify_one(); // Signal that a new result is available
        LayerTilesRequest::notifyResult(result);
    }

    TileLayer::Ptr next() {
        std::unique_lock lock(bufferMutex_);
        bufferSignal_.wait(lock, [this](){ return !buffer_.empty() ||
                                 this->getStatus() != RequestStatus::Open; });
        if (buffer_.empty()) {
            throw py::stop_iteration();
        } else {
            auto result = buffer_.front();
            buffer_.pop();
            return result;
        }
    }

private:
    std::queue<TileLayer::Ptr> buffer_;
    std::mutex bufferMutex_;
    std::condition_variable bufferSignal_;
};

class PyFilterRequest : public FeatureLayerFilterTilesRequest
{
public:
    using FeatureLayerFilterTilesRequest::FeatureLayerFilterTilesRequest;

    void notifyResult(TileSubsetLayer::Ptr result) override {
        std::unique_lock lock(bufferMutex_);
        buffer_.push(result);
        bufferSignal_.notify_one();
        FeatureLayerFilterTilesRequest::notifyResult(std::move(result));
    }

    TileSubsetLayer::Ptr next() {
        std::unique_lock lock(bufferMutex_);
        bufferSignal_.wait(lock, [this](){ return !buffer_.empty() ||
                                 this->getStatus() != RequestStatus::Open; });
        if (buffer_.empty()) {
            throw py::stop_iteration();
        } else {
            auto result = buffer_.front();
            buffer_.pop();
            return result;
        }
    }

private:
    std::queue<TileSubsetLayer::Ptr> buffer_;
    std::mutex bufferMutex_;
    std::condition_variable bufferSignal_;
};
}

void bindHttpClient(py::module_& m)
{
    using namespace mapget;

    py::enum_<FeatureLayerFilterScope>(m, "FilterScope")
        .value("FEATURE", FeatureLayerFilterScope::Feature)
        .value("ATTRIBUTE", FeatureLayerFilterScope::Attribute)
        .value("RELATION", FeatureLayerFilterScope::Relation)
        .value("AUTO", FeatureLayerFilterScope::Auto);

    py::class_<FeatureLayerPointGridGroup>(m, "PointGridGroup")
        .def(
            py::init(
                [](std::array<double, 3> const& origin,
                   std::array<double, 3> const& cellSize)
                {
                    return FeatureLayerPointGridGroup{
                        .origin_ = {
                            origin[0],
                            origin[1],
                            origin[2]},
                        .cellSize_ = {
                            cellSize[0],
                            cellSize[1],
                            cellSize[2]},
                    };
                }),
            py::arg("origin") =
                std::array<double, 3>{0.0, 0.0, 0.0},
            py::arg("cell_size") =
                std::array<double, 3>{1.0, 1.0, 1.0});

    py::class_<FeatureLayerStoredRelationOptions>(
        m,
        "StoredRelationOptions")
        .def(
            py::init(
                [](std::optional<std::string> namePattern,
                   bool recursive,
                   bool mergeTwoway)
                {
                    return FeatureLayerStoredRelationOptions{
                        .relationNamePattern_ =
                            std::move(namePattern),
                        .recursive_ = recursive,
                        .mergeTwoway_ = mergeTwoway,
                    };
                }),
            py::arg("name_pattern") = py::none(),
            py::arg("recursive") = false,
            py::arg("merge_twoway") = false);

    py::class_<FeatureLayerFilterRoot>(
        m,
        "FilterRoot")
        .def(
            py::init(
                [](TileId tileId,
                   std::string typeId,
                   KeyValuePairVec const& featureId,
                   size_t requestOrdinal)
                {
                    return FeatureLayerFilterRoot{
                        tileId,
                        std::move(typeId),
                        castToKeyValue(
                            castToKeyValueView(
                                featureId)),
                        requestOrdinal,
                    };
                }),
            py::arg("tile_id"),
            py::arg("type_id"),
            py::arg("feature_id_parts"),
            py::arg("request_ordinal") = 0);

    py::class_<FeatureLayerFilterChannel>(m, "FilterChannel")
        .def(
            py::init(
                [](std::string channelId,
                   FeatureLayerFilterScope scope,
                   std::optional<std::string> featureFilter,
                   std::optional<std::string> entryFilter,
                   bool rewrite,
                   std::vector<std::string> featureTypes,
                   std::vector<std::string> featureFields,
                   std::vector<std::string> entryFields,
                   uint32_t geometryTypes,
                   std::optional<std::string> geometryName,
                   std::optional<FeatureLayerPointGridGroup> group,
                   std::optional<FeatureLayerStoredRelationOptions>
                       relation)
                {
                    if (channelId.empty()) {
                        throw py::value_error(
                            "channel_id must not be empty");
                    }
                    if (geometryName && *geometryName == "*") {
                        geometryName.reset();
                    }
                    return FeatureLayerFilterChannel{
                        .channelId_ = std::move(channelId),
                        .featureFilter_ =
                            std::move(featureFilter),
                        .entryFilter_ =
                            std::move(entryFilter),
                        .scope_ = scope,
                        .rewrite_ = rewrite,
                        .featureTypes_ =
                            std::move(featureTypes),
                        .featureFields_ =
                            std::move(featureFields),
                        .entryFields_ =
                            std::move(entryFields),
                        .geometryTypes_ = geometryTypes,
                        .geometryName_ =
                            std::move(geometryName),
                        .group_ = std::move(group),
                        .relation_ = std::move(relation),
                    };
                }),
            py::arg("channel_id"),
            py::arg("scope") =
                FeatureLayerFilterScope::Feature,
            py::arg("feature_filter") = py::none(),
            py::arg("entry_filter") = py::none(),
            py::arg("rewrite") = false,
            py::arg("feature_types") =
                std::vector<std::string>{},
            py::arg("feature_fields") =
                std::vector<std::string>{},
            py::arg("entry_fields") =
                std::vector<std::string>{},
            py::arg("geometry_types") =
                FeatureLayerFilterChannel::AllGeometryTypes,
            py::arg("geometry_name") = py::none(),
            py::arg("group") = py::none(),
            py::arg("relation") = py::none());

    py::class_<PyRequest, std::shared_ptr<PyRequest>>(m, "Request", R"pbdoc(
        Client request for map data.

        A Request is defined by a map id, a map layer id, an array of tile ids,
        and a callback function for results. When a result tile is available, it is
        passed to the callback function, but it can also be retrieved by iterating
        over the Request object.

        When the Request is exhausted (i.e., when all results have been processed),
        iterating over it will raise a StopIteration exception, as per the Python
        iterator protocol.
    )pbdoc")
        .def(
            py::init(
                [](const std::string& mapId,
                   const std::string& layerId,
                   std::vector<TileId> tiles,
                   std::function<void(TileFeatureLayer::Ptr)> onFeatureResult,
                   std::function<void(TileSourceDataLayer::Ptr)> onSourceDataResult,
                   std::optional<std::string> sourceId)
                {
                    auto req = std::make_shared<PyRequest>(
                        mapId,
                        layerId,
                        std::move(tiles));
                    req->sourceId_ = std::move(sourceId);
                    req->onFeatureLayer(std::move(onFeatureResult));
                    req->onSourceDataLayer(std::move(onSourceDataResult));
                    return req;
                }),
            py::arg("map_id"),
            py::arg("layer_id"),
            py::arg("tiles"),
            py::arg("on_feature_result") = py::none(),
            py::arg("on_sourcedata_result") = py::none(),
            py::arg("source_id") = py::none(),
            py::call_guard<py::gil_scoped_acquire>(),
            R"pbdoc(
            Construct a Request.

            Args:
                map_id: The map id for which this request is dedicated.
                layer_id: The map layer id for which this request is dedicated.
                tiles: The ndslive.math.PackedTileId values for which this request is dedicated.
                on_feature_result: The callback function to be called when a result feature tile is available.
                You can also iterate over this Request object instead of providing the callback.
                on_sourcedata_result: The callback function to be callend when a result source-data tile
                is available. You can also iterate over this Request object instead of porviding the callback.
                source_id: Optional catalog source assertion.

            Note: The provided tile ids are processed in the given order.
        )pbdoc")
        .def("__iter__", [](PyRequest &r) { return &r; }, R"pbdoc(
            Return the iterator object (self).
        )pbdoc")
        .def("__next__", &PyRequest::next, R"pbdoc(
            Get the next available result.

            This function blocks until a result is available. If the Request is exhausted,
            this function will raise a StopIteration exception.

            Returns:
                The next available result.
        )pbdoc", py::call_guard<py::gil_scoped_release>())
        .def("wait", &PyRequest::wait, R"pbdoc(
            Wait for the request to be done.

            This function blocks until all results have been processed.
        )pbdoc", py::call_guard<py::gil_scoped_release>());

    py::class_<PyFilterRequest, std::shared_ptr<PyFilterRequest>>(m, "FilterRequest", R"pbdoc(
        Client request for server-side filter evaluation evaluation.

        FilterRequest posts a simplified REST /filter request. Results are
        TileSubsetLayer objects and can be consumed with a callback or by
        iterating over the request object returned by Client.filter().
    )pbdoc")
        .def(
            py::init(
                [](const std::string& mapId,
                   const std::string& layerId,
                   std::vector<TileId> tiles,
                   std::string filterId,
                   uint64_t generation,
                   std::vector<FeatureLayerFilterChannel> channels,
                   py::dict const& bindings,
                   std::vector<FeatureLayerFilterRoot> exactRoots,
                   std::function<void(TileSubsetLayer::Ptr)> onResult,
                   std::function<void(py::object)> onStatus,
                   std::optional<std::string> sourceId)
                {
                    if (channels.empty()) {
                        throw py::value_error(
                            "channels must not be empty");
                    }
                    FeatureLayerFilterRequest filter{
                        .filterId_ = std::move(filterId),
                        .generation_ = generation,
                        .channels_ = std::move(channels),
                        .bindings_ =
                            filter_bindings_from_python(bindings),
                    };

                    auto req = std::make_shared<PyFilterRequest>(
                        mapId,
                        layerId,
                        std::move(tiles),
                        std::move(filter));
                    req->sourceId_ = std::move(sourceId);
                    req->exactRoots_ =
                        std::move(exactRoots);
                    req->onFilterResult(std::move(onResult));
                    if (onStatus) {
                        req->onStatus([callback = std::move(onStatus)](nlohmann::json const& status) {
                            callback(json_to_py_value(status));
                        });
                    }
                    return req;
                }),
            py::arg("map_id"),
            py::arg("layer_id"),
            py::arg("tiles"),
            py::arg("filter_id"),
            py::arg("generation"),
            py::arg("channels"),
            py::arg("bindings") = py::dict(),
            py::arg("exact_roots") =
                std::vector<
                    FeatureLayerFilterRoot>{},
            py::arg("on_result") = py::none(),
            py::arg("on_status") = py::none(),
            py::arg("source_id") = py::none(),
            py::call_guard<py::gil_scoped_acquire>(),
            R"pbdoc(
            Construct a FilterRequest.

            Args:
                map_id: The source map id to filter.
                layer_id: The source feature layer id to filter.
                tiles: Source ndslive.math.PackedTileId values to filter.
                filter_id: Stable identity of this filter subscription.
                generation: Definition/root revision for stale-result rejection; pending coverage changes retain it.
                channels: Ordered FilterChannel instances; channels are never conflated.
                bindings: Scalar SIMFIL constants/overlay fields shared by all channels.
                exact_roots: Optional indexed roots for relation traversal.
                on_result: Optional callback for each TileSubsetLayer.
                on_status: Optional callback for progress/status dictionaries.
                source_id: Optional catalog source assertion.
        )pbdoc")
        .def("__iter__", [](PyFilterRequest &r) { return &r; }, R"pbdoc(
            Return the iterator object (self).
        )pbdoc")
        .def("__next__", &PyFilterRequest::next, R"pbdoc(
            Get the next available subset layer.
        )pbdoc", py::call_guard<py::gil_scoped_release>())
        .def("wait", &PyFilterRequest::wait, R"pbdoc(
            Wait for the filter request to be done.
        )pbdoc", py::call_guard<py::gil_scoped_release>());

    py::class_<HttpClient, std::shared_ptr<HttpClient>>(m, "Client", R"pbdoc(
        Synchronous HTTP client for a running mapget service.

        The client fetches `/sources` once during construction, keeps the
        resulting layer metadata for request decoding, and can submit tile and
        server-side filter requests.
    )pbdoc")
        .def(py::init<const std::string&, uint16_t, AuthHeaders, bool>(),
             R"pbdoc(
                Connect to a running mapget HTTP service.

                The constructor immediately calls `/sources` and caches the
                ready datasource metadata for the lifetime of this object.

                Args:
                    host: Hostname or IP address of the mapget HTTP service.
                    port: TCP port of the mapget HTTP service.
                    headers: Optional HTTP headers sent with `/sources`,
                        `/tiles`, and `/filter` requests. Use this for auth.
                    enable_compression: Request gzip responses unless the
                        headers already contain an `Accept-Encoding` override.
            )pbdoc",
             py::arg("host"),
             py::arg("port"),
             py::arg("headers") = AuthHeaders{},
             py::arg("enable_compression") = true)
        .def("sources", [](HttpClient& self){
                auto jsonArray = nlohmann::json::array();
                for (auto const& dsInfo : self.sources())
                    jsonArray.push_back(dsInfo.toJson());
                return json_to_py_value(jsonArray);
            },
             R"pbdoc(
                Get the sources as they were retrieved when the Client was instantiated.
            )pbdoc")
        .def(
            "request",
            [](HttpClient& self, std::shared_ptr<PyRequest> request) {
                self.request(request);
                return std::move(request);
            },
            R"pbdoc(
                Post a Request for a number of tiles from a particular map layer.
                Returns the request object which was put in.
            )pbdoc",
            py::arg("request"))
        .def(
            "filter",
            [](HttpClient& self, std::shared_ptr<PyFilterRequest> request) {
                self.filter(request);
                return std::move(request);
            },
            R"pbdoc(
                Post a FilterRequest to the REST /filter endpoint.
                Returns the request object which was put in.
            )pbdoc",
            py::arg("request"));
}
