#include "http-service-impl.h"
#include "tiles-request-json.h"

#include "mapget/log.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

#include <zlib.h>

namespace mapget
{
namespace
{

class GzipCompressor
{
public:
    GzipCompressor()
    {
        strm_.zalloc = Z_NULL;
        strm_.zfree = Z_NULL;
        strm_.opaque = Z_NULL;
        // 16+MAX_WBITS enables gzip format (not just deflate)
        int ret = deflateInit2(
            &strm_, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        if (ret != Z_OK) {
            throw std::runtime_error("Failed to initialize gzip compressor");
        }
    }

    ~GzipCompressor() { deflateEnd(&strm_); }

    GzipCompressor(GzipCompressor const&) = delete;
    GzipCompressor(GzipCompressor&&) = delete;

    std::string compress(const char* data, size_t size, int flush_mode = Z_NO_FLUSH)
    {
        std::string result;
        if (size == 0 && flush_mode == Z_NO_FLUSH) {
            return result;
        }

        strm_.avail_in = static_cast<uInt>(size);
        strm_.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data));

        char outbuf[8192];
        do {
            strm_.avail_out = sizeof(outbuf);
            strm_.next_out = reinterpret_cast<Bytef*>(outbuf);

            int ret = deflate(&strm_, flush_mode);
            if (ret == Z_STREAM_ERROR) {
                throw std::runtime_error("Gzip compression failed");
            }

            size_t have = sizeof(outbuf) - strm_.avail_out;
            result.append(outbuf, have);
        } while (strm_.avail_out == 0);

        return result;
    }

    std::string finish() { return compress(nullptr, 0, Z_FINISH); }

private:
    z_stream strm_{};
};

[[nodiscard]] bool containsGzip(std::string_view acceptEncoding)
{
    return !acceptEncoding.empty() && acceptEncoding.find("gzip") != std::string_view::npos;
}

}  // namespace

struct HttpService::Impl::TilesStreamState : std::enable_shared_from_this<TilesStreamState>
{
    static constexpr auto binaryMimeType = "application/binary";
    static constexpr auto jsonlMimeType = "application/jsonl";
    static constexpr auto anyMimeType = "*/*";

    explicit TilesStreamState(Impl const& impl, trantor::EventLoop* loop) : impl_(impl), loop_(loop)
    {
        static std::atomic_uint64_t nextRequestId;
        requestId_ = nextRequestId++;
        writer_ = std::make_unique<TileLayerStream::Writer>(
            [this](auto&& msg, auto&& /*msgType*/) { appendOutgoingUnlocked(msg); }, stringOffsets_);
    }

    void attachStream(drogon::ResponseStreamPtr stream)
    {
        {
            std::lock_guard lock(mutex_);
            if (aborted_ || responseEnded_) {
                if (stream)
                    stream->close();
                return;
            }
            stream_ = std::move(stream);
        }
        scheduleDrain();
    }

    void parseRequestFromJson(nlohmann::json const& requestJson)
    {
        auto parsed = detail::parseLayerTilesRequestJson(requestJson);
        if (parsed.usesStageBuckets) {
            requests_.push_back(std::make_shared<LayerTilesRequest>(
                std::move(parsed.mapId),
                std::move(parsed.layerId),
                std::move(parsed.tileIdsByNextStage)));
        } else {
            auto tileIds = parsed.tileIdsByNextStage.empty()
                ? std::vector<TileId>{}
                : std::move(parsed.tileIdsByNextStage.front());
            requests_.push_back(std::make_shared<LayerTilesRequest>(
                std::move(parsed.mapId),
                std::move(parsed.layerId),
                std::move(tileIds)));
        }
    }

