#include "service-scheduler.h"

#include "mapget/log.h"
#include "service-tiles.h"

#include <algorithm>
#include <unordered_set>

namespace mapget::detail
{

ServiceScheduler::ServiceScheduler(
    DataSourceRegistry& dataSources,
    Cache::Ptr cache,
    std::optional<std::chrono::milliseconds> defaultTtl,
    size_t workerCount)
    : dataSources_(dataSources), cache_(std::move(cache)), defaultTtl_(defaultTtl)
{
    if (!cache_) {
        raise("Cache must not be null!");
    }
    if (workerCount == 0) {
        raise("Service worker count must be greater than zero.");
    }

    workers_.reserve(workerCount);
    for (size_t index = 0; index < workerCount; ++index) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

ServiceScheduler::~ServiceScheduler() noexcept
{
    stop();
}

void ServiceScheduler::registerDataSource(RegisteredDataSource::Ptr const& source)
{
    if (!source || source->info->isAddOn_) {
        return;
    }
    auto concurrency = std::make_shared<SourceConcurrency>(SourceConcurrency{
        .source = source,
        .limit = static_cast<size_t>(source->info->maxParallelJobs_),
    });
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            return;
        }
        sources_.push_back(std::move(concurrency));
    }
    jobsAvailable_.notify_all();
}

void ServiceScheduler::unregisterDataSource(RegisteredDataSource::Ptr const& source)
{
    if (!source || source->info->isAddOn_) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        for (auto const& state : sources_) {
            if (state->source == source) {
                state->enabled = false;
            }
        }
        std::erase_if(sources_, [&](auto const& state) { return state->source == source; });
        if (sources_.empty()) {
            nextSourceIndex_ = 0;
        }
        else {
            nextSourceIndex_ %= sources_.size();
        }
    }
    jobsAvailable_.notify_all();
}

void ServiceScheduler::enqueueRequest(LayerTilesRequest::Ptr request)
{
    if (!request) {
        raise("Attempt to enqueue a null LayerTilesRequest.");
    }
    if (request->isDone()) {
        request->notifyStatus();
        return;
    }

    auto reject = false;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            reject = true;
        }
        else {
            requests_.push_back(request);
        }
    }
    // Completion callbacks are external and may re-enter the service, so they
    // must never execute while the scheduler mutex is held.
    if (reject) {
        request->setStatus(RequestStatus::Aborted);
        return;
    }
    jobsAvailable_.notify_all();
}

void ServiceScheduler::abortRequest(LayerTilesRequest::Ptr const& request)
{
    if (!request || request->isDone()) {
        return;
    }
    request->setStatus(RequestStatus::Aborted);
    {
        std::lock_guard lock(mutex_);
        requests_.remove_if([&](auto const& queued) { return queued == request; });
        for (auto& [_, job] : inFlightTiles_) {
            std::erase(job->waitingRequests, request);
        }
    }
    jobsAvailable_.notify_all();
}

void ServiceScheduler::enqueueJob(std::unique_ptr<ServiceJob> job)
{
    if (!job) {
        return;
    }
    bool discard = false;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            discard = true;
        }
        else {
            queuedJobs_.push_back(std::move(job));
        }
    }
    if (discard) {
        job->discard();
        return;
    }
    jobsAvailable_.notify_one();
}

void ServiceScheduler::abortFilterJobs(FeatureLayerFilterTilesRequest::Ptr const& request)
{
    std::vector<std::unique_ptr<ServiceJob>> discarded;
    {
        std::lock_guard lock(mutex_);
        for (auto job = queuedJobs_.begin(); job != queuedJobs_.end();) {
            if ((*job)->filterOwner() != request) {
                ++job;
                continue;
            }
            discarded.push_back(std::move(*job));
            job = queuedJobs_.erase(job);
        }
    }
    for (auto& job : discarded) {
        job->discard();
    }
    jobsAvailable_.notify_all();
}

