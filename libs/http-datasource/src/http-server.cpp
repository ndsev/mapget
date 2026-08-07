#include "mapget/detail/http-server.h"
#include "mapget/log.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "fmt/format.h"

namespace mapget
{

// Used by waitForSignal() so the signal handler knows what to stop.
static std::atomic<HttpServer*> activeHttpServer = nullptr;

// Drogon uses a singleton app instance; running multiple independent servers
// in-process is not supported.
static std::atomic<HttpServer*> activeDrogonServer = nullptr;

namespace
{

struct MountPoint
{
    std::string urlPrefix;
    std::filesystem::path fsRoot;
    StaticMountAccess access = StaticMountAccess::ReadOnly;
};

struct RuntimeStaticMountRegistry
{
    std::mutex mutex;
    std::vector<MountPoint> mounts;
};

[[nodiscard]] bool looksLikeWindowsDrivePath(std::string_view s)
{
    if (s.size() < 3)
        return false;
    const unsigned char drive = static_cast<unsigned char>(s[0]);
    return std::isalpha(drive) && s[1] == ':' && (s[2] == '\\' || s[2] == '/');
}

[[nodiscard]] std::string normalizeUrlPrefix(std::string prefix)
{
    if (prefix.empty())
        prefix = "/";
    if (prefix.front() != '/')
        prefix.insert(prefix.begin(), '/');
    if (prefix.size() > 1 && prefix.back() == '/')
        prefix.pop_back();
    return prefix;
}

[[nodiscard]] RuntimeStaticMountRegistry& runtimeStaticMountRegistry()
{
    static RuntimeStaticMountRegistry registry;
    return registry;
}

[[nodiscard]] std::optional<MountPoint> normalizeMountPoint(
    std::string urlPrefix,
    std::filesystem::path fsRoot,
    StaticMountAccess access = StaticMountAccess::ReadOnly)
{
    urlPrefix = normalizeUrlPrefix(std::move(urlPrefix));

    std::error_code ec;
    fsRoot = std::filesystem::absolute(fsRoot, ec);
    if (ec)
        return std::nullopt;
    fsRoot = std::filesystem::weakly_canonical(fsRoot, ec);
    if (ec)
        return std::nullopt;

    auto exists = std::filesystem::exists(fsRoot, ec);
    if (!exists || ec)
        return std::nullopt;
    auto isDirectory = std::filesystem::is_directory(fsRoot, ec);
    if (!isDirectory || ec)
        return std::nullopt;

    return MountPoint{std::move(urlPrefix), std::move(fsRoot), access};
}

[[nodiscard]] std::optional<MountPoint> parseMountPoint(std::string const& pathFromTo)
{
    std::string urlPrefix;
    std::string fsRootStr;

    const auto firstColon = pathFromTo.find(':');
    if (firstColon == std::string::npos || looksLikeWindowsDrivePath(pathFromTo)) {
        urlPrefix = "/";
        fsRootStr = pathFromTo;
    } else {
        urlPrefix = pathFromTo.substr(0, firstColon);
        fsRootStr = pathFromTo.substr(firstColon + 1);
        if (fsRootStr.empty())
            return std::nullopt;
    }

    return normalizeMountPoint(std::move(urlPrefix), std::filesystem::path(fsRootStr));
}

[[nodiscard]] bool pathIsWithin(std::filesystem::path const& path, std::filesystem::path const& root)
{
    auto pathIt = path.begin();
    for (auto rootIt = root.begin(); rootIt != root.end(); ++rootIt, ++pathIt) {
        if (pathIt == path.end() || *pathIt != *rootIt) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool requestPathMatchesPrefix(std::string_view requestPath, std::string_view urlPrefix)
{
    if (requestPath == urlPrefix)
        return true;
    return requestPath.size() > urlPrefix.size()
        && requestPath.substr(0, urlPrefix.size()) == urlPrefix
        && requestPath[urlPrefix.size()] == '/';
}

[[nodiscard]] std::optional<std::filesystem::path> resolveMountedRequestPath(
    MountPoint const& mount,
    std::string const& requestPath)
{
    if (!requestPathMatchesPrefix(requestPath, mount.urlPrefix))
        return std::nullopt;

    auto relativePath = requestPath == mount.urlPrefix
        ? std::string()
        : requestPath.substr(mount.urlPrefix.size() + 1);
    if (relativePath.empty())
        return std::nullopt;

    std::error_code ec;
    auto candidate = std::filesystem::weakly_canonical(mount.fsRoot / std::filesystem::path(relativePath), ec);
    if (ec || !pathIsWithin(candidate, mount.fsRoot))
        return std::nullopt;

    auto exists = std::filesystem::exists(candidate, ec);
    if (!exists || ec)
        return std::nullopt;
    auto isRegularFile = std::filesystem::is_regular_file(candidate, ec);
    if (!isRegularFile || ec)
        return std::nullopt;

    return candidate;
}

[[nodiscard]] std::optional<drogon::HttpResponsePtr> dynamicStaticMountResponse(
    drogon::HttpRequestPtr const& req)
{
    if (req->method() != drogon::Get
        && req->method() != drogon::Head
        && req->method() != drogon::Put) {
        return std::nullopt;
    }

    std::vector<MountPoint> mounts;
    {
        auto& registry = runtimeStaticMountRegistry();
        std::lock_guard lock(registry.mutex);
        mounts = registry.mounts;
    }
    if (mounts.empty())
        return std::nullopt;

    std::sort(
        mounts.begin(),
        mounts.end(),
        [](MountPoint const& a, MountPoint const& b) { return a.urlPrefix.size() > b.urlPrefix.size(); });

    for (auto const& mount : mounts) {
        if (auto localPath = resolveMountedRequestPath(mount, req->path())) {
            if (req->method() == drogon::Put) {
                auto response = drogon::HttpResponse::newHttpResponse();
                response->setContentTypeCode(drogon::CT_TEXT_PLAIN);
                if (mount.access != StaticMountAccess::ReadWrite) {
                    response->setStatusCode(drogon::k403Forbidden);
                    response->setBody("Static mount is read-only.");
                    return response;
                }

                std::ofstream output(*localPath, std::ios::binary | std::ios::trunc);
                auto const body = req->body();
                output.write(body.data(), static_cast<std::streamsize>(body.size()));
                output.close();
                if (!output) {
                    response->setStatusCode(drogon::k500InternalServerError);
                    response->setBody("Failed to write static file.");
                    return response;
                }

                response->setStatusCode(drogon::k200OK);
                response->setBody("Static file updated.");
                return response;
            }
            auto response = drogon::HttpResponse::newFileResponse(
                localPath->string(), "", drogon::CT_NONE, "", req);
            if (mount.access == StaticMountAccess::ReadWrite) {
                response->addHeader("Cache-Control", "no-store");
            }
            return response;
        }
    }

    return std::nullopt;
}

}  // namespace

struct HttpServer::Impl
{
    std::thread serverThread_;
    std::atomic_bool running_{false};

    std::mutex startMutex_;
    std::condition_variable startCv_;
    bool startNotified_ = false;
    std::string startError_;

    uint16_t port_ = 0;
    bool printPortToStdout_ = false;
    bool startedOnce_ = false;

    std::mutex mountsMutex_;
    std::vector<MountPoint> mounts_;

    static void handleSignal(int)
    {
        auto* expected = activeHttpServer.load();
        if (activeHttpServer.compare_exchange_strong(expected, nullptr)) {
            if (expected) {
                expected->stop();
            }
        }
    }

    void notifyStart(std::string errorMessage = {})
    {
        std::lock_guard lock(startMutex_);
        startError_ = std::move(errorMessage);
        startNotified_ = true;
        startCv_.notify_one();
    }
};

HttpServer::HttpServer() : impl_(new Impl()) {}

HttpServer::~HttpServer()
{
    stop();
}

void HttpServer::go(std::string const& interfaceAddr, uint16_t port, uint32_t waitMs)
{
    if (impl_->running_ || impl_->serverThread_.joinable())
        raise("HttpServer is already running");
    if (impl_->startedOnce_)
        raise("HttpServer cannot be restarted in-process (Drogon singleton)");

    HttpServer* expected = nullptr;
    if (!activeDrogonServer.compare_exchange_strong(expected, this))
        raise("Only one HttpServer can run per process (Drogon singleton)");

    impl_->startedOnce_ = true;

    // Reset start state.
    {
        std::lock_guard lock(impl_->startMutex_);
        impl_->startNotified_ = false;
        impl_->startError_.clear();
    }

    impl_->serverThread_ = std::thread(
        [this, interfaceAddr, port]
        {
            try {
                auto& app = drogon::app();

                // Copy mounts to avoid locking after the server thread starts.
                std::vector<MountPoint> mountsCopy;
                {
                    std::lock_guard lock(impl_->mountsMutex_);
                    mountsCopy = impl_->mounts_;
                }

                if (!mountsCopy.empty()) {
                    std::sort(
                        mountsCopy.begin(),
                        mountsCopy.end(),
                        [](MountPoint const& a, MountPoint const& b) { return a.urlPrefix.size() > b.urlPrefix.size(); });

                    // Using empty document root makes addALocation's "alias" parameter
                    // work with absolute Windows paths (e.g. "C:/path").
                    app.setDocumentRoot("");

                    for (auto const& m : mountsCopy) {
                        app.addALocation(m.urlPrefix, "", m.fsRoot.generic_string());
                    }
                }

                app.registerPreRoutingAdvice(
                    [](drogon::HttpRequestPtr const& req,
                       drogon::AdviceCallback&& callback,
                       drogon::AdviceChainCallback&& chainCallback) {
                        if (auto response = dynamicStaticMountResponse(req)) {
                            callback(*response);
                            return;
                        }
                        chainCallback();
                    });

                // Allow derived class to set up the server.
                setup(app);

                // Raise Drogon's default WebSocket client message size limit
                // (128 KB) to 10 MB. Large tile requests with many tile IDs
                // can exceed the default and trigger connection shutdown,
                // cascading into a crash (std::terminate).
                app.setClientMaxWebSocketMessageSize(10 * 1024 * 1024);

                app.addListener(interfaceAddr, port);

                app.registerBeginningAdvice([this]() {
                    // Beginning advice runs before listeners start. Post the actual
                    // startup notification to run after startListening() completed.
                    drogon::app().getLoop()->queueInLoop([this]() {
                        auto listeners = drogon::app().getListeners();
                        if (listeners.empty()) {
                            impl_->notifyStart("HttpServer started without listeners");
                            return;
                        }

                        impl_->port_ = listeners.front().toPort();
                        impl_->running_ = true;
                        impl_->notifyStart();

                        if (impl_->printPortToStdout_)
                            std::cout << "====== Running on port " << impl_->port_ << " ======" << std::endl;
                        else
                            log().info("====== Running on port {} ======", impl_->port_);
                    });
                });

                app.run();
            }
            catch (std::exception const& e) {
                impl_->notifyStart(e.what());
            }

            impl_->running_ = false;

            HttpServer* expected = this;
            (void)activeDrogonServer.compare_exchange_strong(expected, nullptr);
        });

    std::unique_lock lk(impl_->startMutex_);
    if (!impl_->startCv_.wait_for(
            lk,
            std::chrono::milliseconds(waitMs),
            [this] { return impl_->startNotified_; })) {
        raise(fmt::format("Could not start HttpServer on {}:{} (timeout)", interfaceAddr, port));
    }

    if (!impl_->startError_.empty())
        raise(impl_->startError_);
}

bool HttpServer::isRunning()
{
    return impl_->running_;
}

void HttpServer::stop()
{
    if (!impl_->serverThread_.joinable())
        return;

    if (drogon::app().isRunning()) {
        drogon::app().quit();
    }

    if (impl_->serverThread_.get_id() != std::this_thread::get_id())
        impl_->serverThread_.join();
}

uint16_t HttpServer::port() const
{
    return impl_->port_;
}

void HttpServer::waitForSignal()
{
    activeHttpServer = this;

    std::signal(SIGINT, Impl::handleSignal);
    std::signal(SIGTERM, Impl::handleSignal);

    while (isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    activeHttpServer = nullptr;
}

bool HttpServer::mountFileSystem(std::string const& pathFromTo)
{
    auto mount = parseMountPoint(pathFromTo);
    if (!mount)
        return false;

    std::scoped_lock lock(impl_->mountsMutex_);
    impl_->mounts_.emplace_back(std::move(*mount));
    return true;
}

void HttpServer::printPortToStdOut(bool enabled)
{
    impl_->printPortToStdout_ = enabled;
}

bool ensureStaticMount(
    std::string const& urlPrefix,
    std::filesystem::path const& filesystemRoot,
    StaticMountAccess access)
{
    auto mount = normalizeMountPoint(urlPrefix, filesystemRoot, access);
    if (!mount)
        return false;

    auto& registry = runtimeStaticMountRegistry();
    std::lock_guard lock(registry.mutex);
    for (auto const& existing : registry.mounts) {
        if (existing.urlPrefix != mount->urlPrefix)
            continue;
        if (existing.fsRoot == mount->fsRoot && existing.access == mount->access)
            return true;
        log().warn(
            "Refusing to remount static URL prefix {} from {} to {}.",
            mount->urlPrefix,
            existing.fsRoot.generic_string(),
            mount->fsRoot.generic_string());
        return false;
    }

    log().info(
        "Static mount: {}:{}",
        mount->urlPrefix,
        mount->fsRoot.generic_string());
    registry.mounts.emplace_back(std::move(*mount));
    return true;
}

bool ensureStaticMount(std::string const& pathFromTo)
{
    auto mount = parseMountPoint(pathFromTo);
    if (!mount)
        return false;
    return ensureStaticMount(mount->urlPrefix, mount->fsRoot, StaticMountAccess::ReadOnly);
}

bool removeStaticMount(std::string const& urlPrefix)
{
    auto normalizedUrlPrefix = normalizeUrlPrefix(urlPrefix);
    auto& registry = runtimeStaticMountRegistry();
    std::lock_guard lock(registry.mutex);
    auto previousSize = registry.mounts.size();
    registry.mounts.erase(
        std::remove_if(
            registry.mounts.begin(),
            registry.mounts.end(),
            [&normalizedUrlPrefix](MountPoint const& mount) {
                return mount.urlPrefix == normalizedUrlPrefix;
            }),
        registry.mounts.end());

    if (registry.mounts.size() == previousSize)
        return false;

    log().info("Removed static mount: {}", normalizedUrlPrefix);
    return true;
}

}  // namespace mapget
