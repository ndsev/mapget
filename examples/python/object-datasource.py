#!/usr/bin/env python3
"""A credential-free object datasource; put mapget serve in front of this server."""

import argparse

import mapget
from ndslive.math import PackedTileId


class ObjectRoadSource:
    """Serve one immutable road object, independently of its spatial discovery tiles."""

    MAP = "ObjectExample"
    LAYER = "Road"
    OBJECT_ID = 2**64 - 1
    LEVEL = 13
    START = (11.57, 48.14)
    END = (11.60, 48.14)

    def __init__(self):
        """Declare object addressing and install discovery/fill callbacks without starting I/O."""
        start = PackedTileId.from_wgs84(*self.START, self.LEVEL)
        end = PackedTileId.from_wgs84(*self.END, self.LEVEL)
        # The synthetic east-west line occupies every tile between its endpoints.
        self.discovery_tiles = frozenset(
            PackedTileId.from_tile_xy(x, start.y(), self.LEVEL).value
            for x in range(start.x(), end.x() + 1)
        )
        self.server = mapget.DataSourceServer({
            "mapId": self.MAP,
            "stringPoolId": "object-example-pool",
            "maxParallelJobs": 2,
            "layers": {
                self.LAYER: {
                    "partitionKind": "object",
                    "tileAssociationLevel": self.LEVEL,
                    "featureTypes": [{
                        "name": "Road",
                        "uniqueIdCompositions": [[
                            {"partId": "objectId", "datatype": "U64"},
                            {"partId": "localId", "datatype": "U32"},
                        ]],
                    }],
                },
            },
        })
        self.server.on_object_discovery_request(self.discover)
        self.server.on_tile_feature_request(self.fill)

    def discover(self, request: mapget.ObjectDiscoveryRequest):
        """Return known associations only; even successful discovery must not load the object."""
        result = mapget.ObjectDiscoveryResult()
        result.ttl_ms = 30_000
        if request.layer_id != self.LAYER or request.tile_id.level() != self.LEVEL:
            result.status = mapget.ObjectDiscoveryStatus.FAILED
            result.message = "Expected the Road layer and a level-13 discovery tile."
        elif request.tile_id.value in self.discovery_tiles:
            result.objects = [mapget.ObjectReference(
                self.OBJECT_ID, [self.START[0], self.START[1], self.END[0], self.END[1]])]
        # This complete synthetic index knows that every other valid tile is empty.
        # A real provider with missing association data should return UNAVAILABLE instead.
        return result

    def fill(self, layer: mapget.PartitionFeatureLayer):
        """Construct the complete object with an explicit anchor and independently expiring data."""
        object_id = layer.partition_id().object_id  # Checked; never reinterpret as a PackedTileId.
        if object_id != self.OBJECT_ID:
            layer.set_error("Unknown object ID.")
            layer.set_ttl(1000)
            return
        layer.set_geometry_anchor(mapget.Point(*self.START))
        layer.set_ttl(60_000)
        # Only model integer ID parts use signed int64; transport identity remains unsigned.
        signed_id = object_id if object_id < 2**63 else object_id - 2**64
        road = layer.new_feature("Road", [("objectId", signed_id), ("localId", 1)])
        road.add_line([mapget.Point(*self.START), mapget.Point(*self.END)])
        road.attributes().add_field("name", "Example road")
        road.attributes().add_field("speedLimitKmh", 50)


def main():
    """Start a loopback datasource for use by a separate public mapget service."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=9100)
    args = parser.parse_args()
    source = ObjectRoadSource()
    source.server.go("127.0.0.1", args.port)
    print(f"Discovery tile IDs: {sorted(source.discovery_tiles)}", flush=True)
    try:
        source.server.wait_for_signal()
    finally:
        source.server.stop()


if __name__ == "__main__":
    main()
