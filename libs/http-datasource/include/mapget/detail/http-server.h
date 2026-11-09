#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

// Forward declare Drogon app type to avoid including drogon headers in public headers.
namespace drogon {
class HttpAppFramework;
}

namespace mapget {

/** Controls whether a runtime static mount may overwrite existing files through HTTP PUT. */
enum class StaticMountAccess
{
    ReadOnly,
    ReadWrite
};

/**
 * Base class for HTTP servers. It allows derived classes to set up
 * their own endpoints via setup(). Drogon has one process-global application:
 * only one server may run at a time and a stopped instance cannot be restarted.
 */
class HttpServer
{
public:
    /** Default maximum time allowed for the listener to become ready. */
    static constexpr uint32_t DefaultStartupWaitMs = 5000;

    /**
     * Launch the server in its own thread.
     * Use the stop-function to stop the thread.
     * The server will also be stopped automatically, if the server object is destroyed.
     * An exception will be thrown if this instance is already running,
     * or if the server fails to launch within waitMs.
     *
     * @param interfaceAddr Network interface to bind to.
     * @param port Port which the HTTP server shall bind to.
     *  With port=0, the OS chooses an available port. On startup,
     *  the chosen port is available from port(). The message
     *  "====== Running on port <port> ======" goes to the logger unless the
     *  derived server enables printPortToStdOut(). RemoteDataSourceProcess reads
     *  this message from stdout to connect to its child server.
     * @param waitMs Number of milliseconds to wait for the server to start up.
     */
    void go(std::string const& interfaceAddr = "0.0.0.0",
       uint16_t port = 0,
       uint32_t waitMs = DefaultStartupWaitMs);

    /**
     * Returns true if the server is currently running.
     */
    [[nodiscard]] bool isRunning();

    /**
     * Request shutdown and join the server thread, unless called from that thread.
     */
    void stop();

    /**
     * Get the port currently used by the server, or 0 if go() has never been called.
     */
    [[nodiscard]] uint16_t port() const;

    /**
     * Install process signal handlers and wait until shutdown or SIGINT/SIGTERM.
     * Receiving either signal stops the active server.
     */
    void waitForSignal();

    /**
     * Add a filesystem mount point in the format `<url-path-prefix>:<filesystem-path>`.
     * Call before go(), which snapshots these startup mounts. Returns false for
     * an invalid or nonexistent directory. Use ensureStaticMount() after startup.
     */
    bool mountFileSystem(std::string const& pathFromTo);

    /** Resolve the directory of a valid `[url-prefix:]path` mount, using the mounting rules. */
    static std::optional<std::filesystem::path> fileSystemMountRoot(std::string const& pathFromTo);

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
    virtual void setup(drogon::HttpAppFramework&) = 0;

    /**
     * Derived servers can use this to control whether
     * the port should be printed to stdout in go.
     */
    void printPortToStdOut(bool enabled);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * Ensure that a static filesystem mount is available to the running HTTP server.
 * This is intended for embedding applications that discover additional static
 * assets after the server has already started. The mount is process-global
 * because Drogon exposes a process-global application instance.
 *
 * @param urlPrefix URL path prefix, e.g. `/assets`.
 * @param filesystemRoot Directory to serve under the prefix.
 * @param access Whether HTTP PUT may overwrite existing files in this mount.
 * @return true when the mount exists or was added successfully.
 */
bool ensureStaticMount(
    std::string const& urlPrefix,
    std::filesystem::path const& filesystemRoot,
    StaticMountAccess access = StaticMountAccess::ReadOnly);

/**
 * Ensure a static mount using the same `[<url-scope>:]<filesystem-path>` syntax
 * as HttpServer::mountFileSystem().
 */
bool ensureStaticMount(std::string const& pathFromTo);

/**
 * Remove a runtime static mount previously added through ensureStaticMount().
 * Startup mounts added through HttpServer::mountFileSystem() are not affected.
 */
bool removeStaticMount(std::string const& urlPrefix);

}  // namespace mapget
