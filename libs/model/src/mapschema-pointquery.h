#pragma once

#include "mapschema-impl.h"

namespace mapget
{
/** Private type proof for the shared expression cache. An arbitrary SIMFIL pool
 * with the same caller-supplied identity string must never authorize a cache hit. */
bool isPublishedPointQueryPool(simfil::SchemaPool const*);

/** A point query needs the raw graph reachable through its host and geometry, not every
 * layer's attribute contexts. Import just that closure into an independent SIMFIL pool.
 * Its views retain this owner without retaining the datasource or mutating its dictionary. */
class MapSchema::Impl::PointQueryPool final : public simfil::SchemaPool
{
public:
    friend class PointBuilder;
    friend bool isPublishedPointQueryPool(simfil::SchemaPool const* pool)
    {
        auto view = dynamic_cast<PointQueryPool const*>(pool);
        return view && view->finalized();
    }

    // Generic graph import materializes owned ModelNodes. Call only after this
    // object has its shared owner, never from the shared_ptr's constructor.
    void importHost(MapSchema::Impl const& source, TypeId feature, TypeId geometry)
    {
        sourceIdentity_ = source.pool.schemaId();
        auto const& owner = source.pool;
        auto unused = addSchema(Kind::Unknown);
        // A nonzero reuse entry skips a source record. No reachable edge is allowed
        // to target this placeholder; check the whole closure before importing it.
        std::vector<TypeId> reuse(owner.typeCount() + 1, unused);
        reuse[0] = 0;
        std::vector<TypeId> pending{feature, geometry};
        while (!pending.empty()) {
            auto id = pending.back();
            pending.pop_back();
            if (!id || !reuse.at(id))
                continue;
            reuse[id] = 0;
            auto type = owner.schemaType(id);
            auto enqueue = [&](auto ids)
            {
                pending.insert(pending.end(), ids.begin(), ids.end());
            };
            pending.push_back(type.baseType().id());
            enqueue(type.elements());
            enqueue(type.alternatives());
            type.forEachDirectField([&](auto const& field) { enqueue(field.types()); });
        }
        for (size_t i = 1; i < reuse.size(); ++i) {
            auto kind = simfil::SchemaType::kindNameId(owner.schemaType(TypeId(i)).kind());
            if (!strings()->resolve(kind))
                strings()->addStaticKey(kind, std::string(source.name(kind)));
            if (source.data(TypeId(i)).flags & PointQueryFlag)
                canonical_.emplace(source.text(source.data(TypeId(i)).key), TypeId(i));
        }
        imported_ = importGraph(owner, reuse);
        resultTypes_.resize(typeCount() + 1);
        for (size_t i = 1; i < imported_.size(); ++i)
            if (imported_[i] != unused)
                resultTypes_[imported_[i]] = TypeId(i);
    }

    TypeId raw(TypeId id) const
    {
        auto local = imported_.at(id);
        if (id && !resultTypes_.at(local))
            throw std::logic_error("Raw point type outside the host query closure.");
        return local;
    }

    /** Results persisted in subsets must use the source artifact, never this pool's
     * private namespace. Unnamed occurrence/index scaffolding has no canonical type. */
    void bindResult(TypeId id, std::string_view key, TypeId fallback)
    {
        if (id != resultTypes_.size())
            throw std::logic_error("Noncontiguous point query type allocation.");
        auto found = canonical_.find(key);
        resultTypes_.push_back(found == canonical_.end() ? fallback : found->second);
    }

    void publish(TypeId root, std::string key)
    {
        root_ = root;
        // Includes borrowed keys into the source; the frozen query pool needs no backlink.
        canonical_.clear();
        std::vector<TypeId>().swap(imported_);
        finalize(sourceIdentity_ + ":" + key);
        usage_ = schemaMemoryUsage() + memoryUsageStats().total() + strings()->memoryUsage() +
            vectorMemoryUsage(resultTypes_) + stringMemoryUsage(sourceIdentity_);
    }

    TypeId root_ = 0;
    std::string sourceIdentity_;
    std::vector<TypeId> resultTypes_;
    simfil::MemoryUsage usage_;

private:
    std::vector<TypeId> imported_;
    std::map<std::string_view, TypeId, std::less<>> canonical_;
};

}  // namespace mapget
