#pragma once

#include <memory>
#include <string>

// Pre-declare httplib::Server to avoid including httplib.h in header
namespace httplib {
class Server;
}

namespace mapget {

/**
 * Base class for HTTP servers. It allows derived classes to set up
 * their own endpoints via the setup() function.
 */
class HttpServer
{
public:
    /**
     * Launch the server in its own thread.
     * Use the stop-function to stop the thread.
     * The server will also be stopped automatically, if the server object is destroyed.
     * An exception will be thrown if this instance is already running,
     * or if the server fails to launch within waitMs.
     *
     * @param interface Network interface to bind to.
     * @param port Port which the HTTP server shall bind to.
     *  With port=0, a random free port will be chosen. On startup,
     *  a message will be written to stdout:
     *  "====== Running on port <port> ======"
     * @param waitMs Number of milliseconds to wait for the server to start up.
     */
    void go(std::string const& interfaceAddr = "0.0.0.0",
       uint16_t port = 0,
       uint32_t waitMs = 100);

    /**
     * Returns true if the server is currently running.
     */
    [[nodiscard]] bool isRunning();

    /**
     * Stop the server.
     */
    void stop();

    /**
     * Get the port currently used by the server, or 0 if go() has never been called.
     */
    [[nodiscard]] uint16_t port() const;

    /**
     * Wait until SIGINT or SIGTERM is received, then shuts down the server.
     */
    void waitForSignal();

    /**
     * Add a filesystem mount point in the format `<url-path-prefix>:<filesystem-path>`.
     * Returns true if successful, false otherwise.
     */
    bool mountFileSystem(std::string const& pathFromTo);

protected:
    /**
     * Constructor.
     */
    HttpServer();

    /**
     * Destructor, also stops the server if it is running.
     */
    virtual ~HttpServer();

    /**
     * This function is called upon the first call to go(),
     * and allows any derived server class to add endpoints.
     */
    virtual void setup(httplib::Server&) = 0;

    /**
     * Derived servers can use this to control whether
     * the port should be printed to stdout in go.
     */
    void printPortToStdOut(bool enabled);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mapget
