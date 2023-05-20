#include "fields.h"

namespace mapget
{

Fields::Fields() {
    addStaticKey(IdStr, "id");
    addStaticKey(TypeIdStr, "typeId");
    addStaticKey(LayerStr, "layer");
    addStaticKey(ChildrenStr, "children");
    addStaticKey(DirectionStr, "direction");
    addStaticKey(ValidityStr, "validity");
}

}
