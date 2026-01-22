#include "mapget/detail/http-server.h"
#include "mapget/log.h"

#include <App.h>
#include <libusockets.h>

#include <atomic>
#include <cctype>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "fmt/format.h"

namespace mapget
{

// initialize the atomic activeHttpServer with nullptr
static std::atomic<HttpServer*> activeHttpServer = nullptr;

namespace
{
struct MountPoint
{
    std::string urlPrefix;
    std::filesystem::path fsRoot;
};

[[nodiscard]] bool startsWith(std::string_view s, std::string_view prefix)
{
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
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

[[nodiscard]] std::string_view guessMimeType(std::filesystem::path const& filePath)
{
    auto ext = filePath.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });

    if (ext == ".html" || ext == ".htm")
        return "text/html";
    if (ext == ".css")
        return "text/css";
    if (ext == ".js")
        return "application/javascript";
    if (ext == ".json")
        return "application/json";
    if (ext == ".svg")
        return "image/svg+xml";
    if (ext == ".png")
        return "image/png";
    if (ext == ".jpg" || ext == ".jpeg")
        return "image/jpeg";
    if (ext == ".ico")
        return "image/x-icon";
    if (ext == ".woff2")
        return "font/woff2";
    if (ext == ".woff")
        return "font/woff";
    if (ext == ".ttf")
        return "font/ttf";
    if (ext == ".txt")
        return "text/plain";