void ServiceScheduler::addFilterMemoryTracker(std::shared_ptr<FilterMemoryTracker> const& tracker)
{
    if (!tracker) {
        return;
    }
    std::lock_guard lock(mutex_);
    filterMemoryTrackers_.push_back(tracker);
}

void ServiceScheduler::invalidateMap(std::string const& mapId)
{
    std::vector<LayerTilesRequest::Ptr> abortedRequests;
    std::vector<std::unique_ptr<ServiceJob>> discarded;
    {
        std::lock_guard lock(mutex_);
        ++mapEpochs_[mapId];

        for (auto request = requests_.begin(); request != requests_.end();) {
            if (*request && (*request)->mapId_ == mapId) {
                abortedRequests.push_back(*request);
                request = requests_.erase(request);
            }
            else {
                ++request;
            }
        }
        for (auto job = inFlightTiles_.begin(); job != inFlightTiles_.end();) {
            if (job->first.mapId_ != mapId) {
                ++job;
                continue;
            }
            abortedRequests.insert(
                abortedRequests.end(),
                job->second->waitingRequests.begin(),
                job->second->waitingRequests.end());
            job->second->waitingRequests.clear();
            job = inFlightTiles_.erase(job);
        }
        for (auto job = queuedJobs_.begin(); job != queuedJobs_.end();) {
            if ((*job)->mapId() != mapId) {
                ++job;
                continue;
            }
            discarded.push_back(std::move(*job));
            job = queuedJobs_.erase(job);
        }

        // A running tile publishes under this mutex and checks the same epoch,
        // so no stale result can enter the cache after invalidation returns.
        cache_->invalidateMap(mapId);
    }

    std::ranges::sort(abortedRequests, {}, [](auto const& request) { return request.get(); });
    abortedRequests
        .erase(std::unique(abortedRequests.begin(), abortedRequests.end()), abortedRequests.end());
    for (auto const& request : abortedRequests) {
        if (request && !request->isDone()) {
            request->setStatus(RequestStatus::Aborted);
        }
    }
    for (auto& job : discarded) {
        job->discard();
    }
    jobsAvailable_.notify_all();
}

void ServiceScheduler::stop() noexcept
{
    std::vector<std::unique_ptr<ServiceJob>> discarded;
    std::vector<LayerTilesRequest::Ptr> abortedRequests;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
        discarded.reserve(queuedJobs_.size());
        while (!queuedJobs_.empty()) {
            discarded.push_back(std::move(queuedJobs_.front()));
            queuedJobs_.pop_front();
        }
        abortedRequests.assign(requests_.begin(), requests_.end());
        requests_.clear();
        for (auto& [_, job] : inFlightTiles_) {
            abortedRequests.insert(
                abortedRequests.end(),
                job->waitingRequests.begin(),
                job->waitingRequests.end());
            job->waitingRequests.clear();
        }
    }

    for (auto& job : discarded) {
        job->discard();
    }
    for (auto const& request : abortedRequests) {
        if (request && !request->isDone()) {
            try {
                request->setStatus(RequestStatus::Aborted);
            }
            catch (...) {
                // Shutdown must still join the worker pool if a client callback fails.
                try {
                    log().error("LayerTilesRequest completion callback failed during shutdown.");
                }
                catch (...) {
                    // Logging failure cannot be recovered during noexcept shutdown.
                }
            }
        }
    }
    jobsAvailable_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

void ServiceScheduler::workerLoop()
{
    while (true) {
        std::unique_ptr<ServiceJob> job;
        {
            std::unique_lock lock(mutex_);
            jobsAvailable_.wait(lock, [this] { return stopping_ || hasRunnableWorkLocked(); });
            if (stopping_) {
                return;
            }
            job = takeNextJobLocked(lock);
            if (!job) {
                continue;
            }
            if (job->cancelled()) {
                lock.unlock();
                job->discard();
                continue;
            }
            ++runningJobs_;
            if (job->kind() == ServiceJobKind::FilterEvaluation) {
                ++runningFilterJobs_;
            }
        }

        auto source = job->sourceAffinity();
        job->run();

        {
            std::lock_guard lock(mutex_);
            if (runningJobs_ > 0) {
                --runningJobs_;
            }
            if (job->kind() == ServiceJobKind::FilterEvaluation && runningFilterJobs_ > 0) {
                --runningFilterJobs_;
            }
            if (source && source->running > 0) {
                --source->running;
            }
        }
        jobsAvailable_.notify_all();
    }
}

