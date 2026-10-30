#!/usr/bin/env python3
"""Schema2 consumer contracts. Run against the coordinated, rebuilt mapget wheel."""

from __future__ import annotations

import copy
import gc
import json
import multiprocessing
import unittest

import mapget
from ndslive.math import PackedTileId


def legacy_metadata() -> dict:
    """Identity-only legacy input remains usable without authoring a native graph."""
    return {
        "mapId": "PythonSchema2",
        "stringPoolId": "python-schema2",
        "layers": {
            "Road": {
                "featureTypes": [
                    {
                        "name": "Road",
                        "uniqueIdCompositions": [
                            [
                                {"partId": "id", "datatype": "U64"},
                                {
                                    "partId": "variant",
                                    "datatype": "STR",
                                    "isOptional": True,
                                    "isSynthetic": True,
                                },
                            ],
                            [{"partId": "alias", "datatype": "STR"}],
                        ],
                    }
                ]
            },
            "Raw": {"type": "SourceData"},
        },
    }


def _serve_schema_proxy(source_port: int, ready, stopped) -> None:
    """Drogon's singleton requires the service and datasource in separate processes."""
    service = mapget.Service(worker_count=1)
    try:
        service.add_remote_datasource("127.0.0.1", source_port)
        service.go("127.0.0.1", 0)
        ready.send({"port": service.port()})
        ready.close()
        stopped.wait()
    except Exception as error:
        if not ready.closed:
            ready.send({"error": str(error)})
        raise
    finally:
        service.stop()
        ready.close()


