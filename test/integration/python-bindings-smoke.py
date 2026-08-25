#!/usr/bin/env python3

from __future__ import annotations

import json
import urllib.request

import mapget
from ndslive.math import PackedTileId


def _get_json(url: str):
    with urllib.request.urlopen(url, timeout=10) as response:
        return json.loads(response.read().decode("utf-8"))


def _post_json(url: str, body: dict):
    request = urllib.request.Request(
        url,
        data=json.dumps(body).encode("utf-8"),
        headers={"content-type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=10) as response:
        payload = response.read().decode("utf-8")
        return json.loads(payload) if payload else None


def main() -> int:
    assert hasattr(mapget, "FilterRequest")
    assert hasattr(mapget, "FilterChannel")
    assert hasattr(mapget, "TileSubsetLayer")
    assert mapget.TileId is PackedTileId
    assert mapget.PackedTileId is PackedTileId
    parsed_key = mapget.MapTileKey("Features:Map:WayLayer:65536")
    assert isinstance(parsed_key.tile_id, PackedTileId)
    assert parsed_key.tile_id.value == 65536

    point = mapget.Point
    cache_expired_calls: list[tuple[str, int]] = []
    requested_tiles: list[int] = []

    def fill_feature_tile(tile: mapget.TileFeatureLayer) -> None:
        assert isinstance(tile.tile_id(), PackedTileId)
        assert tile.tile_id().value == 65536
        requested_tiles.append(tile.tile_id().value)
        feature = tile.new_feature("Way", [("wayId", 1)])

        geometry = feature.geom().new_geometry(mapget.GeomType.LINE)
        geometry.append(point(1.0, 2.0))
        geometry.append(point(2.0, 3.0))
        geometry.set_name("centerline")
        assert geometry.name() == "centerline"

        point_source_refs = tile.new_source_data_references(
            [("RawLayer", "attribute-point", mapget.SourceDataAddress(30, 6))]
        )
        sequence_source_refs = tile.new_source_data_references(
            [("RawLayer", "attribute-point-sequence", mapget.SourceDataAddress(36, 8))]
        )
        attr_point_sequence = tile.new_attr_point_sequence(feature, geometry)
        attr_point = attr_point_sequence.append_attr_point(
            1,
            point(1.5, 2.5),
            point_source_refs,
        )
        attr_point_sequence.set_source_data_references(sequence_source_refs)
        assert isinstance(attr_point, mapget.AttrPoint)
        assert attr_point.index() == 1
        assert attr_point.point() == point(1.5, 2.5)
        assert attr_point.source_data_references().to_list()[0]["qualifier"] == "attribute-point"
        plain_attr_point = attr_point_sequence.append_attr_point(2, point(1.75, 2.75))
        assert plain_attr_point.source_data_references() is None
        assert len(attr_point_sequence.attr_points()) == 2
        assert attr_point_sequence.attr_points()[0].index() == 1
        assert (
            attr_point_sequence.source_data_references().to_list()[0]["qualifier"]
            == "attribute-point-sequence"
        )
        assert attr_point_sequence.feature_id().to_string() == "Way.1"
        assert attr_point_sequence.geometry_index() == 0
        assert attr_point_sequence.attr_point_count() == 2
        assert attr_point_sequence.position_count() == 4
        assert attr_point_sequence.points(0, 3) == [
            point(1.0, 2.0),
            point(1.5, 2.5),
            point(1.75, 2.75),
            point(2.0, 3.0),
        ]
        assert attr_point_sequence.is_attr_point(1)

        rules_layer = feature.attribute_layers().new_layer("rules")
        attr = rules_layer.new_attribute("speed")
        attr.validity().new_offset_range(
            mapget.ValidityGeometryOffsetType.RELATIVE_LENGTH,
            0.1,
            0.5,
            direction=mapget.Direction.POSITIVE,
        )
        attr.add_field("value", 42)

        indexed_attr = rules_layer.new_attribute("indexed")
        indexed_attr.validity().new_attr_point_index_range(
            attr_point_sequence,
            0,
            2,
            direction=mapget.Direction.POSITIVE,
        )
        indexed_attr.add_field("value", True)

        target = tile.new_feature_id("Way", [("wayId", 2)])
        relation = feature.add_relation("next", target)
        relation.source_validity().new_complete()

        source_refs = tile.new_source_data_references(
            [("RawLayer", "primary", mapget.SourceDataAddress(10, 20))]
        )
        feature.set_source_data_references(source_refs)

    def fill_source_data_tile(tile: mapget.TileSourceDataLayer) -> None:
        compound = tile.new_compound(2)
        compound.set_schema_name("example.Type")
        compound.set_source_data_address(mapget.SourceDataAddress(1, 8))
        compound.add_field("answer", 42)
        tile.add_root(compound)

    def locate(request: mapget.LocateRequest) -> list[mapget.LocateCandidate]:
        tile_key = mapget.MapTileKey(
            mapget.LayerType.FEATURES,
            request.map_id,
            "WayLayer",
            PackedTileId.from_tile_xy(0, 0, 0),
        )
        computed = mapget.LocateCandidate.from_feature_id_expression(
            tile_key,
            "Way",
            "select(($features.*.id), locateWayIndex)",
            {"locateWayIndex": 0},
        )
        assert computed.to_dict()["selector"] == {
            "typeId": "Way",
            "featureIdExpression": "select(($features.*.id), locateWayIndex)",
            "bindings": {"locateWayIndex": 0},
        }
        return [
            mapget.LocateCandidate(
                tile_key,
                "Way",
                "wayId == locateWayId",
                {"locateWayId": request.get_int_id_part("wayId")},
            )
        ]

    def on_cache_expired(tile_key: mapget.MapTileKey, expired_at_us: int) -> None:
        cache_expired_calls.append((tile_key.to_string(), expired_at_us))

    datasource = mapget.DataSourceServer(
        {
            "stringPoolId": "python-bindings-smoke",
            "mapId": "Map",
            "layers": {
                "WayLayer": {
                    "featureTypes": [
                        {
                            "name": "Way",
                            "uniqueIdCompositions": [[{"partId": "wayId", "datatype": "I64"}]],
                        }
                    ]
                },
                "RawLayer": {"type": "SourceData"},
            },
        }
    )
    datasource.on_tile_feature_request(fill_feature_tile)
    datasource.on_tile_sourcedata_request(fill_source_data_tile)
    datasource.on_locate_request(locate)
    datasource.on_cache_expired(on_cache_expired)

    datasource.go("127.0.0.1")
    try:
        base_url = f"http://127.0.0.1:{datasource.port()}"

        feature_tile = _get_json(f"{base_url}/tile?layer=WayLayer&tileId=65536&responseType=json")
        assert requested_tiles == [65536]
        feature = feature_tile["features"][0]
        assert feature["geometry"]["geometryName"] == "centerline"
        assert feature["relations"][0]["name"] == "next"
        assert feature["relations"][0]["sourceValidity"]["direction"] == "COMPLETE"
        assert feature["properties"]["layer"]["rules"]["speed"]["validity"]["offsetType"] == "RelativeLengthOffset"
        assert feature["properties"]["layer"]["rules"]["indexed"]["validity"] == {
            "direction": "POSITIVE",
            "attrPointIndexRange": {
                "sequence": {"$mapgetAttrPointSequence": 0},
                "start": 0,
                "end": 2,
            },
        }
        attr_point_json = feature_tile["attrPointSequences"][0]["attrPoints"][0]
        assert attr_point_json["index"] == 1
        assert attr_point_json["point"] == [1.5, 2.5, 0.0]
        assert attr_point_json["_sourceData"][0]["qualifier"] == "attribute-point"
        assert (
            feature_tile["attrPointSequences"][0]["_sourceData"][0]["qualifier"]
            == "attribute-point-sequence"
        )
        assert feature["_sourceData"][0]["qualifier"] == "primary"

        source_tile = _get_json(f"{base_url}/tile?layer=RawLayer&tileId=65536&responseType=json")
        assert source_tile == [{"answer": 42}]

        locate_response = _post_json(
            f"{base_url}/locate",
            {"mapId": "Map", "typeId": "Way", "featureId": ["wayId", 1]},
        )
        assert locate_response[0]["tileId"] == "Features:Map:WayLayer:65536"
        assert locate_response[0]["selector"] == {
            "typeId": "Way",
            "featureFilter": "wayId == locateWayId",
            "bindings": {"locateWayId": 1},
        }

        _post_json(
            f"{base_url}/cache-expired",
            {"tileKey": "Features:Map:WayLayer:65536", "expiredAt": 123456},
        )
        assert cache_expired_calls == [("Features:Map:WayLayer:65536", 123456)]
    finally:
        datasource.stop()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
