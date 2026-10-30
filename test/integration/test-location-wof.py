#!/usr/bin/env python3
"""Test plazs-backed mapget REST/MCP on an isolated port; no GIS/import dependencies."""
from concurrent.futures import ThreadPoolExecutor
import http.client
import json
from pathlib import Path
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest
from urllib.parse import urlencode

MAPGET = Path(sys.argv.pop(1)).resolve()
DATABASE = Path(sys.argv.pop(1)).resolve()
POLYGON = {"type":"Polygon", "coordinates":[
    [[10,47],[12,47],[12,49],[10,49],[10,47]],
    [[10.5,47.5],[10.5,48],[11,48],[11,47.5],[10.5,47.5]]
]}
MULTIPOLYGON = {"type":"MultiPolygon","coordinates":[POLYGON["coordinates"],[
    [[13,47],[14,47],[14,48],[13,48],[13,47]]]]}

def assert_boundary(test, actual, expected):
    """Compare ring membership independent of GEOS rotation; precision/topology tests live in plazs."""
    test.assertEqual(actual["type"],expected["type"])
    def normalized(geometry):
        polygons = geometry["coordinates"] if geometry["type"] == "MultiPolygon" else [geometry["coordinates"]]
        result=[]
        for polygon in polygons:
            rings=[]
            for ring in polygon:
                test.assertEqual(ring[0],ring[-1])
                rings.append(sorted((round(x,4),round(y,4)) for x,y in ring[:-1]))
            result.append(sorted(rings))
        return sorted(result)
    test.assertEqual(normalized(actual),normalized(expected))

class WofHttpTests(unittest.TestCase):
    """Read the imported database through the real native server on a disposable loopback port."""

    @classmethod
    def setUpClass(cls):
        """Start one isolated backend for REST and MCP parity, never the user's live service."""
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.database = Path(cls.directory.name) / "places.sqlite"
        shutil.copyfile(DATABASE, cls.database)
        with socket.socket() as port:
            port.bind(("127.0.0.1", 0))
            cls.port = port.getsockname()[1]
        cls.log = tempfile.TemporaryFile()
        cls.addClassCleanup(cls.log.close)
        cls.process = subprocess.Popen([str(MAPGET), "serve", "--host", "127.0.0.1", "-p", str(cls.port),
                                        "--worker-count", "2", "--location-db", str(cls.database), "--mcp", "local"],
                                       stdout=cls.log, stderr=subprocess.STDOUT)
        cls.addClassCleanup(cls.stop)
        for _ in range(200):
            try:
                connection = http.client.HTTPConnection("127.0.0.1", cls.port, timeout=1)
                connection.request("GET", "/mcp/info")
                response = connection.getresponse()
                response.read()
                connection.close()
                if response.status == 200:
                    return
            except OSError:
                pass
            time.sleep(0.05)
        cls.log.seek(0)
        raise RuntimeError(cls.log.read().decode())

    @classmethod
    def stop(cls):
        """Always reap the child before deleting its open SQLite database on Windows."""
        cls.process.terminate()
        try:
            cls.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            cls.process.kill()
            cls.process.wait()

    def location(self, **query):
        """Read the established array response shape including non-200 errors."""
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        self.addCleanup(connection.close)
        connection.request("GET", "/location?" + urlencode(query))
        response = connection.getresponse()
        return response.status, json.loads(response.read())

    def test_names_types_extents_and_aliases(self):
        """Countries/states resolve translated names, not just settlement centroids."""
        status, matches = self.location(name="Allemagne")
        self.assertEqual(status, 200)
        self.assertEqual(matches[0]["id"], "wof:1")
        self.assertEqual(matches[0]["placeType"], "country")
        self.assertEqual(matches[0]["aabb"], [[10, 47], [4, 2]])
        self.assertTrue(matches[0]["geometryAvailable"])
        self.assertNotIn("geometry", matches[0])
        self.assertEqual(self.location(name="munchen")[1][0]["id"], "wof:3")
        self.assertNotIn("current", self.location(name="Bavaria")[1][0])

    def test_exact_identity_polygon_holes_islands_and_points(self):
        """ID geometry is complete and never reconstructed from a point's extent."""
        assert_boundary(self, self.location(id="wof:1")[1][0]["geometry"], POLYGON)
        assert_boundary(self, self.location(id="wof:2")[1][0]["geometry"], MULTIPOLYGON)
        point = self.location(id="wof:3")[1][0]
        self.assertFalse(point["geometryAvailable"])
        self.assertNotIn("geometry", point)
        self.assertEqual(self.location(id="wof:9")[1][0]["aabb"], [[170, 47], [15, 2]])
        self.assertEqual(self.location(id="wof:999")[1], [])
        self.assertEqual(self.location(id="geonames:1")[1], [])

    def test_rejects_ambiguous_or_oversized_input(self):
        """Bad selectors cannot turn ordinary name search into a bulk boundary request."""
        for query in ({"name": "Germany", "geometry": "true"}, {"name": "Germany", "id": "wof:1"},
                      {"id": "wof:1", "geometry": "yes"}, {"name": "x" * 201}):
            self.assertEqual(self.location(**query)[0], 400)
        for ident in ("wof:", "wof:-1", "wof:1junk", "wof:99999999999999999999"):
            self.assertEqual(self.location(id=ident), (200, []))

    def test_large_geometry_is_rejected_without_hiding_metadata(self):
        """The size check occurs before reading the boundary blob, not after allocating it."""
        with sqlite3.connect(self.database) as db:
            original = db.execute("SELECT size FROM boundary WHERE id=1").fetchone()[0]
            db.execute("UPDATE boundary SET size=17*1024*1024 WHERE id=1")
        try:
            self.assertEqual(self.location(name="Germany")[0], 200)
            self.assertEqual(self.location(id="wof:1")[0], 413)
        finally:
            with db:
                db.execute("UPDATE boundary SET size=? WHERE id=1", (original,))
            db.close()

    def test_concurrent_metadata_and_geometry_reads(self):
        """Shared SQLite ownership is safe across homogeneous service workers."""
        with ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(lambda _: self.location(id="wof:2"), range(16)))
        for status, matches in results:
            self.assertEqual(status, 200)
            assert_boundary(self, matches[0]["geometry"], MULTIPOLYGON)

    def test_mcp_exact_boundary_parity(self):
        """MCP resolves the same identity and polygon under its existing result budget."""
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        self.addCleanup(connection.close)
        body = {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {
            "name": "mapget_get_place_geometry", "arguments": {"id": "wof:2"}}}
        connection.request("POST", "/mcp", json.dumps(body), {
            "Content-Type": "application/json", "Accept": "application/json, text/event-stream", "MCP-Protocol-Version": "2025-11-25"})
        response = connection.getresponse()
        payload = response.read().decode()
        self.assertEqual(response.status, 200, payload)
        reply = json.loads(next(line[5:].strip() for line in payload.splitlines() if line.startswith("data:"))) if "text/event-stream" in response.getheader("Content-Type", "") else json.loads(payload)
        result = reply["result"]["structuredContent"]
        self.assertTrue(result["complete"], result)
        assert_boundary(self, result["items"][0]["geometry"], MULTIPOLYGON)


if __name__ == "__main__":
    unittest.main()
