#include "service-incoming.h"
#include "mapget/model/feature.h"
#include "mapget/model/featureid.h"
#include "mapget/model/layerschema.h"
#include "mapget/service/locate.h"

#include <algorithm>
#include <ostream>
#include <streambuf>

namespace mapget::detail
{

namespace {
/** Measure a serialized snapshot without retaining a second copy of its payload. */
class SnapshotSizeBuffer : public std::streambuf {
public:
    size_t size_ = 0;
protected:
    std::streamsize xsputn(char const*, std::streamsize count) override {
        size_ += static_cast<size_t>(count); return count;
    }
    int_type overflow(int_type value) override {
        if (!traits_type::eq_int_type(value, traits_type::eof())) ++size_;
        return traits_type::not_eof(value);
    }
};
}

bool IncomingRequestExecution::start(Service::Impl& service,
    FeatureLayerFilterTilesRequest::Ptr request, std::optional<AuthHeaders> headers)
{
    auto const& filter = request->filter_;
    auto const& channel = filter.channels_.front();
    if (filter.channels_.size() != 1 || !channel.relation_ || !channel.relation_->incoming_ ||
        channel.scope_ != FeatureLayerFilterScope::Relation || channel.relation_->recursive_ || channel.relation_->mergeTwoway_ ||
        channel.relation_->relationNamePattern_ || channel.rewrite_ || channel.group_ || channel.featureFilter_ ||
        channel.entryFilter_ || !channel.featureFields_.empty() || !channel.entryFields_.empty() ||
        !channel.featureTypes_.empty() || channel.geometryName_ || channel.pointCloudMaxPoints_ ||
        channel.geometryTypes_ != FeatureLayerFilterChannel::AllGeometryTypes ||
        !filter.bindings_.empty() || request->exactRoots_.empty() ||
        request->exactRoots_.size() > 16 || request->tileIds_.size() > 16) {
        request->notifyProgress({{"type", "mapget.filter.status"}, {"state", "Failed"},
            {"error", "Incoming inspection requires one unfiltered relation channel and 1-16 exact roots; recursion, merging, projections and spatial filtering are unsupported."}});
        request->setStatus(RequestStatus::Aborted);
        return false;
    }
    auto state = std::shared_ptr<IncomingRequestExecution>(new IncomingRequestExecution);
    state->service_ = &service;
    state->request_ = request;
    state->headers_ = std::move(headers);
    state->catalogRevision_ = service.getSourceCatalogRevision();
    auto child = std::make_shared<LayerTilesRequest>(request->mapId_, request->layerId_, request->tileIds_);
    child->sourceId_ = request->sourceId_;
    child->onFeatureLayer([state](auto layer) { state->guarded([&] { state->targetLoaded(layer); }); });
    child->onDone_ = [state](auto status) { state->guarded([&] { state->discover(status); }); };
    state->guarded([&] { state->submit({child}); });
    return true;
}

void IncomingRequestExecution::fail(std::string const& message)
{
    if (terminal_.exchange(true) || request_->isCancelled()) return;
    std::vector<LayerTilesRequest::Ptr> children;
    { std::lock_guard lock(request_->childRequestsMutex_); children.swap(request_->childRequests_); }
    try { request_->notifyProgress({{"type", "mapget.filter.status"}, {"state", "Failed"}, {"error", message}}); }
    catch (...) { /* Completion must still release the request when a consumer callback throws. */ }
    request_->setStatus(RequestStatus::Aborted);
    for (auto const& child : children) if (!child->isDone()) service_->scheduler_.abortRequest(child);
}

void IncomingRequestExecution::submit(std::vector<LayerTilesRequest::Ptr> const& jobs)
{
    {
        std::lock_guard lock(request_->childRequestsMutex_);
        if (terminal_ || request_->isCancelled()) return;
        for (auto const& job : jobs) {
            job->setWorkAdmissionGate(request_->workAdmissionGate_);
            request_->childRequests_.push_back(job);
        }
    }
    for (auto const& job : jobs) {
        if (terminal_ || request_->isCancelled()) break;
        service_->requestTiles({job}, headers_);
        if (terminal_ || request_->isCancelled()) service_->scheduler_.abortRequest(job);
    }
}

void IncomingRequestExecution::dependency(PartitionFeatureLayer const& layer)
{
    for (auto const& warning : layer.warnings()) issues_.push_back(warning);
    if (auto error = layer.error()) issues_.push_back(*error);
    if (auto ttl = layer.ttl(); ttl && ttl->count() > 0) {
        auto expires = std::chrono::duration_cast<std::chrono::milliseconds>(
            (layer.timestamp() + *ttl).time_since_epoch()).count();
        if (!expiresAtMs_ || expires < *expiresAtMs_) expiresAtMs_ = expires;
    }
}

bool IncomingRequestExecution::retain(PartitionFeatureLayer::Ptr const& layer)
{
    if (models_.contains(layer->id())) return true;
    auto bytes = layer->memoryUsage().total().allocatedBytes;
    constexpr size_t MaxRetainedBytes = 64 * 1024 * 1024;
    if (models_.size() >= IncomingRelationCollection::MaxSnapshots ||
        bytes > MaxRetainedBytes - retainedBytes_) {
        issues_.push_back("Incoming inspection reached its snapshot memory limit.");
        return false;
    }
    retainedBytes_ += bytes;
    models_.emplace(layer->id(), layer);
    return true;
}

void IncomingRequestExecution::targetLoaded(PartitionFeatureLayer::Ptr const& layer)
{
    if (terminal_ || request_->isCancelled()) return;
    std::lock_guard lock(mutex_);
    dependency(*layer);
    if (layer->error() || !retain(layer)) return;
    for (auto const& root : request_->exactRoots_) {
        if (root.partitionId_ != layer->partitionId()) continue;
        auto target = root.canonicalFeatureId_.empty()
            ? layer->find(root.typeId_, root.featureId_) : layer->find(root.canonicalFeatureId_);
        if (!target) { issues_.push_back("Selected incoming target is no longer available."); continue; }
        ++resolvedRoots_;
        auto id = target->id()->toString();
        if (!selected_[layer->id()].insert(id).second) continue;
        IncomingRelationCollection collection;
        collection.target_ = layer->id();
        collection.featureId_ = id;
        collections_.push_back(std::move(collection));
        targets_.push_back(target);
    }
}

void IncomingRequestExecution::discover(RequestStatus status)
{
    if (terminal_ || request_->isCancelled()) return;
    if (status != RequestStatus::Success || targets_.empty() || resolvedRoots_ != request_->exactRoots_.size()) {
        fail("Incoming inspection could not resolve every selected target snapshot.");
        return;
    }
    std::vector<LayerTilesRequest::Ptr> jobs;
    std::set<MapPartitionKey> scheduled;
    std::set<std::string> ownerLayers;
    std::set<size_t> supportedTargets;
    auto addons = service_->dataSources_.addOnSources();
    for (auto const& source : service_->dataSources_.snapshot()) {
        if (source->info->mapId_ != request_->mapId_ || source->info->isAddOn_ ||
            (headers_ && !source->dataSource->isDataSourceAuthorized(*headers_))) continue;
        for (auto const& [layerId, info] : source->info->layers_) {
            if (layerId == request_->layerId_ && request_->sourceId_ && source->sourceId != *request_->sourceId_)
                continue;
            // Match normal primary-source precedence and the selected target's explicit owner.
            if (!ownerLayers.insert(layerId).second) continue;
            auto schema = info->layerSchema();
            if (info->type_ != LayerType::Features || !info->canRead_) continue;
            auto declarations = schema ? schema->relationDeclarations() : std::vector<LayerSchema::RelationDeclaration>{};
            bool unknownAddition = false;
            for (auto const& addon : addons) {
                if (addon->info->mapId_ != request_->mapId_) continue;
                auto found = addon->info->layers_.find(layerId);
                if (found == addon->info->layers_.end()) continue;
                auto addedSchema = found->second->layerSchema();
                auto added = addedSchema ? addedSchema->relationDeclarations() : std::vector<LayerSchema::RelationDeclaration>{};
                if (!addedSchema || !addedSchema->hasRelationDeclarations()) {
                    unknownAddition = true;
                } else declarations.insert(declarations.end(), added.begin(), added.end());
            }
            for (size_t i = 0; i < targets_.size(); ++i) {
                auto const& key = collections_[i].target_;
                if (key.partitionId_.kind() != info->partitionKind_) continue;
                auto type = targets_[i]->id()->typeId();
                if (!std::ranges::any_of(declarations, [&](auto const& d) {
                        return d.incoming_ && std::ranges::find(d.targetTypes_, type) != d.targetTypes_.end();
                    })) continue;
                supportedTargets.insert(i);
                for (auto const& declaration : declarations)
                    if (declaration.incoming_ && !declaration.complete_ &&
                        std::ranges::find(declaration.targetTypes_, type) != declaration.targetTypes_.end())
                        issues_.push_back("An incoming relation family has unknown conversion coverage.");
                if (unknownAddition) issues_.push_back("Incoming coverage of a composite provider is not established.");
                MapPartitionKey provider(LayerType::Features, key.mapId_, layerId, key.partitionId_);
                if (!scheduled.insert(provider).second) continue;
                if (scheduled.size() > IncomingRelationCollection::MaxSnapshots) {
                    issues_.push_back("Incoming inspection reached its provider limit.");
                    continue;
                }
                if (auto loaded = models_.find(provider); loaded != models_.end()) {
                    scan(loaded->second, declarations, source->dataSource);
                    continue;
                }
                auto child = std::make_shared<LayerTilesRequest>(key.mapId_, layerId,
                    std::vector<PartitionId>{key.partitionId_});
                child->sourceId_ = source->sourceId;
                child->onFeatureLayer([self = shared_from_this(), declarations, provider = source->dataSource](auto layer) {
                    self->guarded([&] { self->scan(layer, declarations, provider); });
                });
                child->onDone_ = [self = shared_from_this()](auto result) { self->guarded([&] { self->providerDone(result); }); };
                jobs.push_back(std::move(child));
            }
        }
    }
    if (supportedTargets.size() != targets_.size()) {
        fail("Incoming inspection is unsupported for a selected feature: no eligible provider declarations.");
        return;
    }
    { std::lock_guard lock(mutex_); pending_ = jobs.size(); }
    if (jobs.empty()) finish();
    else submit(jobs);
}

void IncomingRequestExecution::scan(PartitionFeatureLayer::Ptr const& layer,
    std::vector<LayerSchema::RelationDeclaration> const& declarations, DataSource::Ptr const& provider)
{
    if (terminal_ || request_->isCancelled()) return;
    { std::lock_guard lock(mutex_); dependency(*layer); }
    if (layer->error()) return;
    std::map<std::string, std::vector<LocateCandidate>> locations;
    auto issue = [&](std::string message) { std::lock_guard lock(mutex_); issues_.push_back(std::move(message)); };
    for (auto const& source : *layer) {
        if (terminal_ || request_->isCancelled()) return;
        for (uint32_t ordinal = 0; ordinal < source->numRelations(); ++ordinal) {
            if (terminal_ || request_->isCancelled()) return;
            auto relation = source->getRelation(ordinal);
            auto reference = relation->target();
            for (size_t i = 0; i < targets_.size(); ++i) {
                auto const& target = targets_[i];
                if (layer->partitionId() != target->model().partitionId() ||
                    !std::ranges::any_of(declarations, [&](auto const& d) {
                        return d.permitsIncoming(source->id()->typeId(), relation->name(), target->typeId());
                    }) || !reference || reference->mapId() != target->model().mapId() ||
                    reference->typeId() != target->typeId()) continue;
                bool matches = relationTargetsFeature(*relation, *target);
                auto composition = target->model().layerInfo()->matchingFeatureIdCompositionIndex(
                    reference->typeId(), reference->keyValuePairs(), false);
                if (!composition) { issue("An incoming reference uses an unknown ID composition."); continue; }
                if (!matches && *composition != 0) {
                    // Resolve secondary IDs only against the already loaded target model.
                    // No candidate is allowed to schedule a partition or layer fetch here.
                    LocateRequest query(reference->mapId(), std::string(reference->typeId()),
                                        castToKeyValue(reference->keyValuePairs()));
                    query.layerId_ = target->model().layerInfo()->layerId_;
                    query.partition_ = target->model().partitionId();
                    auto key = query.serialize().dump();
                    if (!locations.contains(key)) {
                        if (locations.size() >= IncomingRelationCollection::MaxRelations) {
                            issue("Incoming inspection reached its secondary-reference limit."); return;
                        }
                        locations.emplace(key, provider->locate(query));
                    }
                    auto const& candidates = locations.at(key);
                    std::set<std::string> selected;
                    bool unresolved = candidates.empty();
                    for (auto const& candidate : candidates) {
                        if (candidate.tileKey_.mapId_ != target->model().mapId() ||
                            candidate.tileKey_.partitionId_ != target->model().partitionId()) continue;
                        if (candidate.tileKey_ != target->model().id()) { unresolved = true; continue; }
                        auto resolved = resolveLocateCandidate(candidate, target->model(),
                            [this] { return terminal_ || request_->isCancelled(); });
                        if (!resolved) { unresolved = true; continue; }
                        for (auto const& feature : *resolved) selected.insert(feature->id()->toString());
                    }
                    if (unresolved || selected.size() > 1) {
                        issue("An incoming secondary reference could not be resolved unambiguously in the selected partition.");
                        continue;
                    }
                    matches = selected.size() == 1 && *selected.begin() == target->id()->toString();
                }
                if (!matches) continue;
                std::lock_guard lock(mutex_);
                auto& collection = collections_[i];
                if (collection.relations_.size() == IncomingRelationCollection::MaxRelations) {
                    issues_.push_back("Incoming inspection reached its relation limit."); return;
                }
                if (!retain(layer)) return;
                auto id = source->id()->toString();
                selected_[layer->id()].insert(id);
                collection.relations_.push_back({layer->id(), std::move(id), ordinal,
                    std::string(relation->name()), target->id()->toString()});
            }
        }
    }
}

void IncomingRequestExecution::providerDone(RequestStatus status)
{
    if (terminal_ || request_->isCancelled()) return;
    bool done;
    {
        std::lock_guard lock(mutex_);
        if (status != RequestStatus::Success) issues_.push_back("An incoming provider could not be loaded.");
        done = --pending_ == 0;
    }
    if (done) finish();
}

void IncomingRequestExecution::finish()
{
    if (terminal_ || request_->isCancelled()) return;
    if (catalogRevision_ != service_->getSourceCatalogRevision()) {
        fail("Datasource catalog changed during incoming discovery; retry required."); return;
    }
    std::sort(issues_.begin(), issues_.end());
    issues_.erase(std::unique(issues_.begin(), issues_.end()), issues_.end());
    try {
        constexpr size_t MaxOutputBytes = 64 * 1024 * 1024;
        size_t remaining = MaxOutputBytes;
        size_t remainingWireBytes = MaxOutputBytes;
        std::map<MapPartitionKey, PartitionFeatureLayer::Ptr> snapshots;
        // Prepare targets first so output limiting can never leave a successful orphan manifest.
        std::vector<MapPartitionKey> order;
        for (auto const& collection : collections_)
            if (std::ranges::find(order, collection.target_) == order.end()) order.push_back(collection.target_);
        for (auto const& [key, _] : selected_)
            if (std::ranges::find(order, key) == order.end()) order.push_back(key);
        for (auto const& key : order) {
            if (terminal_ || request_->isCancelled()) return;
            auto const& ids = selected_.at(key);
            std::vector<std::string> features(ids.begin(), ids.end());
            auto snapshot = cloneInspectionFeatures(models_.at(key), features, remaining,
                [this] { return terminal_ || request_->isCancelled(); });
            if (snapshot->size() == 0) { issues_.push_back("Incoming inspection reached its output limit."); continue; }
            auto bytes = snapshot->memoryUsage().total().allocatedBytes;
            if (bytes > remaining) { issues_.push_back("Incoming inspection reached its output limit."); continue; }
            SnapshotSizeBuffer counter;
            std::ostream output(&counter);
            auto written = snapshot->write(output);
            if (!written) throw std::runtime_error(written.error().message);
            if (counter.size_ > remainingWireBytes) {
                issues_.push_back("Incoming inspection reached its serialized output limit."); continue;
            }
            remainingWireBytes -= counter.size_;
            remaining -= bytes;
            for (auto const& warning : snapshot->warnings()) issues_.push_back(warning);
            snapshots.emplace(key, snapshot);
        }
        for (auto const& collection : collections_)
            if (!snapshots.contains(collection.target_) || !snapshots.at(collection.target_)->find(collection.featureId_)) {
                fail("Incoming inspection target exceeds the output budget."); return;
            }
        for (auto& collection : collections_) {
            std::erase_if(collection.relations_, [&](auto const& ref) {
                return !snapshots.contains(ref.source_) || !snapshots.at(ref.source_)->find(ref.featureId_);
            });
        }
        std::sort(issues_.begin(), issues_.end());
        issues_.erase(std::unique(issues_.begin(), issues_.end()), issues_.end());
        for (auto const& [_, snapshot] : snapshots) {
            if (terminal_ || request_->isCancelled()) return;
            request_->notifyFeatureSnapshot(snapshot);
        }
        for (auto& collection : collections_) {
            if (terminal_ || request_->isCancelled()) break;
            collection.snapshots_.push_back(collection.target_);
            std::sort(collection.relations_.begin(), collection.relations_.end(), [](auto const& a, auto const& b) {
                return std::tie(a.source_, a.featureId_, a.relationIndex_) <
                       std::tie(b.source_, b.featureId_, b.relationIndex_);
            });
            for (auto const& ref : collection.relations_)
                collection.snapshots_.push_back(ref.source_);
            std::sort(collection.snapshots_.begin(), collection.snapshots_.end());
            collection.snapshots_.erase(std::unique(collection.snapshots_.begin(), collection.snapshots_.end()), collection.snapshots_.end());
            collection.issues_ = issues_;
            collection.complete_ = issues_.empty();
            collection.expiresAtMs_ = expiresAtMs_;
            request_->notifyIncomingRelations(collection);
        }
        if (catalogRevision_ != service_->getSourceCatalogRevision()) {
            fail("Datasource catalog changed during incoming discovery; retry required."); return;
        }
        request_->notifyProgress({{"type", "mapget.filter.status"}, {"state", "Success"}});
    } catch (std::exception const& error) {
        fail(error.what());
        return;
    }
    { std::lock_guard lock(request_->childRequestsMutex_); request_->childRequests_.clear(); }
    if (!request_->isCancelled()) request_->setStatus(RequestStatus::Success);
}

} // namespace mapget::detail
