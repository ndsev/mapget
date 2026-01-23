#include "mapget/detail/http-server.h"
#include "mapget/log.h"

#include <drogon/HttpAppFramework.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
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

                // Allow derived class to set up the server.
                setup(app);

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
            return false;
    }

    urlPrefix = normalizeUrlPrefix(std::move(urlPrefix));

    std::filesystem::path fsRoot(fsRootStr);
    std::error_code ec;
    fsRoot = std::filesystem::absolute(fsRoot, ec);
    if (ec)
        return false;

    if (!std::filesystem::exists(fsRoot, ec) || ec || !std::filesystem::is_directory(fsRoot, ec) || ec)
        return false;

    std::lock_guard lock(impl_->mountsMutex_);
    impl_->mounts_.push_back(MountPoint{std::move(urlPrefix), std::move(fsRoot)});
    return true;
}

void HttpServer::printPortToStdOut(bool enabled)
{
    impl_->printPortToStdout_ = enabled;
}

}  // namespace mapget
