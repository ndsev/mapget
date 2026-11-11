#!/usr/bin/env python3
"""Exercise error/empty TTL configuration through a real service and local GeoJSON source."""

import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request


def check_service(binary: Path, error_ttl: int, empty_ttl: int, configured_in_yaml: bool,
                  source_ttls: tuple[int, int] | None = None) -> None:
    """Check independent nonempty/empty/error lifetimes, cache hits and empty recovery."""
    with tempfile.TemporaryDirectory(prefix="mapget-tile-ttl-") as directory:
        root = Path(directory)
        (root / "65536.geojson").write_text("invalid JSON", encoding="utf-8")
        (root / "65537.geojson").write_text(
            json.dumps({"type": "FeatureCollection", "features": []}), encoding="utf-8")
        populated = json.dumps({"type": "FeatureCollection", "features": [{
            "type": "Feature", "properties": {},
            "geometry": {"type": "Point", "coordinates": [0, 0]},
        }]})
        (root / "262144.geojson").write_text(populated, encoding="utf-8")
        serve = {"ttl": 60}
        if configured_in_yaml:
            serve.update({"error-tile-ttl": error_ttl, "empty-tile-ttl": empty_ttl})
        config = root / "config.yaml"
        settings = {
            "mapget": {"serve": serve},
            "sources": [{
                "type": "GeoJsonFolder", "folder": directory,
                "tilePathTemplate": "{tileId}.geojson",
                "dataSourceInfo": {
                    "mapId": "ErrorTtl", "layers": {"Test": {"featureTypes": [{
                        "name": "Test", "uniqueIdCompositions": [[{
                            "partId": "id", "datatype": "I64"}]]}]}}},
            }],
        }
        if source_ttls is not None:
            settings["sources"][0].update(
                {"error-tile-ttl": source_ttls[0], "empty-tile-ttl": source_ttls[1]})
        config.write_text(json.dumps(settings), encoding="utf-8")
        expected_error, expected_empty = source_ttls or (error_ttl, empty_ttl)
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        base_url = f"http://127.0.0.1:{port}"
        command = [str(binary), "--config", str(config), "serve", "--host", "127.0.0.1",
                   "--port", str(port), "--worker-count", "1", "--no-location",
                   "--allow-cache-reset", "--cache-reset-auth-header", "x-reset=^test$"]
        if not configured_in_yaml and (error_ttl, empty_ttl) != (5, 3600):
            command += ["--error-tile-ttl", str(error_ttl), "--empty-tile-ttl", str(empty_ttl)]
        with tempfile.TemporaryFile(mode="w+") as log:
            process = subprocess.Popen(command, cwd=root, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        log.seek(0)
                        raise AssertionError(f"Service exited early:\n{log.read()}")
                    try:
                        with urllib.request.urlopen(base_url + "/sources", timeout=2) as response:
                            sources = json.load(response)
                        if any(source.get("status") == "ready" for source in sources):
                            break
                    except OSError:
                        pass
                    time.sleep(0.05)
                else:
                    log.seek(0)
                    raise AssertionError(f"Service did not become ready:\n{log.read()}")

                def fetch():
                    """Fetch all outcomes together so failure cannot suppress healthy coverage."""
                    body = {"responseType": "jsonl", "requests": [{
                        "mapId": "ErrorTtl", "layerId": "Test", "tileIds": [65536, 65537, 262144]}]}
                    request = urllib.request.Request(base_url + "/tiles", json.dumps(body).encode(),
                                                     {"Content-Type": "application/json"})
                    with urllib.request.urlopen(request, timeout=10) as response:
                        rows = [json.loads(line) for line in response if line.strip()]
                    return {row["mapgetTileId"]: row for row in rows
                            if row.get("type") == "FeatureCollection"}

                tiles = fetch()
                assert tiles[65536]["error"], tiles
                assert tiles[65536]["ttl"] == expected_error * 1000, tiles
                assert not tiles[65537].get("error"), tiles
                assert not tiles[65537]["features"], tiles
                assert tiles[65537]["ttl"] == expected_empty * 1000, tiles
                assert not tiles[262144].get("error"), tiles
                assert len(tiles[262144]["features"]) == 1, tiles
                assert tiles[262144]["ttl"] == 60000, tiles
                cached = fetch()
                for tile_id in tiles:
                    assert cached[tile_id]["timestamp"] == tiles[tile_id]["timestamp"]
                if expected_error == 0 and expected_empty == 0:
                    # Changing the underlying data must not bypass non-expiring cache
                    # entries. Explicit reset, not an expiry notification, reloads them.
                    (root / "65536.geojson").write_text(populated, encoding="utf-8")
                    (root / "65537.geojson").write_text(populated, encoding="utf-8")
                    cached = fetch()
                    assert cached[65536]["error"]
                    assert not cached[65537]["features"]
                    request = urllib.request.Request(
                        base_url + "/cache/reset", json.dumps({"mapId": "ErrorTtl"}).encode(),
                        {"Content-Type": "application/json", "x-reset": "test"})
                    with urllib.request.urlopen(request, timeout=10) as response:
                        assert response.status == 204
                    for tile in fetch().values():
                        assert not tile.get("error"), tile
                        assert len(tile["features"]) == 1, tile
                        assert tile["ttl"] == 60000, tile
                if expected_empty == 1:
                    # Reacquire the previously empty tile after its configured expiry.
                    (root / "65537.geojson").write_text(populated, encoding="utf-8")
                    time.sleep(1.1)
                    recovered = fetch()[65537]
                    assert len(recovered["features"]) == 1, recovered
                    assert recovered["ttl"] == 60000, recovered
                    assert recovered["timestamp"] > tiles[65537]["timestamp"], recovered
            finally:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


def main() -> int:
    """Check the default, CLI override, and YAML override without a Python wheel."""
    binary = Path(sys.argv[1]).resolve()
    for error_ttl, empty_ttl, configured_in_yaml in (
            (5, 3600, False), (2, 3, False), (7, 1, True), (0, 0, False), (0, 0, True)):
        check_service(binary, error_ttl, empty_ttl, configured_in_yaml)
    check_service(binary, 2, 3, True, source_ttls=(0, 0))
    check_service(binary, 0, 0, True, source_ttls=(2, 1))
    print("Tile TTLs: defaults, global/source policies, zero, cache reset and recovery passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
