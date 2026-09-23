#!/usr/bin/env python3
"""Exercise the documented object datasource through a real public mapget service."""

import importlib.util
import http.client
import json
import math
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

import mapget
from ndslive.math import PackedTileId


def post(base_url, path, body):
    """Return bytes so tests can distinguish discovery JSON from payload JSON Lines."""
    url = urllib.parse.urlsplit(base_url)
    connection = http.client.HTTPConnection(url.hostname, url.port, timeout=10)
    try:
        connection.request("POST", path, json.dumps(body), {"Content-Type": "application/json"})
        response = connection.getresponse()
        assert response.status == 200, response.read()
        return response.read()
    finally:
        connection.close()


def check_discovery(source, port, fills):
    """Check discovery before any client has requested payloads."""
    base = f"http://127.0.0.1:{port}"
    tiles = sorted(source.discovery_tiles)
    assert len(tiles) > 1
    discovery = json.loads(post(base, "/objects/discover", {"requests": [{
        "mapId": source.MAP, "layerId": source.LAYER, "tileIds": tiles}]}))
    assert fills == []  # Associations never cause payload conversion.
    assert [item["tileId"] for item in discovery["responses"]] == tiles
    assert all(item["status"] == "success" for item in discovery["responses"])
    assert all(item["ttlMs"] == 30_000 for item in discovery["responses"])
    ids = {obj["id"] for item in discovery["responses"] for obj in item["objects"]}
    assert ids == {str(source.OBJECT_ID)}



def exercise(source, port):
    """Run the documented client in its own process, separate from Python datasource callbacks."""
    base = f"http://127.0.0.1:{port}"
    client = mapget.Client("127.0.0.1", port)
    # This is the developer-guide snippet's typed discovery/load path.
    associations = client.discover_objects(mapget.ObjectDiscoveryRequest(
        source.MAP, source.LAYER, PackedTileId.from_wgs84(*source.START, source.LEVEL)))
    assert associations.status == mapget.ObjectDiscoveryStatus.SUCCESS
    partitions = [mapget.PartitionId.object(ref.object_id) for ref in associations.objects]
    layers = list(client.request(mapget.Request(source.MAP, source.LAYER, partitions)))
    assert len(layers) == 1
    layer = layers[0]
    assert layer.partition_id().object_id == source.OBJECT_ID
    assert layer.geometry_anchor() == mapget.Point(*source.START)
    assert layer.ttl() == 60_000
    canonical_id = f"Road.{source.OBJECT_ID}.1"
    feature_json = json.loads(layer.geojson())["features"][0]
    assert feature_json["id"] == canonical_id
    coordinates = feature_json["geometry"]["coordinates"]
    assert len(coordinates) == 2
    for actual, expected in zip(coordinates, ([*source.START, 0.0], [*source.END, 0.0])):
        assert len(actual) == 3
        assert all(math.isclose(a, e, rel_tol=0, abs_tol=1e-7) for a, e in zip(actual, expected))

    request = {"mapId": source.MAP, "layerId": source.LAYER,
               "partitions": [{"kind": "object", "id": str(source.OBJECT_ID)}]}
    payloads = post(base, "/tiles", {"requests": [request], "responseType": "jsonl"})
    payload = json.loads(payloads.splitlines()[0])
    assert payload["partition"] == request["partitions"][0]
    assert payload["features"][0]["id"] == canonical_id
    subsets = post(base, "/filter", {
        "requests": [request], "responseType": "jsonl",
        "channels": [{"channelId": "roads", "scope": "feature",
                      "entryFilter": "properties.speedLimitKmh > 30",
                      "featureFields": ["properties.name"]}]})
    # Filter streams interleave status records with model payloads.
    subset_layers = [item for line in subsets.splitlines()
                     if (item := json.loads(line)).get("type") == "PartitionSubsetLayer"]
    assert len(subset_layers) == 1
    subset = subset_layers[0]
    assert subset["partition"] == request["partitions"][0]
    assert not subset["issues"]
    assert len(subset["channels"]) == 1
    channel = subset["channels"][0]
    assert channel["channelId"] == "roads"
    assert channel["featureFields"] == ["properties.name"]
    assert len(channel["featureEntries"]) == 1
    assert channel["featureEntries"][0]["featureId"] == canonical_id
    assert channel["featureEntries"][0]["values"] == ["Example road"]
    assert subset["dependencies"] == [{
        "sourceTileKey": f"Features:{source.MAP}:{source.LAYER}:object/{source.OBJECT_ID}",
        "sourceFeatureCount": 1}]

    empty = client.discover_objects(mapget.ObjectDiscoveryRequest(
        source.MAP, source.LAYER, PackedTileId.from_wgs84(0, 0, source.LEVEL)))
    assert empty.status == mapget.ObjectDiscoveryStatus.SUCCESS and not empty.objects
    wrong_level = client.discover_objects(mapget.ObjectDiscoveryRequest(
        source.MAP, source.LAYER, PackedTileId.from_wgs84(*source.START, source.LEVEL - 1)))
    assert wrong_level.status == mapget.ObjectDiscoveryStatus.FAILED
    assert wrong_level.message and not wrong_level.objects


def main():
    """Import the example unchanged and keep datasource/service processes independently owned."""
    example_path = Path(__file__).resolve().parents[2] / "examples/python/object-datasource.py"
    spec = importlib.util.spec_from_file_location("object_datasource_example", example_path)
    module = importlib.util.module_from_spec(spec)
    sys.dont_write_bytecode = True
    spec.loader.exec_module(module)
    if len(sys.argv) > 2 and sys.argv[2] == "--client":
        exercise(module.ObjectRoadSource, int(sys.argv[3]))
        return
    source = module.ObjectRoadSource()
    fills = []

    def tracked_fill(layer):
        """Count backend conversions without modifying the documented example."""
        fills.append(layer.partition_id().object_id)
        source.fill(layer)

    source.server.on_tile_feature_request(tracked_fill)
    source.server.go("127.0.0.1")
    try:
        # A separate process avoids sharing Drogon's global application between two servers.
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        with tempfile.TemporaryFile(mode="w+") as log:
            process = subprocess.Popen([
                sys.argv[1], "serve", "--host", "127.0.0.1", "-p", str(port),
                "--worker-count", "2", "-d", f"127.0.0.1:{source.server.port()}"],
                stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 20
                while True:
                    if process.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError("Example service failed to become ready")
                    try:
                        with urllib.request.urlopen(f"http://127.0.0.1:{port}/sources", timeout=1) as r:
                            sources = json.load(r)
                        if any(item.get("mapId") == source.MAP for item in sources):
                            break
                    except (urllib.error.URLError, TimeoutError):
                        pass
                    time.sleep(0.05)
                check_discovery(source, port, fills)
                # Synchronous client requests must not hold the GIL needed by in-process fills.
                subprocess.run([sys.executable, __file__, sys.argv[1], "--client", str(port)],
                               check=True, timeout=30)
                assert fills == [source.OBJECT_ID]  # Binary, JSON, and filters share the cache.
            except BaseException:
                log.seek(0)
                print(log.read(), file=sys.stderr)
                raise
            finally:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    finally:
        source.server.stop()


if __name__ == "__main__":
    main()
