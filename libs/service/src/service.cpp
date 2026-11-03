#include "service.h"
#include "service-filter.h"
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
    size_t workerCount,
    std::chrono::milliseconds errorTileTtl,
    std::chrono::milliseconds emptyTileTtl)
    : impl_(std::make_unique<Impl>(
          std::move(cache),
          useDataSourceConfig,
          defaultTtl,
          workerCount,
          errorTileTtl,
          emptyTileTtl))
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
    std::optional<ClientAuthorization> const& clientAuthorization)
{
    return impl_->requestTiles(requests, clientAuthorization);
}

bool Service::request(
    FeatureLayerFilterTilesRequest::Ptr const& request,
    std::optional<ClientAuthorization> const& clientAuthorization)
{
    return impl_->requestFilter(request, clientAuthorization);
}

AttachmentResult Service::attachment(
    AttachmentRequest const& request,
    std::optional<ClientAuthorization> const& clientAuthorization)
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
        clientAuthorization);
    std::unique_lock lock(resultMutex);
    resultAvailable.wait(lock, [&] { return completed.has_value(); });
    auto result = std::move(*completed);
    return result;
}

void Service::requestAttachment(
    AttachmentRequest request,
    std::function<void(AttachmentResult)> callback,
    std::optional<ClientAuthorization> const& clientAuthorization)
{
    impl_->requestAttachment(std::move(request), std::move(callback), clientAuthorization);
}

bool Service::request(
    std::vector<FeatureLayerFilterTilesRequest::Ptr> const& requests,
    std::optional<ClientAuthorization> const& clientAuthorization)
{
    bool allAccepted = true;
    for (auto const& request : requests) {
        allAccepted = this->request(request, clientAuthorization) && allAccepted;
    }
    return allAccepted;
}

std::vector<LocateResponse> Service::locate(
    LocateRequest const& request, std::optional<ClientAuthorization> const& authorization)
{
    return impl_->locate(request, authorization);
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
    std::shared_ptr<detail::FilterRequestExecution> execution;
    {
        std::lock_guard lock(r->childRequestsMutex_);
        execution = r->execution_.lock();
    }
    if (execution) {
        execution->cancel();
        return;
    }
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

void Service::retainOutputs(
    LayerTilesRequest::Ptr const& request,
    std::set<PartitionId> const& retainedPartitionIds)
{
    impl_->scheduler_.retainRequestOutputs(request, retainedPartitionIds);
}

void Service::retainOutputs(
    FeatureLayerFilterTilesRequest::Ptr const& request,
    std::set<PartitionId> const& retainedPartitionIds)
{
    if (!request || request->isDone()) {
        return;
    }
    if (retainedPartitionIds.empty()) {
        abort(request);
        return;
    }
    std::shared_ptr<detail::FilterRequestExecution> execution;
    {
        std::lock_guard lock(request->childRequestsMutex_);
        execution = request->execution_.lock();
    }
    if (execution) {
        execution->retainOutputs(retainedPartitionIds);
    }
}

void Service::notifyWorkAvailable()
{
    impl_->scheduler_.notifyWorkAvailable();
}

bool Service::scheduleTask(std::function<void(bool)> task)
{
    return impl_->scheduler_.enqueueTask(std::move(task));
}

std::vector<DataSourceInfo> Service::info(std::optional<ClientAuthorization> const& clientAuthorization)
{
    return impl_->getDataSourceInfos(clientAuthorization);
}

std::shared_ptr<MapSchema const> Service::getSchema(
    std::string_view sourceId,
    std::string_view schemaId,
    std::optional<ClientAuthorization> const& clientAuthorization) const
{
    return impl_->dataSources_.getSchema(sourceId, schemaId, clientAuthorization);
}

nlohmann::json Service::schemaInventory(
    std::optional<ClientAuthorization> const& clientAuthorization,
    std::function<bool(MapSchema const&)> const& include,
    std::function<void()> const& checkpoint,
    std::function<bool(std::string_view sourceId, std::string_view mapId)> const& includeSource)
    const
{
    return impl_->dataSources_.schemaInventory(clientAuthorization, include, checkpoint, includeSource);
}

DataSourceCatalogSnapshot
Service::sourceCatalog(std::optional<ClientAuthorization> const& clientAuthorization, bool waitUntilReloadDone)
    const
{
    return impl_->getSourceCatalog(clientAuthorization, waitUntilReloadDone);
}

uint64_t Service::sourceCatalogRevision() const
{
    return impl_->getSourceCatalogRevision();
}

bool Service::isSourceCatalogChangeVisible(
    DataSourceCatalogChange const& change,
    std::optional<ClientAuthorization> const& clientAuthorization) const
{
    return impl_->isSourceCatalogChangeVisible(change, clientAuthorization);
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
    std::optional<ClientAuthorization> const& clientAuthorization) const
{
    return resolveLayerRequest(mapId, layerId, clientAuthorization).status_;
}

LayerRequestContext Service::resolveLayerRequest(
    std::string const& mapId,
    std::string const& layerId,
    std::optional<ClientAuthorization> const& clientAuthorization,
    std::optional<std::string> const& sourceId) const
{
    return impl_->resolveLayerRequest(mapId, layerId, clientAuthorization, sourceId);
}

bool Service::resetMapCache(
    std::string const& mapId,
    std::optional<ClientAuthorization> const& clientAuthorization)
{
    return impl_->resetMapCache(mapId, clientAuthorization);
}

}  // namespace mapget
