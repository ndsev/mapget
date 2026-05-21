#include "http-service-impl.h"
#include "tiles-request-json.h"
#include "tiles-stream-encoding.h"

#include "mapget/log.h"
#include "mapget/model/featurelayer-search.h"

#include <fmt/format.h>
#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
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

/**
 * Incremental gzip compressor used by the HTTP streaming endpoint.
 *
 * The HTTP path cannot use the one-shot gzip helper because tile frames are
 * produced asynchronously and may need to be flushed in several response chunks.
 */
class GzipCompressor
{
public:
    /** Initialize a zlib stream configured to emit gzip framing. */
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

    /** Release the zlib stream state; callers must have already emitted any footer via finish(). */
    ~GzipCompressor() { deflateEnd(&strm_); }

    GzipCompressor(GzipCompressor const&) = delete;
    GzipCompressor(GzipCompressor&&) = delete;

    /** Compress the next response bytes, preserving stream state across calls. */
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

    /** Finalize the gzip stream and return the gzip footer/trailing bytes. */
    std::string finish() { return compress(nullptr, 0, Z_FINISH); }

private:
    z_stream strm_{};
};

}  // namespace

/**
 * Per-request state for the HTTP `/tiles` streaming response.
 *
 * It bridges backend tile callbacks into one Drogon async response stream while
 * preserving binary/JSONL encoding, optional gzip compression, and abort cleanup.
 */
struct HttpService::Impl::TilesStreamState : std::enable_shared_from_this<TilesStreamState>
{
    static constexpr auto binaryMimeType = "application/binary";
    static constexpr auto jsonlMimeType = "application/jsonl";
    static constexpr auto anyMimeType = "*/*";

    /** Construct one streaming response state object bound to the Drogon event loop. */
    explicit TilesStreamState(Impl const& impl, trantor::EventLoop* loop) : impl_(impl), loop_(loop)
    {
        static std::atomic_uint64_t nextRequestId;
        requestId_ = nextRequestId++;
        writer_ = std::make_unique<TileLayerStream::Writer>(
            [this](auto&& msg, auto&& /*msgType*/) { appendOutgoingUnlocked(msg); }, stringOffsets_);
    }

    /** Attach Drogon's stream handle once the async response starts and trigger draining. */
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

    /** Convert one parsed request JSON object into a backend tile or search request. */
    void parseRequestFromJson(nlohmann::json const& requestJson)
    {
        auto parsed = detail::parseLayerTilesRequestJson(requestJson);
        auto searchRequest = std::move(parsed.searchRequest);
        if (searchRequest) {
            auto request = std::make_shared<FeatureLayerSearchTilesRequest>(
                std::move(parsed.mapId),
                std::move(parsed.layerId),
                detail::collectSearchTileIds(parsed),
                std::move(*searchRequest),
                std::move(parsed.priorityTileIds));
            searchRequests_.push_back(std::move(request));
            return;
        }

        LayerTilesRequest::Ptr request;
        if (parsed.usesStageBuckets) {
            request = std::make_shared<LayerTilesRequest>(
                std::move(parsed.mapId),
                std::move(parsed.layerId),
                std::move(parsed.tileIdsByNextStage),
                std::move(parsed.priorityTileIds));
        } else {
            auto tileIds = parsed.tileIdsByNextStage.empty()
                ? std::vector<TileId>{}
                : std::move(parsed.tileIdsByNextStage.front());
            request = std::make_shared<LayerTilesRequest>(
                std::move(parsed.mapId),
                std::move(parsed.layerId),
                std::move(tileIds),
                std::move(parsed.priorityTileIds));
        }
        requests_.push_back(std::move(request));
    }

    /** Interpret the Accept header and choose the stream payload format. */
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

    /** Enable incremental gzip compression for all subsequently appended response bytes. */
    void enableGzip() { compressor_ = std::make_unique<GzipCompressor>(); }