bool ServiceScheduler::hasRunnableWorkLocked() const
{
    if (!queuedJobs_.empty()) {
        return true;
    }
    for (auto const& source : sources_) {
        if (!source->enabled || source->running >= source->limit) {
            continue;
        }
        if (nextCandidateLocked(*source)) {
            return true;
        }
    }
    return false;
}

std::unique_ptr<ServiceJob> ServiceScheduler::takeNextJobLocked(std::unique_lock<std::mutex>& lock)
{
    while (!stopping_) {
        if (preferDerivedJob_ && !queuedJobs_.empty()) {
            auto job = std::move(queuedJobs_.front());
            queuedJobs_.pop_front();
            preferDerivedJob_ = false;
            if (job->cancelled()) {
                lock.unlock();
                job->discard();
                lock.lock();
                continue;
            }
            return job;
        }

        if (auto tileJob = takeNextTileJobLocked(lock)) {
            preferDerivedJob_ = true;
            return tileJob;
        }

        if (!queuedJobs_.empty()) {
            auto job = std::move(queuedJobs_.front());
            queuedJobs_.pop_front();
            preferDerivedJob_ = false;
            if (job->cancelled()) {
                lock.unlock();
                job->discard();
                lock.lock();
                continue;
            }
            return job;
        }
        return {};
    }
    return {};
}

std::unique_ptr<ServiceJob>
ServiceScheduler::takeNextTileJobLocked(std::unique_lock<std::mutex>& lock)
{
    while (!stopping_) {
        if (sources_.empty()) {
            return {};
        }

        auto const sourceCount = sources_.size();
        auto handledInline = false;
        for (size_t offset = 0; offset < sourceCount; ++offset) {
            auto const index = (nextSourceIndex_ + offset) % sourceCount;
            // Keep a shared handle, not a reference into sources_: cache
            // callbacks run without the lock and may mutate the source list.
            auto source = sources_[index];
            if (!source->enabled || source->running >= source->limit) {
                continue;
            }

            auto candidate = nextCandidateLocked(*source);
            if (!candidate) {
                continue;
            }
            auto request = candidate->request;
            request->nextTileIndex_ = candidate->nextTileIndex;
            request->tileKeysNotStarted_.erase(candidate->tileKey);
            requests_.splice(requests_.end(), requests_, candidate->requestIt);
            nextSourceIndex_ = (index + 1) % sourceCount;

            auto cached = cache_->getTileLayer(candidate->tileKey, *source->source->info);
            if (cached.tile) {
                log().debug("Serving cached tile: {}", candidate->tileKey.toString());
                lock.unlock();
                request->notifyResult(cached.tile);
                lock.lock();
                removeCompletedRequestsLocked();
                handledInline = true;
                break;
            }

            if (auto inFlight = inFlightTiles_.find(candidate->tileKey);
                inFlight != inFlightTiles_.end()) {
                log().debug("Joining tile with job in progress: {}", candidate->tileKey.toString());
                auto const state = inFlight->second->loadStatus;
                inFlight->second->waitingRequests.push_back(request);
                // Load-state callbacks are external and may mutate datasource
                // or request state, so retain no container references here.
                lock.unlock();
                request->notifyLoadState(candidate->tileKey, state);
                lock.lock();
                removeCompletedRequestsLocked();
                handledInline = true;
                break;
            }

            auto state = std::make_shared<TileLoadState>();
            state->tileKey = candidate->tileKey;
            state->waitingRequests = {request};
            state->cacheExpiredAt = cached.expiredAt;
            state->mapEpoch = mapEpochs_[candidate->tileKey.mapId_];
            attachMatchingRequestsLocked(
                request,
                *source,
                candidate->tileKey,
                state->waitingRequests);
            inFlightTiles_.emplace(state->tileKey, state);
            ++source->running;
            removeCompletedRequestsLocked();
            log().debug("Working on tile: {}", state->tileKey.toString());
            return std::make_unique<TileLoadJob>(*this, source, std::move(state));
        }

        if (!handledInline) {
            removeCompletedRequestsLocked();
            return {};
        }
        // Restart with a fresh source count/cursor because the callback above
        // ran without the scheduler lock and may have changed either one.
    }
    return {};
}

