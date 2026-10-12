#!/usr/bin/env python3
"""Exercise error TTL configuration through a real service and local GeoJSON source."""

import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request


def check_service(binary: Path, ttl_seconds: int, configured_in_yaml: bool) -> None:
    """Check independent successful/error lifetimes and cached failure reuse."""
    with tempfile.TemporaryDirectory(prefix="mapget-error-ttl-") as directory:
        root = Path(directory)
        (root / "65536.geojson").write_text("invalid JSON", encoding="utf-8")
        (root / "65537.geojson").write_text(
            json.dumps({"type": "FeatureCollection", "features": []}), encoding="utf-8")
        serve = {"ttl": 60}
        if configured_in_yaml:
            serve["error-tile-ttl"] = ttl_seconds
        config = root / "config.yaml"
        config.write_text(json.dumps({
            "mapget": {"serve": serve},
            "sources": [{
                "type": "GeoJsonFolder", "folder": directory,
                "tilePathTemplate": "{tileId}.geojson",
                "dataSourceInfo": {
                    "mapId": "ErrorTtl", "layers": {"Test": {"featureTypes": [{
                        "name": "Test", "uniqueIdCompositions": [[{
                            "partId": "id", "datatype": "I64"}]]}]}}},
            }],
        }), encoding="utf-8")
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        base_url = f"http://127.0.0.1:{port}"
        command = [str(binary), "--config", str(config), "serve", "--host", "127.0.0.1",
                   "--port", str(port), "--worker-count", "1", "--no-location"]
        if not configured_in_yaml and ttl_seconds != 5:
            command += ["--error-tile-ttl", str(ttl_seconds)]
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
                    """Request both tiles so one failed value cannot suppress healthy coverage."""
                    body = {"responseType": "jsonl", "requests": [{
                        "mapId": "ErrorTtl", "layerId": "Test", "tileIds": [65536, 65537]}]}
                    request = urllib.request.Request(base_url + "/tiles", json.dumps(body).encode(),
                                                     {"Content-Type": "application/json"})
                    with urllib.request.urlopen(request, timeout=10) as response:
                        rows = [json.loads(line) for line in response if line.strip()]
                    return {row["mapgetTileId"]: row for row in rows
                            if row.get("type") == "FeatureCollection"}

                tiles = fetch()
                assert tiles[65536]["error"], tiles
                assert tiles[65536]["ttl"] == ttl_seconds * 1000, tiles
                assert not tiles[65537].get("error"), tiles
                assert tiles[65537]["ttl"] == 60000, tiles
                assert fetch()[65536]["timestamp"] == tiles[65536]["timestamp"]
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
    for ttl_seconds, configured_in_yaml in ((5, False), (2, False), (7, True)):
        check_service(binary, ttl_seconds, configured_in_yaml)
    print("Error tile TTL: default, CLI, YAML, successful tiles, and cached failures passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
