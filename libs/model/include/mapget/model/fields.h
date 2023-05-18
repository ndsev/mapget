#pragma once

#include "simfil/model/fields.h"

namespace mapget
{

/// Extended field name cache with extra static string IDs
struct Fields : public simfil::Fields
{
    enum StaticFieldIds : simfil::FieldId {
        IdStr = NextStaticId,
        TypeIdStr,
        LayerStr,
        ChildrenStr
    };

    Fields();
};

}