std::optional<ServiceScheduler::Candidate>
ServiceScheduler::nextCandidateLocked(SourceConcurrency const& source) const
{
    for (auto request = requests_.begin(); request != requests_.end(); ++request) {
        if (!requestMatchesSourceLocked(*request, source)) {
            continue;
        }
        auto pendingIndex = nextPendingTileKeyLocked(**request);
        if (!pendingIndex) {
            continue;
        }
        return Candidate{
            .requestIt = request,
            .request = *request,
            .tileKey = (*request)->resolvedTileKeys_[*pendingIndex],
            .nextTileIndex = *pendingIndex,
        };
    }
    return {};
}

std::optional<size_t>
ServiceScheduler::nextPendingTileKeyLocked(LayerTilesRequest const& request) const
{
    auto index = request.nextTileIndex_;
    while (index < request.resolvedTileKeys_.size()) {
        if (request.tileKeysNotStarted_.contains(request.resolvedTileKeys_[index])) {
            return index;
        }
        ++index;
    }
    return {};
}

bool ServiceScheduler::requestMatchesSourceLocked(
    LayerTilesRequest::Ptr const& request,
    SourceConcurrency const& source) const
{
    if (!request || request->isDone()) {
        return false;
    }
    if (request->sourceId_ && *request->sourceId_ != source.source->sourceId) {
        return false;
    }
    return request->mapId_ == source.source->info->mapId_ &&
        source.source->info->layers_.contains(request->layerId_);
}

void ServiceScheduler::attachMatchingRequestsLocked(
    LayerTilesRequest::Ptr const& selectedRequest,
    SourceConcurrency const& source,
    MapTileKey const& tileKey,
    std::vector<LayerTilesRequest::Ptr>& waitingRequests) const
{
    for (auto const& request : requests_) {
        if (request == selectedRequest || !requestMatchesSourceLocked(request, source)) {
            continue;
        }
        if (request->mapId_ != selectedRequest->mapId_ ||
            request->layerId_ != selectedRequest->layerId_) {
            continue;
        }
        if (request->tileKeysNotStarted_.erase(tileKey) != 0) {
            waitingRequests.push_back(request);
        }
    }
}

void ServiceScheduler::removeCompletedRequestsLocked()
{
    requests_.remove_if(
        [](auto const& request)
        { return !request || request->isDone() || request->tileKeysNotStarted_.empty(); });
}

void ServiceScheduler::completeTileJob(TileLoadState const& job, TileLayer::Ptr const& layer)
{
    std::vector<LayerTilesRequest::Ptr> notifyRequests;
    {
        std::lock_guard lock(mutex_);
        if (job.mapEpoch == mapEpochs_[job.tileKey.mapId_]) {
            cache_->putTileLayer(layer);
            notifyRequests = job.waitingRequests;
        }
        if (auto inFlight = inFlightTiles_.find(job.tileKey);
            inFlight != inFlightTiles_.end() && inFlight->second.get() == &job)
        {
            inFlightTiles_.erase(inFlight);
        }
    }
    for (auto const& request : notifyRequests) {
        if (request) {
            request->notifyResult(layer);
        }
    }
    jobsAvailable_.notify_all();
}

