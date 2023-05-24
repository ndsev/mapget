#pragma once

#include "simfil/model/fields.h"

namespace mapget
{

/**
 * The Fields class is a case-insensitive dictionary for field names.
 * It acts as a container that automatically populates itself when fields are added to an object.
 * Multiple TileFeatureLayers can share the same Fields dictionary, which helps reduce the size
 * of serialized FeatureLayers.
 *
 * The inherited mapget::Fields provides additional static field IDs that can be used for various purposes.
 *
 * Field IDs managed by the Fields dictionary are stored as uint16 values, which are smaller and faster
 * to work with compared to strings. When querying fields, the Fields dictionary is used to look up the
 * field ID for a given name. Once the ID is obtained, subsequent searches are performed using this 16-bit
 * integer, resulting in improved efficiency.
 */
struct Fields : public simfil::Fields
{
    enum StaticFieldIds : simfil::FieldId {
        IdStr = NextStaticId,
        TypeIdStr,
        LayerStr,
        ChildrenStr,
        DirectionStr,
        ValidityStr,
        PropertiesStr,
    };

    Fields();
};

}
