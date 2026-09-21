#!/usr/bin/env python3
"""Check real HTTP/1.1 stream framing and close-after-response semantics."""

import gzip
import http.client
import itertools
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

import mapget
from ndslive.math import PackedTileId


class HttpStreamCloseTest(unittest.TestCase):
    """Own a delayed Python datasource and a separate native service process."""

    # Exceed the HTTP handler's 64 KiB drain chunk to detect truncated responses.
    PAYLOAD = "0123456789abcdef" * 8192

    def setUp(self):
        """Start independently owned servers; never hold the GIL in a native client call."""
        self.fills = []
        self.headers_received = threading.Event()
        self.source = mapget.DataSourceServer({
            "mapId": "StreamRegression", "maxParallelJobs": 2,
            "layers": {"Way": {"featureTypes": [{
                "name": "Way", "uniqueIdCompositions": [[{
                    "partId": "wayId", "datatype": "I64"}]]}]}}})
        self.source.on_tile_feature_request(self.fill)
        self.source.go("127.0.0.1")
        self.addCleanup(self.source.stop)
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            self.port = listener.getsockname()[1]
        self.log = tempfile.TemporaryFile(mode="w+")
        self.addCleanup(self.log.close)
        self.process = subprocess.Popen([
            str(MAPGET_BINARY), "serve", "--host", "127.0.0.1", "-p", str(self.port),
            "--worker-count", "2", "-d", f"127.0.0.1:{self.source.port()}"],
            stdout=self.log, stderr=subprocess.STDOUT)
        self.addCleanup(self.stop_service)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline and self.process.poll() is None:
            connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=1)
            try:
                connection.request("GET", "/sources")
                sources = json.loads(connection.getresponse().read())
                if any(item.get("mapId") == "StreamRegression" for item in sources):
                    return
            except (OSError, http.client.HTTPException):
                pass
            finally:
                connection.close()
            time.sleep(0.05)
        self.fail("Native service did not become ready")

    def stop_service(self):
        """Bound teardown even if a regression leaves a stream or worker stuck."""
        self.process.terminate()
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        self.log.seek(0)
        print(self.log.read(), file=sys.stderr)

    def fill(self, tile):
        """Make cold responses asynchronous and record conversions for the cache assertions."""
        # The service must send headers without waiting for datasource completion.
        # Releasing this gate in request() also exercises writes after Drogon has
        # scheduled close-after-response on the HTTP connection.
        if not self.headers_received.wait(timeout=5):
            raise RuntimeError("HTTP headers were not sent before datasource completion")
        tile.set_ttl(60_000)
        tile_id = tile.tile_id().value
        feature = tile.new_feature("Way", [("wayId", tile_id)])
        feature.add_point(mapget.Point(11, 48))
        feature.attributes().add_field("payload", self.PAYLOAD)
        self.fills.append(tile_id)

    def request(self, endpoint, body, close, compressed):
        """Read the complete stream, then verify EOF or reuse the same connection."""
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        self.headers_received.clear()
        try:
            connection.request("POST", endpoint, json.dumps(body), {
                "Content-Type": "application/json",
                "Connection": "close" if close else "keep-alive",
                "Accept-Encoding": "gzip" if compressed else "identity"})
            # http.client closes its socket on a Connection: close response. A duplicate
            # lets us distinguish genuine server EOF from that client-side cleanup.
            with connection.sock.dup() as wire:
                response = connection.getresponse()
                self.headers_received.set()
                self.assertEqual(response.status, 200)
                self.assertEqual(response.getheader("Transfer-Encoding"), "chunked")
                payload = response.read()
                if compressed:
                    self.assertEqual(response.getheader("Content-Encoding"), "gzip")
                    payload = gzip.decompress(payload)
                else:
                    self.assertIsNone(response.getheader("Content-Encoding"))
                if close:
                    self.assertEqual(response.getheader("Connection"), "close")
                    wire.settimeout(5)
                    self.assertEqual(wire.recv(1), b"", "Server did not close after the response")
                else:
                    original_socket = connection.sock
                    connection.request("GET", "/sources")
                    followup = connection.getresponse()
                    self.assertEqual(followup.status, 200)
                    self.assertTrue(json.loads(followup.read()))
                    self.assertIs(connection.sock, original_socket)
                return payload
        finally:
            self.headers_received.set()
            connection.close()

    def check_payload(self, payload, endpoint, response_type, tile_ids):
        """Check JSON values or complete binary framing, including the terminal EOS frame."""
        if response_type == "jsonl":
            records = [json.loads(line) for line in payload.splitlines()]
            layer_type = "FeatureCollection" if endpoint == "/tiles" else "PartitionSubsetLayer"
            layers = [record for record in records if record.get("type") == layer_type]
            self.assertEqual(len(layers), len(tile_ids))
            ids = []
            for layer in layers:
                if endpoint == "/tiles":
                    self.assertEqual(len(layer["features"]), 1)
                    feature = layer["features"][0]
                    ids.append(feature["id"])
                    self.assertEqual(feature["properties"]["payload"], self.PAYLOAD)
                else:
                    self.assertFalse(layer["issues"])
                    entries = layer["channels"][0]["featureEntries"]
                    self.assertEqual(len(entries), 1)
                    ids.append(entries[0]["featureId"])
                    self.assertEqual(entries[0]["values"], [self.PAYLOAD])
            self.assertCountEqual(ids, [f"Way.{tile_id}" for tile_id in tile_ids])
            return
        # VTLV is version (3 uint16), type (uint8), length (uint32), then payload.
        header = struct.Struct("<HHHBI")
        offset = 0
        frames = []
        while offset < len(payload):
            self.assertGreaterEqual(len(payload) - offset, header.size)
            major, minor, patch, kind, size = header.unpack_from(payload, offset)
            self.assertEqual((major, minor), (5, 0))
            offset += header.size + size
            self.assertLessEqual(offset, len(payload), "Truncated VTLV payload")
            frames.append((kind, size))
        self.assertTrue(frames, "Empty binary stream")
        self.assertEqual(frames[-1], (128, 0))
        self.assertEqual(sum(kind == 128 for kind, _ in frames), 1)
        self.assertEqual(sum(kind == (2 if endpoint == "/tiles" else 7)
                             for kind, _ in frames), len(tile_ids))

    def check_disconnect(self, endpoint, body):
        """Disconnect before delayed tiles complete and require stream ownership to drain."""
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        self.headers_received.clear()
        try:
            connection.request("POST", endpoint, json.dumps(body), {
                "Content-Type": "application/json", "Connection": "close"})
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            response.close()
        finally:
            connection.close()
            self.headers_received.set()
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=2)
            try:
                connection.request("GET", "/status-data")
                status = json.loads(connection.getresponse().read())
                if status["tilesHttp"]["active-streams"] == 0:
                    return
            finally:
                connection.close()
            time.sleep(0.05)
        self.fail("Disconnected stream retained response state")

    def test_streams(self):
        """Cover both endpoint paths, encodings, connection policies, and cold/cached tiles."""
        variants = itertools.product(("/tiles", "/filter"), ("jsonl", "binary"),
                                     (False, True), (False, True))
        for index, (endpoint, response_type, close, compressed) in enumerate(variants):
            tile_ids = [PackedTileId.from_tile_xy(index * 2 + i, 0, 8).value for i in range(2)]
            body = {"responseType": response_type, "requests": [{
                "mapId": "StreamRegression", "layerId": "Way", "tileIds": tile_ids}]}
            if endpoint == "/filter":
                body["channels"] = [{"channelId": "ways", "scope": "feature",
                                     "entryFilter": "true", "featureFields": ["properties.payload"]}]
            for cached in (False, True):
                with self.subTest(endpoint=endpoint, format=response_type, close=close,
                                  gzip=compressed, cached=cached):
                    payload = self.request(endpoint, body, close, compressed)
                    self.check_payload(payload, endpoint, response_type, tile_ids)
                    for tile_id in tile_ids:
                        self.assertEqual(self.fills.count(tile_id), 1)
        for index, endpoint in enumerate(("/tiles", "/filter")):
            with self.subTest(disconnect=endpoint):
                disconnect_body = {"responseType": "binary", "requests": [{
                    "mapId": "StreamRegression", "layerId": "Way",
                    "tileIds": [PackedTileId.from_tile_xy(100 + index, 0, 8).value]}]}
                if endpoint == "/filter":
                    disconnect_body["channels"] = [{
                        "channelId": "ways", "scope": "feature", "entryFilter": "true"}]
                self.check_disconnect(endpoint, disconnect_body)


if __name__ == "__main__":
    MAPGET_BINARY = Path(sys.argv.pop(1)).resolve()
    unittest.main()
