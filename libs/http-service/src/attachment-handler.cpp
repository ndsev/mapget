#include "http-service-impl.h"

#include <charconv>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <drogon/HttpResponse.h>

#include "picosha2.h"

namespace mapget
{
namespace
{

[[nodiscard]] int32_t parseTileId(
    std::string_view text)
{
    if (text.empty()) {
        throw std::runtime_error(
            "Missing tileId.");
    }
    int64_t value = 0;
    auto const [end, error] =
        std::from_chars(
            text.data(),
            text.data() + text.size(),
            value);
    if (error != std::errc{} ||
        end != text.data() + text.size() ||
        value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max())
    {
        throw std::runtime_error(
            "tileId must be a signed 32-bit integer.");
    }
    return static_cast<int32_t>(value);
}

[[nodiscard]] drogon::HttpResponsePtr
plainResponse(
    drogon::HttpStatusCode status,
    std::string body)
{
    auto response =
        drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(status);
    response->setContentTypeCode(
        drogon::CT_TEXT_PLAIN);
    response->setBody(std::move(body));
    return response;
}

[[nodiscard]] std::string contentEtag(
    AttachmentResponse const& response)
{
    if (response.etag_) {
        return *response.etag_;
    }
    static auto const emptyBytes =
        std::vector<uint8_t>{};
    auto const& bytes =
        response.bytes_
            ? *response.bytes_
            : emptyBytes;
    auto digest =
        picosha2::hash256_hex_string(
            bytes);
    return "\"sha256-" + digest + "\"";
}

}  // namespace

void HttpService::Impl::handleAttachmentRequest(
    const drogon::HttpRequestPtr& req,
    std::function<void(
        const drogon::HttpResponsePtr&)>&&
        callback) const
{
    try {
        auto mapId =
            req->getParameter("mapId");
        auto layerId =
            req->getParameter("layerId");
        auto name =
            req->getParameter("name");
        if (mapId.empty() ||
            layerId.empty() ||
            name.empty())
        {
            callback(plainResponse(
                drogon::k400BadRequest,
                "mapId, layerId, tileId, and name "
                "are required."));
            return;
        }
        if (name.size() > 4096) {
            callback(plainResponse(
                drogon::k400BadRequest,
                "Attachment name is too long."));
            return;
        }

        auto request = AttachmentRequest{
            .tileKey_ = MapTileKey(
                LayerType::Features,
                std::move(mapId),
                std::move(layerId),
                TileId::fromValue(parseTileId(
                    req->getParameter(
                        "tileId")))),
            .name_ = std::move(name),
        };
        if (auto sourceId =
                req->getParameter("sourceId");
            !sourceId.empty())
        {
            request.sourceId_ =
                std::move(sourceId);
        }

        auto ifNoneMatch =
            req->getHeader("if-none-match");
        self_.requestAttachment(
            std::move(request),
            [callback = std::move(callback),
             ifNoneMatch = std::move(ifNoneMatch)](
                AttachmentResult result) mutable
            {
                if (result.status_ ==
                    RequestStatus::Unauthorized)
                {
                    callback(plainResponse(
                        drogon::k403Forbidden,
                        "Not authorized."));
                    return;
                }
                if (result.status_ !=
                        RequestStatus::Success ||
                    !result.response_)
                {
                    auto status =
                        result.status_ ==
                                RequestStatus::Aborted
                            ? drogon::
                                  k503ServiceUnavailable
                            : drogon::k404NotFound;
                    callback(plainResponse(
                        status,
                        "Attachment not found."));
                    return;
                }

                auto etag =
                    contentEtag(
                        *result.response_);
                if (ifNoneMatch == etag) {
                    auto response =
                        drogon::HttpResponse::
                            newHttpResponse();
                    response->setStatusCode(
                        drogon::k304NotModified);
                    response->addHeader(
                        "ETag",
                        etag);
                    response->addHeader(
                        "Cache-Control",
                        "private, max-age=0, "
                        "must-revalidate");
                    callback(response);
                    return;
                }

                auto response =
                    drogon::HttpResponse::
                        newHttpResponse();
                response->setStatusCode(
                    drogon::k200OK);
                response->setContentTypeString(
                    result.response_->mimeType_);
                response->addHeader(
                    "ETag",
                    etag);
                response->addHeader(
                    "Cache-Control",
                    "private, max-age=0, "
                    "must-revalidate");
                if (result.response_->bytes_) {
                    response->setBody(std::string(
                        result.response_->bytes_
                            ->begin(),
                        result.response_->bytes_
                            ->end()));
                }
                callback(response);
            },
            detail::authHeadersFromRequest(req));
    }
    catch (std::exception const& error) {
        callback(plainResponse(
            drogon::k400BadRequest,
            std::string("Invalid attachment request: ") +
                error.what()));
    }
}

}  // namespace mapget
