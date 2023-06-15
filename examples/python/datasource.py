import mapget


def handle_tile_request(tile: mapget.TileFeatureLayer):
    feature: mapget.Feature = tile.new_feature("Way", [("wayId", 0)])


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

ds.on_tile_request(handle_tile_request)
ds.go()
ds.wait_for_signal()
