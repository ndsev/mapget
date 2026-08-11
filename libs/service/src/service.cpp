#include "service.h"
#include "service-impl.h"

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace mapget
{

size_t Service::defaultWorkerCount() noexcept
{
    auto const hardwareThreads = std::max<size_t>(1, std::thread::hardware_concurrency());
    return std::min<size_t>(32, std::max<size_t>(16, hardwareThreads * 2));
}

Service::Service(
    Cache::Ptr cache,
    bool useDataSourceConfig,
    std::optional<std::chrono::milliseconds> defaultTtl,
    size_t workerCount)
    : impl_(std::make_unique<Impl>(std::move(cache), useDataSourceConfig, defaultTtl, workerCount))
{
}

Service::~Service() = default;

Service::DataSourceCatalogSubscription::DataSourceCatalogSubscription(Service* service, uint64_t id)
    : service_(service), id_(id)
{
}

Service::DataSourceCatalogSubscription::~DataSourceCatalogSubscription()
{
    if (service_ && id_) {
        service_->impl_->removeSourceCatalogCallback(id_);
    }
}

Service::DataSourceCatalogSubscription::DataSourceCatalogSubscription(
    DataSourceCatalogSubscription&& other) noexcept
    : service_(std::exchange(other.service_, nullptr)), id_(std::exchange(other.id_, 0))
{
}

Service::DataSourceCatalogSubscription&
Service::DataSourceCatalogSubscription::operator=(DataSourceCatalogSubscription&& other) noexcept
{
    if (this == &other) {
        return *this;
    }
    if (service_ && id_) {
        service_->impl_->removeSourceCatalogCallback(id_);
    }
    service_ = std::exchange(other.service_, nullptr);
    id_ = std::exchange(other.id_, 0);
    return *this;
}

void Service::add(DataSource::Ptr const& dataSource)
{
    impl_->addDataSource(dataSource);
}

void Service::remove(const DataSource::Ptr& dataSource)
{
    impl_->removeDataSource(dataSource);
}

bool Service::request(
    std::vector<LayerTilesRequest::Ptr> const& requests,
    std::optional<AuthHeaders> const& clientHeaders)
{
    return impl_->requestTiles(requests, clientHeaders);
}

bool Service::request(
    FeatureLayerFilterTilesRequest::Ptr const& request,
    std::optional<AuthHeaders> const& clientHeaders)
{
    return impl_->requestFilter(request, clientHeaders);
}

AttachmentResult Service::attachment(
    AttachmentRequest const& request,
    std::optional<AuthHeaders> const& clientHeaders)
{
    std::mutex resultMutex;
    std::condition_variable resultAvailable;
    std::optional<AttachmentResult> completed;
    requestAttachment(
        request,
        [&](AttachmentResult result)
        {
            {
                std::lock_guard lock(resultMutex);
                completed = std::move(result);
            }
            resultAvailable.notify_one();
        },
        clientHeaders);
    std::unique_lock lock(resultMutex);
    resultAvailable.wait(lock, [&] { return completed.has_value(); });
    auto result = std::move(*completed);
    return result;
}

void Service::requestAttachment(
    AttachmentRequest request,
    std::function<void(AttachmentResult)> callback,
    std::optional<AuthHeaders> const& clientHeaders)
{
    impl_->requestAttachment(std::move(request), std::move(callback), clientHeaders);
}

bool Service::request(
    std::vector<FeatureLayerFilterTilesRequest::Ptr> const& requests,
    std::optional<AuthHeaders> const& clientHeaders)
{
    bool allAccepted = true;
    for (auto const& request : requests) {
        allAccepted = this->request(request, clientHeaders) && allAccepted;
    }
    return allAccepted;
}

std::vector<LocateResponse> Service::locate(LocateRequest const& request)
{
    return impl_->locate(request);
}

void Service::abort(const LayerTilesRequest::Ptr& r)
{
    impl_->scheduler_.abortRequest(r);
}

void Service::abort(const FeatureLayerFilterTilesRequest::Ptr& r)
{
    if (!r || r->isDone()) {
        return;
    }
    r->cancel();
    impl_->scheduler_.abortFilterJobs(r);
    std::vector<LayerTilesRequest::Ptr> childRequests;
    {
        std::lock_guard lock(r->childRequestsMutex_);
        childRequests = r->childRequests_;
        r->childRequests_.clear();
    }
    for (auto const& child : childRequests) {
        if (child && !child->isDone()) {
            impl_->scheduler_.abortRequest(child);
        }
    }
}

std::vector<DataSourceInfo> Service::info(std::optional<AuthHeaders> const& clientHeaders)
{
    return impl_->getDataSourceInfos(clientHeaders);
}

DataSourceCatalogSnapshot
Service::sourceCatalog(std::optional<AuthHeaders> const& clientHeaders, bool waitUntilReloadDone)
    const
{
    return impl_->getSourceCatalog(clientHeaders, waitUntilReloadDone);
}

uint64_t Service::sourceCatalogRevision() const
{
    return impl_->getSourceCatalogRevision();
}

bool Service::isSourceCatalogChangeVisible(
    DataSourceCatalogChange const& change,
    std::optional<AuthHeaders> const& clientHeaders) const
{
    return impl_->isSourceCatalogChangeVisible(change, clientHeaders);
}

Service::DataSourceCatalogSubscription
Service::subscribeToSourceCatalogChanges(DataSourceCatalogCallback callback)
{
    return DataSourceCatalogSubscription(
        this,
        impl_->addSourceCatalogCallback(std::move(callback)));
}

Cache::Ptr Service::cache()
{
    return impl_->scheduler_.cache();
}

RequestStatus Service::hasLayerAndCanAccess(
    std::string const& mapId,
    std::string const& layerId,
    std::optional<AuthHeaders> const& clientHeaders) const
{
    return resolveLayerRequest(mapId, layerId, clientHeaders).status_;
}

LayerRequestContext Service::resolveLayerRequest(
    std::string const& mapId,
    std::string const& layerId,
    std::optional<AuthHeaders> const& clientHeaders,
    std::optional<std::string> const& sourceId) const
{
    return impl_->resolveLayerRequest(mapId, layerId, clientHeaders, sourceId);
}

}  // namespace mapget
