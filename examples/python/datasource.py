import mapget
from mapget import Point as Pt


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
    attr.set_direction(mapget.Direction.POSITIVE)
    attr.add_field("speedLimit", 50)

    # Add a child feature ID
    feature.children().append(tile.new_feature_id("Way", [("wayId", 10)]))


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
        }
    }, "mapId": "TestMap"
})

# Set the callback which is invoked when a tile is requested.
ds.on_tile_request(handle_tile_request)

# Run the data source - you may also set port=0 to select a
# port automatically.
ds.go(port=54544)

# Wait until Ctrl-C is hit. Navigate e.g. to
#  http://localhost:54544/tile?layer=WayLayer&tileId=2&responseType=json
# to test the running data source.
ds.wait_for_signal()
