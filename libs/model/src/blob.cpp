#include "blob.h"

#include "bloblayer.h"
#include "simfil/model/nodes.h"

using simfil::ValueType;
using simfil::ModelNode;
using simfil::ModelNodeAddress;

namespace mapget
{

void CompoundBlobNode::setSourceRegion(std::tuple<size_t, size_t> region)
{
    data_->sourceRegion_ = SourceRegion(region);
}

std::tuple<size_t, size_t> CompoundBlobNode::sourceRegion() const
{
    return data_->sourceRegion_.asBits();
}

simfil::shared_model_ptr<simfil::Object> CompoundBlobNode::object()
{
    if (!data_->object_) {
        auto object = model().newObject();
        data_->object_ = object->addr();
        return object;
    }

    return const_cast<const CompoundBlobNode*>(this)->object();
}

simfil::shared_model_ptr<const simfil::Object> CompoundBlobNode::object() const
{
    if (!data_->object_)
        return {};
    return model().resolveObject(ModelNode::Ptr::make(model_, data_->object_));
}

ValueType CompoundBlobNode::type() const
{
    return ValueType::Object;
}

ModelNode::Ptr CompoundBlobNode::at(int64_t index) const
{
    if (auto m = object())
        return m->at(index);
    return {};
}

uint32_t CompoundBlobNode::size() const
{
    if (auto m = object())
        return m->size();
    return 0;
}

ModelNode::Ptr CompoundBlobNode::get(const simfil::FieldId& field) const
{
    if (auto m = object())
        return m->get(field);
    return {};
}

simfil::FieldId CompoundBlobNode::keyAt(int64_t index) const
{
    if (auto m = object())
        return m->keyAt(index);
    return {};
}

bool CompoundBlobNode::iterate(IterCallback const& cb) const
{
    for (auto i = 0; i < size(); ++i)
        if (!cb(*at(i)))
            return false;
    return true;
}

CompoundBlobNode::CompoundBlobNode(Data* data, simfil::ModelConstPtr model, simfil::ModelNodeAddress addr)
    : simfil::MandatoryDerivedModelNodeBase<TileBlobLayer>(std::move(model), addr), data_(data)
{
    // Ensure internal object node is created
    //object();
}

}
