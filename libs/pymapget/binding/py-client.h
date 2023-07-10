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
        throw std::runtime_error("Invalid JSON value type");
    }
    }
}
}

namespace mapget
{

namespace py = pybind11;

class PyRequest : public Request {
public:
    using Request::Request;

    void notifyResult(TileFeatureLayer::Ptr result) override {
        std::unique_lock lock(bufferMutex_);
        buffer_.push(result);
        bufferSignal_.notify_one();  // Signal that a new result is available
        Request::notifyResult(result);
    }

    TileFeatureLayer::Ptr next() {
        std::unique_lock lock(bufferMutex_);
        bufferSignal_.wait(lock, [this](){ return !buffer_.empty() || isDone(); });
        if (buffer_.empty()) {
            throw py::stop_iteration();
        } else {
            auto result = buffer_.front();
            buffer_.pop();
            return result;
        }
    }

private:
    std::queue<TileFeatureLayer::Ptr> buffer_;
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
                   std::vector<uint64_t> tiles,
                   std::function<void(TileFeatureLayer::Ptr)> onResult)
                {
                    return std::make_shared<PyRequest>(
                        mapId,
                        layerId,
                        std::vector<TileId>(tiles.begin(), tiles.end()),
                        std::move(onResult));
                }),
            py::arg("map_id"),
            py::arg("layer_id"),
            py::arg("tiles"),
            py::arg("on_result") = py::none(),
            py::call_guard<py::gil_scoped_acquire>(),
            R"pbdoc(
            Construct a Request.

            Args:
                map_id: The map id for which this request is dedicated.
                layer_id: The map layer id for which this request is dedicated.
                tiles: The map tile ids for which this request is dedicated.
                on_result: The callback function to be called when a result tile is available.
                You can also iterate over this Request object instead of providing on_result.

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

    py::class_<HttpClient, std::shared_ptr<HttpClient>>(m, "Client")
        .def(py::init<const std::string&, uint16_t>(),
             R"pbdoc(
                Connect to a running mapget HTTP service. Immediately calls the /sources
                endpoint, and caches the result for the lifetime of this object.
            )pbdoc",
             py::arg("host"), py::arg("port"))
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
            py::arg("request"));
}