    [[nodiscard]] bool setResponseTypeFromAccept(std::string_view acceptHeader, std::string& error)
    {
        responseType_ = std::string(acceptHeader);
        if (responseType_.empty())
            responseType_ = anyMimeType;
        if (responseType_ == anyMimeType)
            responseType_ = binaryMimeType;

        if (responseType_ == binaryMimeType) {
            trimResponseType_ = HttpService::Impl::ResponseType::Binary;
            return true;
        }
        if (responseType_ == jsonlMimeType) {
            trimResponseType_ = HttpService::Impl::ResponseType::Json;
            return true;
        }

        error = "Unknown Accept header value: " + responseType_;
        return false;
    }

    void enableGzip() { compressor_ = std::make_unique<GzipCompressor>(); }

    void onAborted()
    {
        if (aborted_.exchange(true))
            return;
        for (auto const& req : requests_) {
            if (!req->isDone()) {
                impl_.self_.abort(req);
            }
        }
        drogon::ResponseStreamPtr stream;
        {
            std::lock_guard lock(mutex_);
            if (responseEnded_.exchange(true))
                return;
            stream = std::move(stream_);
        }
        if (stream)
            stream->close();
    }

    void addResult(TileLayer::Ptr const& result)
    {
        {
            std::lock_guard lock(mutex_);
            if (aborted_)
                return;

            log().debug("Response ready: {}", MapTileKey(*result).toString());
            if (responseType_ == binaryMimeType) {
                writer_->write(result);
            } else {
                auto dumped = result->toJson().dump(-1, ' ', false, nlohmann::json::error_handler_t::ignore);
                appendOutgoingUnlocked(dumped);
                appendOutgoingUnlocked("\n");
            }
        }
        scheduleDrain();
    }

    void onRequestDone()
    {
        {
            std::lock_guard lock(mutex_);
            if (aborted_)
                return;

            bool allDoneNow =
                std::all_of(requests_.begin(), requests_.end(), [](auto const& r) { return r->isDone(); });

            if (allDoneNow && !allDone_) {
                allDone_ = true;
                if (responseType_ == binaryMimeType && !endOfStreamSent_) {
                    writer_->sendEndOfStream();
                    endOfStreamSent_ = true;
                }
            }
        }
        scheduleDrain();
    }

    void scheduleDrain()
    {
        if (aborted_ || responseEnded_)
            return;
        if (drainScheduled_.exchange(true))
            return;

        auto weak = weak_from_this();
        loop_->queueInLoop([weak = std::move(weak)]() mutable {
            if (auto self = weak.lock()) {
                self->drainOnLoop();
            }
        });
    }

    void drainOnLoop()
    {
        drainScheduled_ = false;
        if (aborted_ || responseEnded_)
            return;

        constexpr size_t maxChunk = 64 * 1024;

        for (;;) {
            std::string chunk;
            bool done = false;
            bool needAbort = false;
            bool scheduleAgain = false;
            drogon::ResponseStreamPtr streamToClose;
            {
                std::lock_guard lock(mutex_);
                if (!stream_)
                    return;

                if (!pending_.empty()) {
                    size_t n = std::min(pending_.size(), maxChunk);
                    chunk.assign(pending_.data(), n);
                    pending_.erase(0, n);
                } else {
                    if (allDone_ && compressor_ && !compressionFinished_) {
                        pending_.append(compressor_->finish());
                        compressionFinished_ = true;
                        continue;
                    }
                    done = allDone_;
                }

                if (!chunk.empty()) {
                    if (!stream_->send(chunk)) {
                        needAbort = true;
                    } else if (!pending_.empty() || allDone_) {
                        scheduleAgain = true;
                    }
                } else if (done) {
                    responseEnded_ = true;
                    streamToClose = std::move(stream_);
                }
            }

            if (needAbort) {
                onAborted();
                return;
            }

            if (done) {
                if (streamToClose)
                    streamToClose->close();
                impl_.tryMemoryTrim(trimResponseType_);
                return;
            }
            if (scheduleAgain)
                scheduleDrain();
            return;
        }
    }

    void appendOutgoingUnlocked(std::string_view bytes)
    {
        if (bytes.empty())
            return;

        if (compressor_) {
            pending_.append(compressor_->compress(bytes.data(), bytes.size()));
        } else {
            pending_.append(bytes);
        }
    }

