#pragma once

#include "mapget/model/featurelayer.h"

namespace mapget
{

/**
 * Small server, which implements the data source protocol expected
 * by a mapget server. The DataSource must be constructed with a
 * DataSourceInfo metadata instance, which describes the map data that
 * is provided by the source.
 */
class DataSourceServer
{
public:
    /**
     * Construct a DataSource with a DataSourceInfo metadata instance.
     */
    explicit DataSourceServer(DataSourceInfo const& info);

    /**
     * Destructor, also stops the server if it is running.
     */
    ~DataSourceServer();

    /**
     * Set the Callback which will be invoked when a `/tile`-request is received.
     * The callback argument is a fresh TileFeatureLayer, which the callback
     * must fill according to the set TileFeatureLayer's layer info and tile id.
     * If an error occurs while filling the tile, the callback can use
     * TileFeatureLayer::setError(...) to singal the error downstream.
     */
    DataSourceServer& onTileRequest(std::function<void(TileFeatureLayer&)> const&);

    /**
     * Launch the DataSource server in it's own thread.
     * Use the stop-function to stop the thread.
     * The server will also be stopped automatically, if the DataSource object is destroyed.
     * An exception will be thrown if this instance is already running,
     * or if the server fails to launch within waitMs.
     *
     * @param interface Network interface to bind to.
     * @param port Port which the DataSource HTTP server shall bind to.
     *  With port=0, a random free port will be chosen. On startup,
     *  a message will be written to stdout:
     *
     *  "====== Running on port <port> ======"
     *
     *  A parent supervisor process may parse this message to determine
     *  the chosen port, and enter the DataSource's network address into
     *  the mapget data source configuration file.
     * @param waitMs Number of milliseconds to wait for the server to start up.
     */
    void go(std::string const& interfaceAddr="0.0.0.0", uint16_t port=0, uint32_t waitMs=100);
    
    /**
     * Returns true if this instance is currently running (go() was called
     * and not stopped).
     */
    [[nodiscard]] bool isRunning();

    /**
     * Stop this instance. Will be a no-op if this instance is not running.
     */
    void stop();

    /**
     * Blocks until SIGINT or SIGTERM is received, then shuts down the server.
     * Note: You can never run this function in parallel for multiple sources
     *  within the same process!
     */
    void waitForSignal();

    /**
     * Get the port currently used by the instance, or 0 if go() has never been called.
     */
    [[nodiscard]] uint16_t port() const;

    /**
     * Get the DataSourceInfo metadata which this instance was constructed with.
     */
    DataSourceInfo const& info();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
