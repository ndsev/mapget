import mapget


def handle_tile_request(tile: mapget.TileFeatureLayer):
    # Read out requested tile-id / map-id / layer-id
    print(f"Got request for tile={tile.tile_id().value:02X}, map={tile.map_id()}, layer={tile.layer_id()}.")

    # Create a new feature
    feature: mapget.Feature = tile.new_feature("Way", [("wayId", 0)])

    # Assign geometry
    geom: mapget.Geometry = feature.geom().new_geometry(mapget.GeomType.LINE)
    geom.append(41., 11.)
    geom.append(42., 12.)

    # Add an attribute
    fixed_attrs: mapget.Object = feature.attributes()
    fixed_attrs.add_field("isBridge", False)

    # Add an attribute layer
    attr_layer: mapget.Object = feature.attributeLayers().new_layer("rules")
    attr: mapget.Attribute = attr_layer.new_attribute("SPEED_LIMIT")
    attr.set_direction(mapget.Direction.POSITIVE)
    attr.add_field("speedLimit", "50")


# Instantiate a data source with a minimal mandatory set
# of meta-information.
ds = mapget.DataSource({
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
ds.go(port=54545)

# Wait until Ctrl-C is hit. Navigate e.g. to
#  http://localhost:54545/tile?layer=WayLayer&tileId=2&responseType=json
# to test the running data source.
ds.wait_for_signal()
