#include "mapget/detail/http-server.h"

#include "httplib.h"
#include <csignal>
#include <atomic>

#include "stx/format.h"
#include "stx/string.h"

namespace mapget
{

// initialize the atomic activeHttpServer with nullptr
static std::atomic<HttpServer*> activeHttpServer = nullptr;

struct HttpServer::Impl
{
    httplib::Server server_;
    std::thread serverThread_;
    uint16_t port_ = 0;
    bool setupWasCalled_ = false;

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
};

HttpServer::HttpServer() : impl_(new Impl()) {}

HttpServer::~HttpServer() {
    if (isRunning())
        stop();
}

void HttpServer::go(
    std::string const& interfaceAddr,
    uint16_t port,
    uint32_t waitMs)
{
    if (!impl_->setupWasCalled_) {
        // Allow derived class to set up the server
        setup(impl_->server_);
        impl_->setupWasCalled_ = true;
    }

    if (impl_->server_.is_running() || impl_->serverThread_.joinable())
        throw std::runtime_error("HttpServer is already running");

    if (port == 0) {
        impl_->port_ = impl_->server_.bind_to_any_port(interfaceAddr);
    }
    else {
        impl_->port_ = port;
        impl_->server_.bind_to_port(interfaceAddr, port);
    }

    impl_->serverThread_ = std::thread(
        [this, interfaceAddr]
        {
            std::cout << "====== Running on port " << impl_->port_ << " ======" << std::endl;
            impl_->server_.listen_after_bind();
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
    if (!impl_->server_.is_running() || !impl_->server_.is_valid())
        throw std::runtime_error(stx::format("Could not start HttpServer on {}:{}", interfaceAddr, port));
}

bool HttpServer::isRunning() {
    return impl_->server_.is_running();
}

void HttpServer::stop() {
    if (!impl_->server_.is_running())
        return;

    impl_->server_.stop();
    impl_->serverThread_.join();
}

uint16_t HttpServer::port() const {
    return impl_->port_;
}

void HttpServer::waitForSignal() {
    // So the signal handler knows what to call
    activeHttpServer = this;

    // Set the signal handler for SIGINT and SIGTERM.
    std::signal(SIGINT, Impl::handleSignal);
    std::signal(SIGTERM, Impl::handleSignal);

    // Wait for the signal handler to stop us, or the server to shut down on its own.
    while (isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds (200));
    }

    activeHttpServer = nullptr;
}

bool HttpServer::mountFileSystem(const std::string& pathFromTo)
{
    auto parts = stx::split(pathFromTo, ":");
    if (parts.size() == 1)
        return impl_->server_.set_mount_point("/", parts[0]);
    return impl_->server_.set_mount_point(parts[0], parts[1]);
}

}  // namespace mapget
