#!python

import mapget
from ndslive.math import PackedTileId
from mapget import Point as Pt
import sys


def handle_tile_request(tile: mapget.TileFeatureLayer):
    # Read out requested tile-id / map-id / layer-id
    print(f"Got request for tile={tile.tile_id().value:02X}, map={tile.map_id()}, layer={tile.layer_id()}.")

    # Create a new feature
    feature: mapget.Feature = tile.new_feature("Way", [("wayId", 0)])

    # Assign geometry (low-level)
    geom: mapget.Geometry = feature.geom().new_geometry(mapget.GeomType.LINE)
    geom.append(41., 11.)
    geom.append(Pt(x=42., y=12., z=506))

    # Assign geometry (high-level)
    feature.add_point(Pt(42.5, 11.6))
    feature.add_points([Pt(42.5, 11.6), Pt(42.5, 11.8)])
    feature.add_line([Pt(42.5, 11.6), Pt(42.5, 11.7)])
    feature.add_mesh([Pt(42.5, 11.6), Pt(42.5, 11.7), Pt(42.2, 11.7)])
    feature.add_poly([Pt(42.5, 11.6), Pt(42.5, 11.7), Pt(42.2, 11.7), Pt(42.2, 11.3)])

    # Add an attribute
    fixed_attrs: mapget.Object = feature.attributes()
    fixed_attrs.add_field("isBridge", False)

    # Add an attribute which has a compound value (low-level)
    attr_obj = tile.new_object()
    attr_obj.add_field("name", "Main St.")
    attr_obj.add_field("houseNumber", 5)
    attr_arr = tile.new_array()
    attr_arr.append(attr_obj)
    attr_arr.append(attr_obj)
    fixed_attrs.add_field("addresses", attr_arr)

    # Add an attribute which has a compound value (high-level)
    # Note: Map values may also be feature IDs to create references.
    fixed_attrs.add_field("pois", [
        {"name": "Bakery", "rating": 10},
        {"name": "Gas Station", "rating": 8},
        {"reference": tile.new_feature_id("Way", [("wayId", 7)])}
    ])

    # Add an attribute layer
    attr_layer: mapget.Object = feature.attribute_layers().new_layer("rules")
    attr: mapget.Attribute = attr_layer.new_attribute("SPEED_LIMIT")
    attr.validity().new_offset_range(
        mapget.ValidityGeometryOffsetType.RELATIVE_LENGTH,
        0.0,
        1.0,
        direction=mapget.Direction.POSITIVE)
    attr.add_field("speedLimit", 50)

    # Add a feature relation
    target = tile.new_feature_id("Way", [("wayId", 10)])
    relation = feature.add_relation("successor", target)
    relation.source_validity().new_complete()

    # Attach source-data provenance.
    refs = tile.new_source_data_references([
        ("RawWayLayer", "primary", mapget.SourceDataAddress(0, 128))
    ])
    feature.set_source_data_references(refs)


def handle_source_data_request(tile: mapget.TileSourceDataLayer):
    compound = tile.new_compound(2)
    compound.set_schema_name("example.RawWay")
    compound.set_source_data_address(mapget.SourceDataAddress(0, 128))
    compound.add_field("wayId", 0)
    compound.add_field("name", "Main St.")
    tile.add_root(compound)


def handle_locate_request(request: mapget.LocateRequest):
    response = mapget.LocateResponse(request)
    response.tile_key = mapget.MapTileKey(
        mapget.LayerType.FEATURES,
        request.map_id,
        "WayLayer",
        PackedTileId.from_tile_xy(0, 0, 0),
        0)
    return [response]


def handle_cache_expired(tile_key: mapget.MapTileKey, expired_at_us: int):
    print(f"Cached tile expired: {tile_key} at {expired_at_us}us.")


# Instantiate a data source with a minimal mandatory set
# of meta-information.
ds = mapget.DataSourceServer({
    "layers": {
        "WayLayer": {
            "featureTypes": [
                {
                    "name": "Way",
                    "uniqueIdCompositions": [[{
                        "partId": "wayId",
                        "datatype": "I64"
                    }]]
                }
            ]
        },
        "RawWayLayer": {
            "type": "SourceData"
        }
    }, "mapId": "TestMap"
})

# Set the callback which is invoked when a tile is requested.
ds.on_tile_feature_request(handle_tile_request)
ds.on_tile_sourcedata_request(handle_source_data_request)
ds.on_locate_request(handle_locate_request)
ds.on_cache_expired(handle_cache_expired)

# Parse port as optional first argument
port = 0  # Pick random free port
if len(sys.argv) > 1:
    port = int(sys.argv[1])

# Run the data source - you may also set port=0 to select a
# port automatically.
ds.go(port=port)

# Wait until Ctrl-C is hit. Navigate e.g. to
#  http://localhost:54544/tile?layer=WayLayer&tileId=2&responseType=json
# to test the running data source.
ds.wait_for_signal()