void ServiceScheduler::failTileJob(TileLoadState const& job)
{
    std::vector<LayerTilesRequest::Ptr> failedRequests;
    {
        std::lock_guard lock(mutex_);
        if (auto inFlight = inFlightTiles_.find(job.tileKey);
            inFlight != inFlightTiles_.end() && inFlight->second.get() == &job)
        {
            inFlightTiles_.erase(inFlight);
        }
        failedRequests = job.waitingRequests;
        for (auto const& failed : failedRequests) {
            requests_.remove_if([&](auto const& request) { return request == failed; });
        }
        for (auto& [_, inFlight] : inFlightTiles_) {
            std::erase_if(
                inFlight->waitingRequests,
                [&](auto const& request)
                { return std::ranges::find(failedRequests, request) != failedRequests.end(); });
        }
    }
    for (auto const& request : failedRequests) {
        if (request && !request->isDone()) {
            request->setStatus(RequestStatus::Aborted);
        }
    }
    jobsAvailable_.notify_all();
}

void ServiceScheduler::notifyTileLoadState(TileLoadState& job, TileLayer::LoadState state)
{
    std::vector<LayerTilesRequest::Ptr> waiting;
    {
        std::lock_guard lock(mutex_);
        job.loadStatus = state;
        waiting = job.waitingRequests;
    }
    for (auto const& request : waiting) {
        if (request) {
            request->notifyLoadState(job.tileKey, state);
        }
    }
}

ServiceSchedulerStatistics ServiceScheduler::statistics() const
{
    std::lock_guard lock(mutex_);
    auto queuedFilterJobs = size_t{0};
    for (auto const& job : queuedJobs_) {
        queuedFilterJobs += job->kind() == ServiceJobKind::FilterEvaluation ? 1 : 0;
    }
    return ServiceSchedulerStatistics{
        .workerCount = workers_.size(),
        .runningJobs = runningJobs_,
        .activeTileRequests = requests_.size(),
        .inFlightTileJobs = inFlightTiles_.size(),
        .queuedFilterJobs = queuedFilterJobs,
        .runningFilterJobs = runningFilterJobs_,
    };
}

std::vector<SourceConcurrencyStatistics> ServiceScheduler::sourceStatistics() const
{
    std::lock_guard lock(mutex_);
    std::vector<SourceConcurrencyStatistics> result;
    result.reserve(sources_.size());
    for (auto const& source : sources_) {
        result.push_back(SourceConcurrencyStatistics{
            .sourceId = source->source->sourceId,
            .mapId = source->source->info->mapId_,
            .limit = source->limit,
            .running = source->running,
        });
    }
    return result;
}

