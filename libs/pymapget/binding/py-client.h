#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

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

class PySearchRequest : public FeatureLayerSearchTilesRequest
{
public:
    using FeatureLayerSearchTilesRequest::FeatureLayerSearchTilesRequest;

    void notifyResult(TileSearchResultLayer::Ptr result) override {
        std::unique_lock lock(bufferMutex_);
        buffer_.push(result);
        bufferSignal_.notify_one();
        FeatureLayerSearchTilesRequest::notifyResult(std::move(result));
    }

    TileSearchResultLayer::Ptr next() {
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
    std::queue<TileSearchResultLayer::Ptr> buffer_;
    std::mutex bufferMutex_;
    std::condition_variable bufferSignal_;
};
}

void bindHttpClient(py::module_& m)
{
    using namespace mapget;

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
                   std::function<void(TileSourceDataLayer::Ptr)> onSourceDataResult)
                {
                    auto req = std::make_shared<PyRequest>(
                        mapId,
                        layerId,
                        std::move(tiles));
                    req->onFeatureLayer(std::move(onFeatureResult));
                    req->onSourceDataLayer(std::move(onSourceDataResult));
                    return req;
                }),
            py::arg("map_id"),
            py::arg("layer_id"),
            py::arg("tiles"),
            py::arg("on_feature_result") = py::none(),
            py::arg("on_sourcedata_result") = py::none(),
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

    py::class_<PySearchRequest, std::shared_ptr<PySearchRequest>>(m, "SearchRequest", R"pbdoc(
        Client request for server-side search-as-map evaluation.

        SearchRequest posts a simplified REST /search request. Results are
        TileSearchResultLayer objects and can be consumed with a callback or by
        iterating over the request object returned by Client.search().
    )pbdoc")
        .def(
            py::init(
                [](const std::string& mapId,
                   const std::string& layerId,
                   std::vector<TileId> tiles,
                   const std::string& query,
                   const std::string& scope,
                   bool rewrite,
                   std::vector<std::string> withFields,
                   std::vector<std::string> featureTypes,
                   std::function<void(TileSearchResultLayer::Ptr)> onResult,
                   std::function<void(py::object)> onStatus)
                {
                    FeatureLayerSearchRequest search;
                    search.query_ = query;
                    if (scope == "feature") {
                        search.scope_ = FeatureLayerSearchScope::Feature;
                    }
                    else if (scope == "attribute") {
                        search.scope_ = FeatureLayerSearchScope::Attribute;
                    }
                    else if (scope == "auto") {
                        search.scope_ = FeatureLayerSearchScope::Auto;
                    }
                    else {
                        throw py::value_error("scope must be 'feature', 'attribute' or 'auto'");
                    }
                    search.rewriteQuery_ = rewrite || search.scope_ == FeatureLayerSearchScope::Auto;
                    search.withFields_ = std::move(withFields);
                    search.featureTypes_ = std::move(featureTypes);

                    auto req = std::make_shared<PySearchRequest>(
                        mapId,
                        layerId,
                        std::move(tiles),
                        std::move(search));
                    req->onSearchResult(std::move(onResult));
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
            py::arg("query"),
            py::arg("scope") = "feature",
            py::arg("rewrite") = false,
            py::arg("with_fields") = std::vector<std::string>{},
            py::arg("feature_types") = std::vector<std::string>{},
            py::arg("on_result") = py::none(),
            py::arg("on_status") = py::none(),
            py::call_guard<py::gil_scoped_acquire>(),
            R"pbdoc(
            Construct a SearchRequest.

            Args:
                map_id: The source map id to search.
                layer_id: The source feature layer id to search.
                tiles: Source ndslive.math.PackedTileId values to search.
                query: SIMFIL predicate.
                scope: "feature", "attribute" or "auto".
                rewrite: Normalize the query through the feature-model schema before evaluation. Auto scope implies rewrite.
                with_fields: SIMFIL expressions stored in each result's values array.
                feature_types: Optional feature type names to search; omitted means all feature types.
                on_result: Optional callback for each TileSearchResultLayer.
                on_status: Optional callback for progress/status dictionaries.
        )pbdoc")
        .def("__iter__", [](PySearchRequest &r) { return &r; }, R"pbdoc(
            Return the iterator object (self).
        )pbdoc")
        .def("__next__", &PySearchRequest::next, R"pbdoc(
            Get the next available search-result layer.
        )pbdoc", py::call_guard<py::gil_scoped_release>())
        .def("wait", &PySearchRequest::wait, R"pbdoc(
            Wait for the search request to be done.
        )pbdoc", py::call_guard<py::gil_scoped_release>());

    py::class_<HttpClient, std::shared_ptr<HttpClient>>(m, "Client", R"pbdoc(
        Synchronous HTTP client for a running mapget service.

        The client fetches `/sources` once during construction, keeps the
        resulting layer metadata for request decoding, and can submit tile and
        server-side search requests.
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
                        `/tiles`, and `/search` requests. Use this for auth.
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
            "search",
            [](HttpClient& self, std::shared_ptr<PySearchRequest> request) {
                self.search(request);
                return std::move(request);
            },
            R"pbdoc(
                Post a SearchRequest to the REST /search endpoint.
                Returns the request object which was put in.
            )pbdoc",
            py::arg("request"));
}