class Schema2PythonTest(unittest.TestCase):
    def test_incoming_v2_scope_and_status(self) -> None:
        key = "Features:Example:Road:131073"
        for status, expected, complete in (
            ("no-possible-family", mapget.IncomingRelationStatus.NO_POSSIBLE_FAMILY, True),
            ("complete", mapget.IncomingRelationStatus.COMPLETE, True),
            ("partial", mapget.IncomingRelationStatus.PARTIAL, False),
            ("unsupported-scope", mapget.IncomingRelationStatus.UNSUPPORTED_SCOPE, False),
        ):
            with self.subTest(status=status):
                manifest = {
                    "type": "mapget.incoming.relations", "version": 2,
                    "target": key, "featureId": "Road.1", "complete": complete,
                    "status": status, "scope": [key], "snapshots": [key],
                    "issues": [], "relations": [],
                }
                result = mapget.IncomingRelationCollection.from_json(json.dumps(manifest))
                self.assertEqual(result.status, expected)
                self.assertEqual(result.complete, complete)
                self.assertEqual(len(result.scope), 1)
                self.assertEqual(result.to_dict()["scope"], [key])

    def test_legacy_normalization_and_canonical_constructor(self) -> None:
        source = mapget.DataSourceServer(legacy_metadata())
        info = source.info()
        self.assertIsInstance(info, dict)
        self.assertEqual(info["schemaId"], source.map_schema.schema_id)
        self.assertEqual(info["mapFormat"], source.map_schema.format.to_dict())
        for layer in info["layers"].values():
            self.assertNotIn("featureTypes", layer)
            self.assertNotIn("featureModelSchema", layer)
            self.assertNotIn("schemaFeatureTypes", layer)
        restored = mapget.DataSourceServer(info, map_schema=source.map_schema)
        self.assertEqual(restored.map_schema.schema_id, source.map_schema.schema_id)
        self.assertEqual(restored.info(), info)
        self.assertEqual(
            mapget.MapSchema.from_metadata(legacy_metadata()).schema_id,
            source.map_schema.schema_id,
        )

    def test_read_only_views_retain_the_original_owner(self) -> None:
        source = mapget.DataSourceServer(legacy_metadata())
        schema = source.map_schema
        layer = schema.layer("Road")
        self.assertEqual(layer.type, mapget.LayerType.FEATURES)
        self.assertEqual(layer.name, "Road")
        self.assertEqual(layer.map_schema.schema_id, schema.schema_id)
        self.assertEqual(layer.produced_feature_types, ("Road",))
        self.assertIsInstance(layer.feature_types, tuple)
        feature_type = layer.get_type_info("Road")
        parts = feature_type.unique_id_compositions
        self.assertIsInstance(parts, tuple)
        self.assertIsInstance(parts[0], tuple)
        part = parts[0][1]
        self.assertEqual(part.datatype, mapget.IdPartDataType.STR)
        self.assertTrue(part.is_optional)
        self.assertTrue(part.is_synthetic)
        with self.assertRaises(AttributeError):
            part.name = "mutated"
        with self.assertRaises(AttributeError):
            feature_type.name = "mutated"
        with self.assertRaises(AttributeError):
            schema.schema_id = "other"
        with self.assertRaises(KeyError):
            layer.get_type_info("Missing")
        with self.assertRaises(KeyError):
            schema.layer("Missing")
        del source, schema, layer, feature_type, parts
        gc.collect()
        self.assertEqual(part.name, "variant")

    def test_exact_binary_roundtrip_and_failures(self) -> None:
        schema = mapget.MapSchema.from_metadata(legacy_metadata())
        wire = schema.write()
        self.assertIsInstance(wire, bytes)
        restored = mapget.MapSchema.read(wire, expected_id=schema.schema_id)
        self.assertEqual(restored.schema_id, schema.schema_id)
        self.assertEqual(restored.format, schema.format)
        self.assertEqual(set(restored.layer_names), {"Road", "Raw"})
        self.assertEqual(restored.write(), wire)
        self.assertEqual(restored.layer("Road").get_type_info("Road").name, "Road")
        for invalid in (b"", wire[:12], wire[:-1], b"not a schema"):
            with self.subTest(size=len(invalid)):
                with self.assertRaises((ValueError, RuntimeError)):
                    mapget.MapSchema.read(invalid, expected_id=schema.schema_id)
        with self.assertRaises((ValueError, RuntimeError)):
            mapget.MapSchema.read(wire, expected_id="fixture:wrong")
        with self.assertRaises((ValueError, RuntimeError)):
            mapget.MapSchema.read(wire, max_bytes=len(wire) - 1)

    def test_canonical_inputs_cannot_reinterpret_a_different_graph(self) -> None:
        source = mapget.DataSourceServer(legacy_metadata())
        changed = legacy_metadata()
        changed["layers"]["Road"]["featureTypes"][0]["name"] = "Other"
        other = mapget.MapSchema.from_metadata(changed)
        with self.assertRaises((ValueError, RuntimeError)):
            mapget.DataSourceServer(source.info(), map_schema=other)
        missing = copy.deepcopy(source.info())
        missing["layers"]["Missing"] = missing["layers"].pop("Road")
        with self.assertRaises((ValueError, RuntimeError)):
            mapget.DataSourceServer(missing, map_schema=source.map_schema)
        incompatible = copy.deepcopy(source.info())
        incompatible["layers"]["Road"]["type"] = "SourceData"
        with self.assertRaises((ValueError, RuntimeError)):
            mapget.DataSourceServer(incompatible, map_schema=source.map_schema)

    def test_format_is_descriptive_and_detached(self) -> None:
        value = mapget.MapFormat("fixture", "2", {"roads": "1"})
        self.assertEqual(value.to_dict(), {
            "name": "fixture", "version": "2", "subsystems": {"roads": "1"}
        })
        self.assertEqual(value, mapget.MapFormat("fixture", "2", {"roads": "1"}))
        self.assertNotEqual(value, mapget.MapFormat("fixture", "2", {"roads": "2"}))
        value.subsystems["roads"] = "mutation of detached dict"
        self.assertEqual(value.subsystems, {"roads": "1"})
        with self.assertRaises(AttributeError):
            value.version = "3"
        with self.assertRaises((ValueError, RuntimeError)):
            mapget.MapFormat("", "2")
        with self.assertRaises((ValueError, RuntimeError)):
            mapget.MapFormat("fixture", "2", {"": "1"})

    def test_client_sources_get_schema_and_partition_ownership(self) -> None:
        source = mapget.DataSourceServer(legacy_metadata())
        expected_id = source.map_schema.schema_id
        callback_owners = []

        def fill(tile: mapget.TileFeatureLayer) -> None:
            callback_owners.append(tile.map_schema)
            self.assertEqual(tile.layer_schema.map_schema.schema_id, expected_id)
            tile.new_feature("Road", [("id", 1)])

        source.on_tile_feature_request(fill)
        context = multiprocessing.get_context("spawn")
        ready, sender = context.Pipe(duplex=False)
        stopped = context.Event()
        proxy = None
        try:
            source.go("127.0.0.1", 0)
            proxy = context.Process(
                target=_serve_schema_proxy, args=(source.port(), sender, stopped)
            )
            proxy.start()
            sender.close()
            self.assertTrue(ready.poll(15), "Schema proxy startup timed out")
            started = ready.recv()
            self.assertNotIn("error", started)
            client = mapget.Client("127.0.0.1", started["port"])
            rows = client.sources()
            self.assertIsInstance(rows, list)
            row = next(item for item in rows if item["mapId"] == "PythonSchema2")
            self.assertIsInstance(row["sourceId"], str)
            self.assertEqual(row["schemaId"], expected_id)
            owner = client.get_schema(row["sourceId"], row["schemaId"])
            self.assertEqual(owner.schema_id, expected_id)
            self.assertEqual(owner.write(), source.map_schema.write())
            with self.assertRaises((ValueError, RuntimeError)):
                client.get_schema("missing-source", expected_id)
            with self.assertRaises((ValueError, RuntimeError)):
                client.get_schema(row["sourceId"], "fixture:wrong")
            request = mapget.Request("PythonSchema2", "Road", [PackedTileId.from_value(131073)],
                                     source_id=row["sourceId"])
            tiles = list(client.request(request))
            self.assertEqual(len(tiles), 1)
            self.assertEqual(tiles[0].map_schema.schema_id, expected_id)
            retained = tiles[0].layer_schema.get_type_info("Road")
            self.assertEqual(len(callback_owners), 1)
            del client, request, tiles, owner
        finally:
            stopped.set()
            if proxy is not None and proxy.pid is not None:
                proxy.join(10)
                if proxy.is_alive():
                    proxy.terminate()
                    proxy.join(10)
            ready.close()
            sender.close()
            source.stop()
        self.assertEqual(proxy.exitcode, 0)
        del source, callback_owners
        gc.collect()
        self.assertEqual(retained.unique_id_compositions[0][0].name, "id")


if __name__ == "__main__":
    unittest.main()