void ServiceScheduler::collectMemoryUsage(
    MemoryUsageBreakdown& scheduler,
    MemoryUsageBreakdown& telemetry,
    std::vector<std::shared_ptr<FilterMemoryTracker>>& filterTrackers)
{
    auto accountMapTileKey = [](MemoryUsageBreakdown& target, MapTileKey const& key)
    {
        target.add("map-tile-key-strings", stringMemoryUsage(key.mapId_));
        target.add("map-tile-key-strings", stringMemoryUsage(key.layerId_));
    };
    auto accountLayerRequest = [&](LayerTilesRequest const& request)
    {
        scheduler.add("request-objects", {sizeof(LayerTilesRequest), sizeof(LayerTilesRequest)});
        scheduler.add("request-strings", stringMemoryUsage(request.mapId_));
        scheduler.add("request-strings", stringMemoryUsage(request.layerId_));
        if (request.sourceId_) {
            scheduler.add("request-strings", stringMemoryUsage(*request.sourceId_));
        }
        scheduler.add("request-tile-ids", vectorMemoryUsage(request.tileIds_));
        scheduler.add(
            "priority-tile-index",
            {
                request.priorityTileIds_.size() * sizeof(TileId),
                request.priorityTileIds_.size() * (sizeof(TileId) + 3 * sizeof(void*)),
            });
        scheduler.add("resolved-tile-keys", vectorMemoryUsage(request.resolvedTileKeys_));
        for (auto const& key : request.resolvedTileKeys_) {
            accountMapTileKey(scheduler, key);
        }
        scheduler.add(
            "not-started-tile-index",
            {
                request.tileKeysNotStarted_.size() * sizeof(MapTileKey),
                request.tileKeysNotStarted_.size() * (sizeof(MapTileKey) + 3 * sizeof(void*)),
            });
        for (auto const& key : request.tileKeysNotStarted_) {
            accountMapTileKey(scheduler, key);
        }
        scheduler.add(
            "feature-id-restrictions",
            {
                request.featureIdsByTile_.size() *
                    sizeof(decltype(request.featureIdsByTile_)::value_type),
                request.featureIdsByTile_.size() *
                    (sizeof(decltype(request.featureIdsByTile_)::value_type) + 3 * sizeof(void*)),
            });
        for (auto const& [_, ids] : request.featureIdsByTile_) {
            scheduler.add("feature-id-vectors", stringVectorMemoryUsage(ids));
        }
    };

    std::lock_guard lock(mutex_);
    scheduler.add(
        "request-list",
        {
            requests_.size() * sizeof(LayerTilesRequest::Ptr),
            requests_.size() * (sizeof(LayerTilesRequest::Ptr) + 2 * sizeof(void*)),
        });
    std::unordered_set<LayerTilesRequest const*> measuredRequests;
    for (auto const& request : requests_) {
        if (request && measuredRequests.insert(request.get()).second) {
            accountLayerRequest(*request);
        }
    }
    scheduler.add(
        "in-flight-job-index",
        {
            inFlightTiles_.size() * sizeof(decltype(inFlightTiles_)::value_type),
            inFlightTiles_.size() *
                (sizeof(decltype(inFlightTiles_)::value_type) + 3 * sizeof(void*)),
        });
    for (auto const& [key, job] : inFlightTiles_) {
        accountMapTileKey(scheduler, key);
        scheduler.add("in-flight-job-objects", {sizeof(TileLoadState), sizeof(TileLoadState)});
        accountMapTileKey(scheduler, job->tileKey);
        scheduler.add("job-waiters", vectorMemoryUsage(job->waitingRequests));
        for (auto const& request : job->waitingRequests) {
            if (request && measuredRequests.insert(request.get()).second) {
                accountLayerRequest(*request);
            }
        }
    }
    scheduler.add(
        "derived-job-queue",
        {
            queuedJobs_.size() * sizeof(std::unique_ptr<ServiceJob>),
            queuedJobs_.size() * sizeof(std::unique_ptr<ServiceJob>),
        });
    for (auto const& job : queuedJobs_) {
        scheduler.add("derived-job-map-ids", stringMemoryUsage(std::string(job->mapId())));
    }
    scheduler.add("worker-handles", vectorMemoryUsage(workers_));
    scheduler.add("source-permit-handles", vectorMemoryUsage(sources_));
    scheduler.add(
        "source-permit-objects",
        {
            sources_.size() * sizeof(SourceConcurrency),
            sources_.size() * sizeof(SourceConcurrency),
        });
    scheduler.add(
        "map-epochs",
        {
            mapEpochs_.size() * sizeof(decltype(mapEpochs_)::value_type),
            mapEpochs_.size() * (sizeof(decltype(mapEpochs_)::value_type) + 3 * sizeof(void*)),
        });
    for (auto const& [mapId, _] : mapEpochs_) {
        scheduler.add("map-epoch-ids", stringMemoryUsage(mapId));
    }

    auto tracker = filterMemoryTrackers_.begin();
    while (tracker != filterMemoryTrackers_.end()) {
        auto active = tracker->lock();
        if (!active) {
            tracker = filterMemoryTrackers_.erase(tracker);
            continue;
        }
        filterTrackers.push_back(std::move(active));
        ++tracker;
    }
    telemetry.add("filter-tracker-handles", vectorMemoryUsage(filterMemoryTrackers_));
}

}  // namespace mapget::detail
