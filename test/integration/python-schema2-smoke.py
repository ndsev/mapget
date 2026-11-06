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
        self.assertIsInstance(schema.feature_identities, tuple)
        self.assertEqual(tuple(info.name for info in schema.feature_identities), ("Road",))
        self.assertFalse(hasattr(layer, "feature_types"))
        feature_type = schema.get_type_info("Road")
        self.assertIsInstance(feature_type, mapget.FeatureIdentity)
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
        with self.assertRaises(AttributeError):
            schema.feature_identities = ()
        with self.assertRaises(KeyError):
            schema.get_type_info("Missing")
        with self.assertRaises(KeyError):
            layer.get_type_info("Missing")
        with self.assertRaises(KeyError):
            schema.layer("Missing")
        del source, schema, layer, feature_type, parts
        gc.collect()
        self.assertEqual(part.name, "variant")

    def test_feature_identities_are_map_owned_and_shared_between_layers(self) -> None:
        metadata = legacy_metadata()
        road = metadata["layers"]["Road"]
        reference = copy.deepcopy(road["featureTypes"][0])
        reference["name"] = "Intersection"
        road["featureTypes"].append(reference)
        hidden = copy.deepcopy(reference)
        hidden["name"] = "Hidden"
        road["featureTypes"].append(hidden)
        road["featureModelSchema"] = {
            "oneOf": [{"$ref": "#/definitions/Road"}, {"$ref": "#/definitions/Hidden"}],
            "definitions": {
                "Road": {
                    "type": "object",
                    "x-mapget": {"metaType": "Feature", "featureType": "Road"},
                },
                "Hidden": {
                    "type": "object",
                    "x-mapget": {
                        "metaType": "Feature", "featureType": "Hidden", "produced": False,
                    },
                },
            },
        }
        metadata["layers"]["RoadCopy"] = copy.deepcopy(road)
        copied_identity = metadata["layers"]["RoadCopy"]["featureTypes"][0]
        copied_identity["uniqueIdCompositions"][0][0]["description"] = "Same ID, other prose."
        lane = copy.deepcopy(reference)
        lane["name"] = "Lane"
        metadata["layers"]["Lane"] = {"featureTypes": [lane]}

        schema = mapget.MapSchema.from_metadata(metadata)
        restored = mapget.MapSchema.read(schema.write(), expected_id=schema.schema_id)
        retained = []
        for owner in (schema, restored):
            with self.subTest(schema_id=owner.schema_id):
                identities = owner.feature_identities
                self.assertEqual(len(identities), 4)
                self.assertEqual(
                    {info.name for info in identities}, {"Road", "Intersection", "Hidden", "Lane"}
                )
                self.assertEqual(owner.layer("Road").produced_feature_types, ("Road",))
                self.assertEqual(owner.layer("RoadCopy").produced_feature_types, ("Road",))
                self.assertEqual(owner.layer("Lane").produced_feature_types, ("Lane",))
                self.assertEqual(owner.layer("Raw").produced_feature_types, ())
                self.assertEqual(owner.layer("Road").feature_schema("Intersection"), 0)
                self.assertEqual(owner.layer("Road").get_type_info("Lane").name, "Lane")
                self.assertEqual(owner.layer("Raw").get_type_info("Hidden").name, "Hidden")
                for info in identities:
                    self.assertEqual(owner.get_type_info(info.name).name, info.name)
                    self.assertEqual(
                        info.unique_id_compositions[0][0].datatype, mapget.IdPartDataType.U64
                    )
                retained.append(next(info for info in identities if info.name == "Intersection"))
        del schema, restored, owner, identities, info
        gc.collect()
        for info in retained:
            self.assertEqual(info.unique_id_compositions[0][0].name, "id")
            self.assertEqual(info.unique_id_compositions[1][0].name, "alias")

    def test_conflicting_map_wide_feature_identities_are_rejected(self) -> None:
        metadata = legacy_metadata()
        other = copy.deepcopy(metadata["layers"]["Road"])
        other["featureTypes"][0]["uniqueIdCompositions"][0][0]["datatype"] = "I64"
        metadata["layers"]["Other"] = other
        with self.assertRaises((ValueError, RuntimeError)):
            mapget.MapSchema.from_metadata(metadata)

    def test_schema_types_and_inspection_are_read_only(self) -> None:
        schema = mapget.MapSchema.from_metadata(legacy_metadata())
        type_id = schema.layer("Raw").root_schema
        self.assertGreater(schema.type_count, 0)
        self.assertGreater(type_id, 0)
        reference = schema.schema_type(type_id)
        self.assertIsInstance(reference, mapget.SchemaType)
        self.assertFalse(hasattr(mapget, "SchemaReference"))
        self.assertTrue(reference)
        self.assertEqual(reference.type_id, type_id)
        self.assertEqual(reference.schema_id, schema.schema_id)
        self.assertIsInstance(reference.type_name, str)
        self.assertTrue(reference.kind)
        type_name, kind = reference.type_name, reference.kind
        self.assertEqual(schema.type_descriptor(type_id)["$ref"], type_id)
        self.assertFalse(hasattr(reference, "domain"))
        self.assertFalse(hasattr(schema, "domain_count"))
        self.assertTrue(hasattr(mapget.Bitmask, "type"))
        self.assertFalse(hasattr(mapget.Bitmask, "domain"))
        with self.assertRaises(AttributeError):
            reference.type_id = 0
        with self.assertRaises(AttributeError):
            reference.kind = "mutated"
        with self.assertRaises(AttributeError):
            schema.type_count = 0
        with self.assertRaises(TypeError):
            mapget.MapSchema()
        for invalid in (0, 65535):
            with self.subTest(type_id=invalid):
                with self.assertRaises(KeyError):
                    schema.schema_type(invalid)
                with self.assertRaises(KeyError):
                    schema.type_descriptor(invalid)
        schema_id = schema.schema_id
        del schema
        gc.collect()
        self.assertEqual(reference.schema_id, schema_id)
        self.assertEqual(reference.type_id, type_id)
        self.assertEqual(reference.type_name, type_name)
        self.assertEqual(reference.kind, kind)
        self.assertTrue(reference)

    def test_native_descriptors_preserve_named_values_and_exact_references(self) -> None:
        metadata = legacy_metadata()
        metadata["layers"]["Raw"]["featureModelSchema"] = {
            "type": "object",
            "additionalProperties": False,
            "definitions": {
                "phase": {
                    "type": "string",
                    "enum": ["ACTIVE", "WAITING"],
                    "x-mapget": {"namedValues": [
                        {"name": "ACTIVE", "value": "ACTIVE", "rawValue": 2**53 + 7},
                        {"name": "WAITING", "value": "WAITING", "rawValue": -7},
                    ]},
                },
            },
            "properties": {
                "phase": {"$ref": "#/definitions/phase"},
                "phaseAlias": {"$ref": "#/definitions/phase"},
                "next": {"$ref": "#"},
                "flags": {"x-mapget": {
                    "kind": {"name": "bitmask", "affinities": ["bitmask"]},
                    "bitWidth": 64,
                    "namedValues": [{"name": "HIGH", "value": 2**63, "role": "flag"}],
                }},
            },
        }
        schema = mapget.MapSchema.from_metadata(metadata)
        restored = mapget.MapSchema.read(schema.write(), expected_id=schema.schema_id)
        self.assertEqual(restored.type_count, schema.type_count)
        type_id = schema.layer("Raw").root_schema
        self.assertEqual(restored.layer("Raw").root_schema, type_id)
        expected = schema.type_descriptor(type_id)
        self.assertEqual(restored.type_descriptor(type_id), expected)
        for owner in (schema, restored):
            descriptor = owner.type_descriptor(type_id)
            fields = descriptor["fields"]
            phase_id = fields["phase"]["$ref"]
            self.assertEqual(fields["phaseAlias"]["$ref"], phase_id)
            phase = owner.type_descriptor(phase_id)
            self.assertEqual(phase["enum"], ["ACTIVE", "WAITING"])
            self.assertEqual(
                {value["name"]: (value["value"], value["rawValue"])
                 for value in phase["namedValues"]},
                {"ACTIVE": ("ACTIVE", 2**53 + 7), "WAITING": ("WAITING", -7)},
            )
            self.assertEqual(fields["next"]["$ref"], type_id)
            self.assertEqual(fields["next"]["truncated"], "cycle")
            flags = owner.type_descriptor(fields["flags"]["$ref"])
            self.assertEqual(flags["kind"], "bitmask")
            self.assertEqual(flags["bitWidth"], 64)
            self.assertEqual(flags["namedValues"][0]["value"], "0x8000000000000000")
            phase["namedValues"][0]["rawValue"] = 0
            self.assertEqual(owner.type_descriptor(type_id), expected)

    def test_exact_binary_roundtrip_and_failures(self) -> None:
        schema = mapget.MapSchema.from_metadata(legacy_metadata())
        wire = schema.write()
        self.assertIsInstance(wire, bytes)
        restored = mapget.MapSchema.read(wire, expected_id=schema.schema_id)
        self.assertEqual(restored.schema_id, schema.schema_id)
        self.assertEqual(restored.format, schema.format)
        self.assertEqual(set(restored.layer_names), {"Road", "Raw"})
        self.assertEqual(restored.write(), wire)
        self.assertEqual(restored.get_type_info("Road").name, "Road")
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
            "name": "fixture", "version": "2", "subsystems": [{"name": "roads", "version": "1"}]
        })
        self.assertEqual(value, mapget.MapFormat("fixture", "2", {"roads": "1"}))
        self.assertNotEqual(value, mapget.MapFormat("fixture", "2", {"roads": "2"}))
        value.subsystems[0]["version"] = "mutation of detached entry"
        self.assertEqual(value.subsystems, [{"name": "roads", "version": "1"}])
        multiple = mapget.MapFormat("fixture", "2", [
            {"name": "roads", "version": "2"}, {"name": "roads", "version": "1"},
            {"name": "roads", "version": "2"},
        ])
        self.assertEqual(multiple.subsystems, [
            {"name": "roads", "version": "1"}, {"name": "roads", "version": "2"},
        ])
        self.assertEqual(multiple, mapget.MapFormat(**multiple.to_dict()))
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
            retained = tiles[0].map_schema.get_type_info("Road")
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
