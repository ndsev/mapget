#pragma once

#include "http-service.h"

#include "httplib.h"

namespace mapget
{

struct HttpService::Impl
{
    HttpService& self_;

    explicit Impl(HttpService& self) : self_(self) {}

    // Use a shared buffer for the responses and a mutex for thread safety
    struct TileLayerRequestState
    {
        static constexpr auto binaryMimeType = "application/binary";
        static constexpr auto jsonlMimeType = "text/jsonl";
        static constexpr auto anyMimeType = "*/*";

        std::mutex mutex_;
        std::condition_variable resultEvent_;

        std::stringstream buffer_;
        std::string responseType_;
        std::vector<Request::Ptr> requests_;
        TileLayerStream::FieldOffsetMap fieldsOffsets_;

        void addRequestFromJson(nlohmann::json const& requestJson)
        {
            std::string mapId = requestJson["mapId"];
            std::string layerId = requestJson["layerId"];
            std::vector<TileId> tileIds;
            tileIds.reserve(requestJson["tileIds"].size());
            for (auto const& tid : requestJson["tileIds"].get<std::vector<uint64_t>>())
                tileIds.emplace_back(tid);
            requests_.push_back(
                std::make_shared<Request>(mapId, layerId, std::move(tileIds), nullptr));
        }

        void setResponseType(std::string const& s)
        {
            responseType_ = s;
            if (responseType_ == TileLayerRequestState::binaryMimeType)
                return;
            if (responseType_ == TileLayerRequestState::jsonlMimeType)
                return;
            if (responseType_ == TileLayerRequestState::anyMimeType) {
                responseType_ = binaryMimeType;
                return;
            }
            throw std::runtime_error(stx::format("Unknown Accept-Header value {}", responseType_));
        }

        void addResult(TileFeatureLayer::Ptr const& result)
        {
            std::unique_lock lock(mutex_);
            std::cout << "Response ready: " << MapTileKey(*result).toString() << std::endl;
            if (responseType_ == binaryMimeType) {
                // Binary response
                TileLayerStream::Writer writer{
                    [&, this](auto&& msg, auto&& msgType) { buffer_ << msg; },
                    fieldsOffsets_};
                writer.write(result);
            }
            else {
                buffer_ << nlohmann::to_string(result->toGeoJson())+"\n";
            }
            resultEvent_.notify_one();
        }
    };

    void handleTilesRequest(const httplib::Request& req, httplib::Response& res)
    {
        // Parse the JSON request
        nlohmann::json j = nlohmann::json::parse(req.body);
        auto requestsJson = j["requests"];
        auto state = std::make_shared<TileLayerRequestState>();
        for (auto& requestJson : requestsJson) {
            state->addRequestFromJson(requestJson);
        }

        // Parse maxKnownFieldIds
        if (j.contains("maxKnownFieldIds")) {
            for (auto& item : j["maxKnownFieldIds"].items()) {
                state->fieldsOffsets_[item.key()] = item.value().get<simfil::FieldId>();
            }
        }

        // Determine response type
        state->setResponseType(req.get_header_value("Accept"));

        // Process requests
        for (auto& request : state->requests_) {
            request->onResult_ = [state](auto&& tileFeatureLayer)
            {
                state->addResult(tileFeatureLayer);
            };
            self_.request(request);
        }

        // Set up the streaming response
        res.set_content_provider(
            state->responseType_,
            [state](size_t offset, httplib::DataSink& sink)
            {
                std::unique_lock lock(state->mutex_);

                // Wait until there is data to be read
                std::string strBuf;
                bool allDone = false;
                state->resultEvent_.wait(
                    lock,
                    [&]
                    {
                        strBuf = state->buffer_.str();
                        allDone = std::all_of(
                            state->requests_.begin(),
                            state->requests_.end(),
                            [](const auto& r) { return r->isDone(); });
                        return !strBuf.empty() || allDone;
                    });

                if (!strBuf.empty()) {
                    std::cout << "Streaming bytes: " << strBuf.size() << std::endl;
                    sink.write(strBuf.data(), strBuf.size());
                    state->buffer_.str("");  // Clear buffer after reading
                }

                // Call sink.done() when all requests are done
                if (allDone)
                    sink.done();

                return true;
            },
            // Cleanup callback to abort the requests
            [state, this](bool success)
            {
                if (!success) {
                    for (auto& request : state->requests_) {
                        self_.abort(request);
                    }
                }
            });
    }

    void handleSourcesRequest(const httplib::Request&, httplib::Response& res)
    {
        nlohmann::json sourcesInfo;
        for (auto& source : self_.info()) {
            sourcesInfo.push_back(source.toJson());
        }
        res.set_content(sourcesInfo.dump(), "application/json");
    }

    void handleStatusRequest(const httplib::Request&, httplib::Response& res)
    {
        std::ostringstream oss;
        oss << "<html><body>";
        oss << "<h1>Status Information</h1>";
        oss << "<h2>Data Sources: " << self_.info().size() << "</h2>";
        oss << "</body></html>";
        res.set_content(oss.str(), "text/html");
    }
};

HttpService::HttpService(Cache::Ptr cache) : Service(std::move(cache)), impl_(std::make_unique<Impl>(*this))
{}

HttpService::~HttpService() = default;

void HttpService::setup(httplib::Server& server)
{
    server.Post(
        "/tiles",
        [&](const httplib::Request& req, httplib::Response& res)
        {
            impl_->handleTilesRequest(req, res);
        });

    server.Get(
        "/sources",
        [this](const httplib::Request& req, httplib::Response& res)
        {
            impl_->handleSourcesRequest(req, res);
        });

    server.Get(
        "/status",
        [this](const httplib::Request& req, httplib::Response& res)
        {
            impl_->handleStatusRequest(req, res);
        });
}

}  // namespace mapget
