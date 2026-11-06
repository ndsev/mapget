#pragma once

#include "mapget/model/featureidentity.h"
#include "simfil/model/schema-type.h"

namespace mapget
{

/** Native Feature envelope over the original canonical type, not a second schema graph.
 * Child views preserve the parent's ownership and writable mode. Identity traversal
 * borrows the map; retain the parent or explicitly retain the returned identity.
 */
class FeatureSchemaType : public simfil::SchemaObjectType
{
public:
    /** Require a complete native Feature envelope; thin imported roots stay generic views. */
    explicit FeatureSchemaType(simfil::SchemaType type);
    /** Retain the same canonical record without changing its writable mode. */
    [[nodiscard]] FeatureSchemaType retain() const;
    /** Resolve the declared identity from the typeId discriminator, not the display typename. */
    [[nodiscard]] FeatureIdentity identity() const;
    /** Borrow extensible basic properties, excluding the immutable envelope itself. */
    [[nodiscard]] simfil::SchemaObjectType properties() const;
    /** Borrow the map of named attribute-layer containers. */
    [[nodiscard]] simfil::SchemaObjectType attributeLayers() const;
};

/** Physical Attribute schema with common validity/source links and provider-defined members.
 * Payloads need not use attributeValue: ADAS, region and Classic converters can
 * declare their own fields through the ordinary object API.
 */
class AttributeSchemaType : public simfil::SchemaObjectType
{
public:
    /** Require the native Attribute kind and its shared optional members. */
    explicit AttributeSchemaType(simfil::SchemaType type);
    /** Retain this same canonical type without copying its graph. */
    [[nodiscard]] AttributeSchemaType retain() const;
    /** Borrow the common singular-or-collection validity type. */
    [[nodiscard]] simfil::SchemaType validities() const;
    /** Borrow the common source-data-reference array type. */
    [[nodiscard]] simfil::SchemaType sourceDataReferences() const;
};

}  // namespace mapget
