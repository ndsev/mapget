#pragma once

#include "service-impl.h"
#include "mapget/model/layerschema.h"

namespace mapget::detail
{

/** Bounded request-local reverse discovery using the ordinary tile scheduler. */
class IncomingRequestExecution : public std::enable_shared_from_this<IncomingRequestExecution>
{
public:
    /** Admit exact roots and asynchronously load their target and provider partitions. */
    static bool start(Service::Impl& service, FeatureLayerFilterTilesRequest::Ptr request,
                      std::optional<AuthHeaders> headers);

private:
    /** Terminalize callback failures and release all dependent requests. */
    void fail(std::string const& message);
    /** Run callbacks with a terminal failure boundary. */
    template <class Fun> void guarded(Fun&& fun) {
        if (terminal_ || request_->isCancelled()) return;
        try { fun(); } catch (std::exception const& error) { fail(error.what()); }
        catch (...) { fail("Incoming inspection failed with an unknown provider error."); }
    }
    /** Submit pre-registered jobs, closing the cancellation/enqueue race. */
    void submit(std::vector<LayerTilesRequest::Ptr> const& jobs);
    /** Record a target snapshot before discovering its provider layers. */
    void targetLoaded(PartitionFeatureLayer::Ptr const& layer);
    /** Schedule all authorized providers after every target snapshot is available. */
    void discover(RequestStatus status);
    /** Match only declared eligible families against already resolved target models. */
    void scan(PartitionFeatureLayer::Ptr const& layer,
              std::vector<LayerSchema::RelationDeclaration> const& declarations, DataSource::Ptr const& provider);
    /** Count one completed provider and publish after the last callback. */
    void providerDone(RequestStatus status);
    /** Publish unique snapshots followed by manifests and terminal success. */
    void finish();
    /** Retain a source model under a request memory budget. Caller holds mutex. */
    bool retain(PartitionFeatureLayer::Ptr const& layer);
    /** Include finite expiry and diagnostics from every dependency, including empty providers. */
    void dependency(PartitionFeatureLayer const& layer);

    Service::Impl* service_ = nullptr;
    FeatureLayerFilterTilesRequest::Ptr request_;
    std::optional<AuthHeaders> headers_;
    uint64_t catalogRevision_ = 0;
    std::mutex mutex_;
    std::atomic_bool terminal_ = false;
    size_t resolvedRoots_ = 0;
    size_t pending_ = 0;
    size_t retainedBytes_ = 0;
    std::map<MapPartitionKey, PartitionFeatureLayer::Ptr> models_;
    std::map<MapPartitionKey, std::set<std::string>> selected_;
    std::vector<IncomingRelationCollection> collections_;
    std::vector<model_ptr<Feature>> targets_;
    std::vector<std::string> issues_;
    std::optional<int64_t> expiresAtMs_;
};

} // namespace mapget::detail