    /** Abort backend work and close the response stream after client disconnect/send failure. */
    void onAborted()
    {
        if (aborted_.exchange(true))
            return;
        for (auto const& req : requests_) {
            if (!req->isDone()) {
                impl_.self_.abort(req);
            }
        }
        for (auto const& req : searchRequests_) {
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

    /** Serialize one backend tile/search/source layer into the pending HTTP response buffer. */
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

    /** Serialize one search status update into the pending HTTP response buffer. */
    void addStatus(nlohmann::json const& status)
    {
        {
            std::lock_guard lock(mutex_);
            if (aborted_)
                return;
            auto dumped = status.dump(-1, ' ', false, nlohmann::json::error_handler_t::ignore);
            if (responseType_ == binaryMimeType) {
                writer_->sendStatus(std::move(dumped));
            } else {
                appendOutgoingUnlocked(dumped);
                appendOutgoingUnlocked("\n");
            }
        }
        scheduleDrain();
    }

    /** Mark backend completion and emit binary end-of-stream once all requests are done. */
    void onRequestDone()
    {
        {
            std::lock_guard lock(mutex_);
            if (aborted_)
                return;

            bool allDoneNow =
                std::all_of(requests_.begin(), requests_.end(), [](auto const& r) { return r->isDone(); }) &&
                std::all_of(searchRequests_.begin(), searchRequests_.end(), [](auto const& r) { return r->isDone(); });

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

    /** Schedule one event-loop drain pass unless one is already queued. */
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

    /** Send pending response bytes on Drogon's event loop and close once completion is flushed. */
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
                        // Completion must wait until zlib has emitted the gzip footer.
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

    /** Append payload bytes to the response buffer, compressing when gzip is enabled. */
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
    std::vector<FeatureLayerSearchTilesRequest::Ptr> searchRequests_;
    TileLayerStream::StringPoolOffsetMap stringOffsets_;

    std::unique_ptr<GzipCompressor> compressor_;
    bool compressionFinished_ = false;
    bool endOfStreamSent_ = false;
    bool allDone_ = false;

    std::atomic_bool aborted_{false};
    std::atomic_bool drainScheduled_{false};
    std::atomic_bool responseEnded_{false};
};

/** Handle the HTTP `/tiles` endpoint and stream tile/search results until backend completion. */
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
            auto effectiveRequestJson = requestJson;
            detail::inheritSearchFields(effectiveRequestJson, j);
            state->parseRequestFromJson(effectiveRequestJson);
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

    if (auto offsetsIt = j.find("stringPoolOffsets"); offsetsIt != j.end()) {
        try {
            state->stringOffsets_ = detail::parseStringPoolOffsetsJson(*offsetsIt);
        }
        catch (const std::exception& e) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k400BadRequest);
            resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
            resp->setBody(std::string("Invalid stringPoolOffsets: ") + e.what());
            callback(resp);
            return;
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

    const bool gzip = detail::containsGzip(acceptEncoding);
    if (gzip) {
        state->enableGzip();
    }

    for (size_t i = 0; i < state->requests_.size(); ++i) {
        auto& request = state->requests_[i];
        request->onFeatureLayer([state](TileFeatureLayer::Ptr layer) {
            state->addResult(std::move(layer));
        });
        request->onSourceDataLayer([state](TileSourceDataLayer::Ptr layer) {
            state->addResult(std::move(layer));
        });
        request->onDone_ = [state](RequestStatus) { state->onRequestDone(); };
    }
    for (auto& request : state->searchRequests_) {
        request->onSearchResult([state](TileSearchResultLayer::Ptr layer) {
            state->addResult(std::move(layer));
        });
        request->onStatus([state](nlohmann::json const& status) {
            state->addStatus(status);
        });
        request->onDone_ = [state](RequestStatus) { state->onRequestDone(); };
    }

    const auto tileRequestsAccepted = self_.request(state->requests_, clientHeaders);
    const auto searchRequestsAccepted =
        tileRequestsAccepted ? self_.request(state->searchRequests_, clientHeaders) : state->searchRequests_.empty();
    const auto canProcess = tileRequestsAccepted && searchRequestsAccepted;
    if (!canProcess) {
        for (auto const& r : state->requests_) {
            if (!r->isDone()) {
                self_.abort(r);
            }
        }
        for (auto const& r : state->searchRequests_) {
            if (!r->isDone()) {
                self_.abort(r);
            }
        }

        std::vector<std::underlying_type_t<RequestStatus>> requestStatuses{};
        bool anyUnauthorized = false;
        for (auto const& r : state->requests_) {
            auto status = r->getStatus();
            requestStatuses.emplace_back(static_cast<std::underlying_type_t<RequestStatus>>(status));
            anyUnauthorized |= (status == RequestStatus::Unauthorized);
        }
        for (auto const& r : state->searchRequests_) {
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
