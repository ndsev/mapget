#pragma once

#include "cache.h"
#include "locate.h"

#include "mapget/model/featurelayer.h"
#include "mapget/model/sourcedatalayer.h"

#include <regex>
#include <optional>
#include <chrono>

namespace mapget
{
/**
 * Dict which is used to store and forward authorization information
 * from the client to the datasource.
 */
using AuthHeaders = std::unordered_map<std::string, std::string>;

/**
 * Request for one named binary attachment belonging to a feature tile.
 *
 * `sourceId_` is an optional catalog assertion used by Service admission;
 * datasource implementations route by the already resolved tile key.
 */
struct AttachmentRequest
{
    MapTileKey tileKey_;
    std::string name_;
    std::optional<std::string> sourceId_;
};

/** Immutable payload and HTTP metadata returned by a datasource attachment. */
struct AttachmentResponse
{
    std::string name_;
    std::string mimeType_ =
        "application/octet-stream";
    std::shared_ptr<
        std::vector<uint8_t> const> bytes_;
    std::optional<std::string> etag_;
};

/**
 * Abstract class which defines the behavior of a mapget data source,
 * as expected by the mapget Service. Any derived data source must implement
 * the info() and fill() methods.
 */
class DataSource
{
public:
    using Ptr = std::shared_ptr<DataSource>;

    /**
     * Method which is called by a service to determine which map layers
     * can be served by this DataSource, and how many layers this
     * data source can process in parallel (i.e. how many threads may
     * run this->fill(...) in parallel).
     */
    virtual DataSourceInfo info() = 0;

    /**
     * Methods which get called up to DataSourceInfo::maxParallelJobs_
     * times in parallel to satisfy data requests for a mapget Service.
     * @param featureTile A TileFeatureLayer object which this data source
     *  should fill according the available data. If any error occurs
     *  while doing so, the data source may use TileLayer::setError.
     *  To store any extra information of interest such as timings or sizes,
     *  TileLayer::setInfo() may be used.
     */
    virtual void fill(TileFeatureLayer::Ptr const& featureTile) = 0;
    virtual void fill(TileSourceDataLayer::Ptr const& sourceData) = 0;

    /**
     * Obtain map tile keys where the feature with the specified ID may be found.
     * The implementation is completely datasource-specific. Note, that the returned
     * resulting LocateResponses may have resolved to a different typeId and featureId
     * than the requested one. This is useful when locate is used to resolve a
     * secondary to a primary feature ID.
     */
    virtual std::vector<LocateResponse> locate(LocateRequest const& req);

    /**
     * Locate candidate tiles without requiring the candidate feature to be
     * materialized. The default preserves the existing locate contract.
     *
     * Datasources whose secondary-id lookup normally needs a complete tile
     * may override this method with a cheap tile-only answer, then canonicalize
     * the identity in resolveFeatures() after the ordinary cached tile load.
     */
    virtual std::vector<LocateResponse> locateCandidates(LocateRequest const& req);

    /**
     * Resolve one located identity against an already loaded complete tile.
     *
     * Zero results mean missing and several results mean ambiguous. The
     * default performs the ordinary primary-id lookup. Implementations must
     * not fetch or reconstruct the tile again.
     */
    virtual std::vector<model_ptr<Feature>> resolveFeatures(
        LocateRequest const& located,
        TileFeatureLayer const& tile);

    /**
     * Produce or return one named tile attachment.
     *
     * The default reports no attachment. Implementations may retain values
     * produced during fill() or construct them lazily. The returned name must
     * equal the requested name.
     */
    virtual std::optional<AttachmentResponse> attachment(
        AttachmentRequest const& request);

    /** Called by mapget::Service worker. Dispatches to Cache or fill(...) on miss. */
    virtual TileLayer::Ptr get(
        MapTileKey const& k,
        Cache::Ptr& cache,
        DataSourceInfo const& info,
        TileLayer::LoadStateCallback loadStateCallback = {});

    /** Add an authorization header-regex pair for this datasource. */
    void requireAuthHeaderRegexMatchOption(std::string header, std::regex re);

    /**
     * Validate that one of the given authorization header-value pairs authorizes
     * use of this datasource, if it is restricted.
     */
    [[nodiscard]] bool isDataSourceAuthorized(AuthHeaders const& clientHeaders) const;

    /**
     * Set a TTL fallback for all tiles produced by this datasource.
     * A value of 0ms means infinite TTL.
     */
    void setTtl(std::optional<std::chrono::milliseconds> ttl);

    /** Get the currently configured TTL fallback (if any). */
    [[nodiscard]] std::optional<std::chrono::milliseconds> ttl() const;

    /** Called when a cached tile was present but expired. Default no-op. */
    virtual void onCacheExpired(const MapTileKey& /*tileKey*/, std::chrono::system_clock::time_point /*expiredAt*/)
    {
        // Nothing to do.
    }

protected:
    static StringId cachedStringPoolOffset(std::string const& stringPoolId, Cache::Ptr const& cache);

    /** Map of authorization header-regex pairs which can be entered into the datasource YAML config. */
    std::unordered_map<std::string, std::regex> authHeaderAlternatives_;

    /** TTL fallback applied to generated tiles (0 = infinite, unset = use service default). */
    std::optional<std::chrono::milliseconds> ttl_;
};

}