    return "application/octet-stream";
}

[[nodiscard]] std::optional<std::filesystem::path> resolveStaticFile(
    std::vector<MountPoint> const& mounts,
    std::string_view urlPath)
{
    if (mounts.empty())
        return std::nullopt;
    if (!startsWith(urlPath, "/"))
        return std::nullopt;

    // Longest-prefix match.
    MountPoint const* best = nullptr;
    for (auto const& m : mounts) {
        if (startsWith(urlPath, m.urlPrefix) && (!best || m.urlPrefix.size() > best->urlPrefix.size()))
            best = &m;
    }
    if (!best)
        return std::nullopt;

    std::string_view remainder = urlPath.substr(best->urlPrefix.size());
    if (!remainder.empty() && remainder.front() == '/')
        remainder.remove_prefix(1);

    std::filesystem::path relativePath = std::filesystem::path(std::string(remainder)).lexically_normal();
    if (relativePath.empty() || urlPath.back() == '/')
        relativePath /= "index.html";

    // Basic path traversal protection: reject any ".." segments.
    for (auto const& part : relativePath) {
        if (part == "..")
            return std::nullopt;
    }

    std::filesystem::path candidate = (best->fsRoot / relativePath).lexically_normal();
    return candidate;
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

    std::mutex mountsMutex_;
    std::vector<MountPoint> mounts_;

    uWS::Loop* loop_ = nullptr;
    us_listen_socket_t* listenSocket_ = nullptr;

    static void handleSignal(int)
    {
        // Temporarily holds the current active HttpServer
        auto* expected = activeHttpServer.load();

        // Stop the active instance when a signal is received.
        // We use compare_exchange_strong to make the operation atomic.
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
    if (isRunning())
        stop();
}

void HttpServer::go(std::string const& interfaceAddr, uint16_t port, uint32_t waitMs)
{
    if (impl_->running_ || impl_->serverThread_.joinable())
        raise("HttpServer is already running");

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
                uWS::App app;

                // Allow derived class to set up the server
                setup(app);

                // Copy mounts to avoid locking in the hot path.
                std::vector<MountPoint> mountsCopy;
                {
                    std::lock_guard lock(impl_->mountsMutex_);
                    mountsCopy = impl_->mounts_;
                }

                if (!mountsCopy.empty()) {
                    app.get(
                        "/*",
                        [mounts = std::move(mountsCopy)](auto* res, auto* req) mutable
                        {
                            auto urlPath = req->getUrl();
                            auto candidate = resolveStaticFile(mounts, urlPath);
                            if (!candidate || !std::filesystem::exists(*candidate) ||
                                !std::filesystem::is_regular_file(*candidate)) {
                                res->writeStatus("404 Not Found");
                                res->writeHeader("Content-Type", "text/plain");
                                res->end("Not found");
                                return;
                            }

                            std::ifstream ifs(*candidate, std::ios::binary);
                            if (!ifs) {
                                res->writeStatus("500 Internal Server Error");
                                res->writeHeader("Content-Type", "text/plain");
                                res->end("Failed to open file");
                                return;
                            }

                            std::string content;
                            ifs.seekg(0, std::ios::end);
                            content.resize(static_cast<size_t>(ifs.tellg()));
                            ifs.seekg(0, std::ios::beg);
                            if (!content.empty()) {
                                ifs.read(content.data(), static_cast<std::streamsize>(content.size()));
                            }

                            res->writeStatus("200 OK");
                            res->writeHeader("Content-Type", guessMimeType(*candidate));
                            res->end(content);
                        });
                }

                app.listen(
                    interfaceAddr,
                    port,
                    [this, interfaceAddr, port](us_listen_socket_t* listenSocket)
                    {
                        if (!listenSocket) {
                            impl_->notifyStart(
                                fmt::format("Could not start HttpServer on {}:{}", interfaceAddr, port));
                            return;
                        }

                        impl_->listenSocket_ = listenSocket;
                        impl_->loop_ = uWS::Loop::get();

                        // Determine actual port (port may be 0 for ephemeral).
                        impl_->port_ = static_cast<uint16_t>(
                            us_socket_local_port(0, reinterpret_cast<us_socket_t*>(listenSocket)));

                        impl_->running_ = true;
                        impl_->notifyStart();

                        if (impl_->printPortToStdout_)
                            std::cout << "====== Running on port " << impl_->port_ << " ======" << std::endl;
                        else
                            log().info("====== Running on port {} ======", impl_->port_);
                    });

                // If listen failed, exit without running the loop.
                if (!impl_->running_) {
                    if (!impl_->startNotified_) {
                        impl_->notifyStart(fmt::format("Could not start HttpServer on {}:{}", interfaceAddr, port));
                    }
                    return;
                }

                app.run();
            }
            catch (std::exception const& e) {
                impl_->notifyStart(e.what());
            }

            impl_->running_ = false;
            impl_->listenSocket_ = nullptr;
            impl_->loop_ = nullptr;
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

    if (impl_->loop_ && impl_->listenSocket_) {
        auto* loop = impl_->loop_;
        auto* listenSocket = impl_->listenSocket_;
        loop->defer([listenSocket]() { us_listen_socket_close(0, listenSocket); });
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
    // So the signal handler knows what to call
    activeHttpServer = this;

    // Set the signal handler for SIGINT and SIGTERM.
    std::signal(SIGINT, Impl::handleSignal);
    std::signal(SIGTERM, Impl::handleSignal);

    // Wait for the signal handler to stop us, or the server to shut down on its own.
    while (isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    activeHttpServer = nullptr;
}

bool HttpServer::mountFileSystem(std::string const& pathFromTo)
{
    using namespace std::ranges;
    auto parts = pathFromTo | views::split(':') |
        views::transform([](auto&& s) { return std::string(&*s.begin(), distance(s)); });
    auto partsVec = std::vector<std::string>(parts.begin(), parts.end());

    std::string urlPrefix;
    std::filesystem::path fsRoot;
    if (partsVec.size() == 1) {
        urlPrefix = "/";
        fsRoot = partsVec[0];
    } else if (partsVec.size() == 2) {
        urlPrefix = partsVec[0];
        fsRoot = partsVec[1];
    } else {
        return false;
    }

    urlPrefix = normalizeUrlPrefix(std::move(urlPrefix));

    if (!std::filesystem::exists(fsRoot) || !std::filesystem::is_directory(fsRoot))
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