    Impl const& impl_;
    trantor::EventLoop* loop_;

    std::mutex mutex_;
    uint64_t requestId_ = 0;

    std::string responseType_;
    HttpService::Impl::ResponseType trimResponseType_ = HttpService::Impl::ResponseType::Binary;

    std::string pending_;
    drogon::ResponseStreamPtr stream_;
    std::unique_ptr<TileLayerStream::Writer> writer_;
    std::vector<LayerTilesRequest::Ptr> requests_;
    TileLayerStream::StringPoolOffsetMap stringOffsets_;

    std::unique_ptr<GzipCompressor> compressor_;
    bool compressionFinished_ = false;
    bool endOfStreamSent_ = false;
    bool allDone_ = false;

    std::atomic_bool aborted_{false};
    std::atomic_bool drainScheduled_{false};
    std::atomic_bool responseEnded_{false};
};

void HttpService::Impl::handleTilesRequest(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) const
{
    auto state = std::make_shared<TilesStreamState>(*this, drogon::app().getLoop());

    const std::string accept = req->getHeader("accept");
    const std::string acceptEncoding = req->getHeader("accept-encoding");
    auto clientHeaders = detail::authHeadersFromRequest(req);

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(std::string(req->body()));
    }
    catch (const std::exception& e) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k400BadRequest);
        resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        resp->setBody(std::string("Invalid JSON: ") + e.what());
        callback(resp);
        return;
    }

    auto requestsIt = j.find("requests");
    if (requestsIt == j.end() || !requestsIt->is_array()) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k400BadRequest);
        resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        resp->setBody("Missing or invalid 'requests' array");
        callback(resp);
        return;
    }

    log().info("Processing tiles request {}", state->requestId_);
    try {
        for (auto& requestJson : *requestsIt) {
            state->parseRequestFromJson(requestJson);
        }
    }
    catch (const std::exception& e) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k400BadRequest);
        resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        resp->setBody(std::string("Invalid request JSON: ") + e.what());
        callback(resp);
        return;
    }

    if (j.contains("stringPoolOffsets")) {
        for (auto& item : j["stringPoolOffsets"].items()) {
            state->stringOffsets_[item.key()] = item.value().get<simfil::StringId>();
        }
    }

    std::string acceptError;
    if (!state->setResponseTypeFromAccept(accept, acceptError)) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k400BadRequest);
        resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        resp->setBody(std::move(acceptError));
        callback(resp);
        return;
    }

    const bool gzip = containsGzip(acceptEncoding);
    if (gzip) {
        state->enableGzip();
    }

    for (auto& request : state->requests_) {
        request->onFeatureLayer([state](auto&& layer) { state->addResult(layer); });
        request->onSourceDataLayer([state](auto&& layer) { state->addResult(layer); });
        request->onDone_ = [state](RequestStatus) { state->onRequestDone(); };
    }

    const auto canProcess = self_.request(state->requests_, clientHeaders);
    if (!canProcess) {
        std::vector<std::underlying_type_t<RequestStatus>> requestStatuses{};
        bool anyUnauthorized = false;
        for (auto const& r : state->requests_) {
            auto status = r->getStatus();
            requestStatuses.emplace_back(static_cast<std::underlying_type_t<RequestStatus>>(status));
            anyUnauthorized |= (status == RequestStatus::Unauthorized);
        }

        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(anyUnauthorized ? drogon::k403Forbidden : drogon::k400BadRequest);
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        resp->setBody(nlohmann::json::object({{"status", requestStatuses}}).dump());
        callback(resp);
        return;
    }

    auto resp = drogon::HttpResponse::newAsyncStreamResponse(
        [state](drogon::ResponseStreamPtr stream) { state->attachStream(std::move(stream)); },
        true);
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeString(state->responseType_);
    if (gzip) {
        resp->addHeader("Content-Encoding", "gzip");
    }
    callback(resp);
}

}  // namespace mapget
