#include "sourcedatareference.h"
#include <cstdint>
#include <stdexcept>
#include <string_view>

#include "featureid.h"
#include "featurelayer.h"
#include "simfil/model/nodes.h"

namespace mapget
{

ValueType SourceDataReferenceCollection::type() const
{
    return ValueType::Array;
}

ModelNode::Ptr SourceDataReferenceCollection::at(int64_t index) const
{
    if (index < 0 || index >= size_ || (offset_ + index) > 0xffffff)
        throw std::out_of_range("Index out of range");

    return ModelNode::Ptr::make(
        model_, ModelNodeAddress{TileFeatureLayer::ColumnId::SourceDataReferences, static_cast<uint32_t>(offset_ + index)});
}

uint32_t SourceDataReferenceCollection::size() const
{
    return size_;
}

bool SourceDataReferenceCollection::iterate(IterCallback const& cb) const
{
    for (auto i = 0u; i < size(); ++i)
        if (!cb(*at(i)))
            return false;
    return true;
}

void SourceDataReferenceCollection::forEachReference(std::function<void(const SourceDataReferenceItem&)> fn) const
{
    const auto& m = model();
    for (auto i = 0u; i < size(); ++i) {
        fn(*m.resolveSourceDataReferenceItem(*at(i)));
    }
}

SourceDataReferenceCollection::SourceDataReferenceCollection(uint32_t offset, uint32_t size, ModelConstPtr pool, ModelNodeAddress a)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(pool, a), offset_(offset), size_(size)
{}

ValueType SourceDataReferenceItem::type() const
{
    return ValueType::Object;
}

uint32_t SourceDataReferenceItem::size() const
{
    return 3;
}

simfil::ModelNode::Ptr SourceDataReferenceItem::at(int64_t i) const
{
    if (i < size())
        return get(keyAt(i));
    return {};
}

ModelNode::Ptr SourceDataReferenceItem::get(const StringId& key) const
{
    switch (key) {
    case StringPool::AddressStr:
        return model_ptr<simfil::ValueNode>::make(static_cast<int64_t>(data_->reference_.address_.u64()), model().shared_from_this());
    case StringPool::LayerIdStr:
        if (auto layerId = model().strings()->resolve(data_->reference_.layerId_))
            return model_ptr<simfil::ValueNode>::make(*layerId, model().shared_from_this());
        return {};
    case StringPool::QualifierStr:
        if (auto qualifier = model().strings()->resolve(data_->qualifier_))
            return model_ptr<simfil::ValueNode>::make(*qualifier, model().shared_from_this());
        return {};
    }
    return {};
}

StringId SourceDataReferenceItem::keyAt(const int64_t index) const
{
    switch (index) {
    case 0: return StringPool::AddressStr;
    case 1: return StringPool::LayerIdStr;
    case 2: return StringPool::QualifierStr;
    }
    return {};
}

bool SourceDataReferenceItem::iterate(const IterCallback& cb) const
{
    for (auto i = 0u; i < size(); ++i)
        if (!cb(*get(at(i))))
            return false;
    return true;
}

std::string_view SourceDataReferenceItem::qualifier() const
{
    if (auto str = model().strings()->resolve(data_->qualifier_))
        return *str;
    return {};
}

std::string_view SourceDataReferenceItem::layerId() const
{
    if (auto str = model().strings()->resolve(data_->reference_.layerId_))
        return *str;
    return {};
}

SourceDataAddress SourceDataReferenceItem::address() const
{
    return data_->reference_.address_;
}

SourceDataReferenceItem::SourceDataReferenceItem(const QualifiedSourceDataReference* const data, const ModelConstPtr pool, const ModelNodeAddress a)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(pool, a), data_(data)
{}

}
