#pragma once

#include "simfil/model/model.h"
#include "simfil/model/string-pool.h"

namespace mapget
{

/** Compact serialized representation shared by embedded and detached feature-id nodes. */
struct FeatureIdData
{
    MODEL_COLUMN_TYPE(12);

    bool useCommonTilePrefix_ = false;
    uint8_t idCompositionIndex_ = 0;
    simfil::StringId typeId_ = 0;
    simfil::ArrayIndex idPartValues_ = simfil::InvalidArrayIndex;
    simfil::StringId extMapId_ = simfil::StringPool::Empty;
};

} // namespace mapget
