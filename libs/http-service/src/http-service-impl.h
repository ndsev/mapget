#pragma once

#include "http-service.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <fstream>

namespace mapget
{

namespace detail
{

[[nodiscard]] inline AuthHeaders authHeadersFromRequest(const drogon::HttpRequestPtr& req)
{
    AuthHeaders headers;
    for (auto const& [k, v] : req->headers()) {
        headers.emplace(k, v);
    }
    return headers;
}

}  // namespace detail

struct HttpService::Impl
{
    HttpService& self_;
    HttpServiceConfig config_;
    mutable std::atomic<uint64_t> binaryRequestCounter_{0};
    mutable std::atomic<uint64_t> jsonRequestCounter_{0};

    explicit Impl(HttpService& self, const HttpServiceConfig& config);

    enum class ResponseType { Binary, Json };

    void tryMemoryTrim(ResponseType responseType) const;

    struct TilesStreamState;

    void handleTilesRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback) const;

    void handleSourcesRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback) const;

    void handleStatusRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback) const;

    void handleStatusDataRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback) const;

    void handleLocateRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback) const;

    static drogon::HttpResponsePtr openConfigFile(std::ifstream& configFile);

    static void handleGetConfigRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void handlePostConfigRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback) const;
};

}  // namespace mapget
