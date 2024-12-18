#include "sourcedata.h"

#include "sourcedatalayer.h"
#include "simfil/model/nodes.h"
#include "sourceinfo.h"

using simfil::ValueType;
using simfil::ModelNode;
using simfil::ModelNodeAddress;

namespace mapget
{

void SourceDataCompoundNode::setSourceDataAddress(SourceDataAddress address)
{
    data_->sourceAddress_ = std::move(address);
}

SourceDataAddress SourceDataCompoundNode::sourceDataAddress() const
{
    return data_->sourceAddress_;
}

void SourceDataCompoundNode::setSchemaName(std::string_view name)
{
    data_->schemaName_ = model().strings()->emplace(name);
}

std::string_view SourceDataCompoundNode::schemaName() const
{
    return model().strings()->resolve(data_->schemaName_).value_or("");
}

simfil::model_ptr<simfil::Object> SourceDataCompoundNode::object()
{
    if (!data_->object_) {
        auto object = model().newObject();
        data_->object_ = object->addr();
        return object;
    }

    return const_cast<const SourceDataCompoundNode*>(this)->object();
}

simfil::model_ptr<const simfil::Object> SourceDataCompoundNode::object() const
{
    if (!data_->object_)
        return {};
    return model().resolveObject(ModelNode::Ptr::make(model_, data_->object_));
}

ValueType SourceDataCompoundNode::type() const
{
    return ValueType::Object;
}

ModelNode::Ptr SourceDataCompoundNode::at(int64_t index) const
{
    if (auto m = object())
        return m->at(index);
    return {};
}

uint32_t SourceDataCompoundNode::size() const
{
    if (auto m = object())
        return m->size();
    return 0;
}

ModelNode::Ptr SourceDataCompoundNode::get(const simfil::StringId& field) const
{
    if (auto m = object())
        return m->get(field);
    return {};
}

simfil::StringId SourceDataCompoundNode::keyAt(int64_t index) const
{
    if (auto m = object())
        return m->keyAt(index);
    return {};
}

bool SourceDataCompoundNode::iterate(IterCallback const& cb) const
{
    for (auto i = 0; i < size(); ++i)
        if (!cb(*at(i)))
            return false;
    return true;
}

SourceDataCompoundNode::SourceDataCompoundNode(Data* data, TileSourceDataLayer::ConstPtr model, simfil::ModelNodeAddress addr)
    : simfil::MandatoryDerivedModelNodeBase<TileSourceDataLayer>(std::move(model), addr), data_(data)
{
    assert(data_);
}

SourceDataCompoundNode::SourceDataCompoundNode(Data* data, TileSourceDataLayer::Ptr model, simfil::ModelNodeAddress addr, size_t initialSize)
    : simfil::MandatoryDerivedModelNodeBase<TileSourceDataLayer>(model, addr), data_(data)
{
    assert(data_);
    assert(!data_->object_);

    // Ensure internal object node is created
    if (!data_->object_) {
        auto object = model->newObject(initialSize);
        data_->object_ = object->addr();
    }
}

}
