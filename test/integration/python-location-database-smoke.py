#!/usr/bin/env python3
"""Verify that an installed mapget wheel can use its bundled location database."""

from __future__ import annotations

import json
from pathlib import Path
import urllib.parse
import urllib.request

import mapget


def main() -> int:
    """Start the wheel's service and query the default GeoNames database."""
    package_dir = Path(mapget.__file__).resolve().parent
    assert (package_dir / "geonames-cities5000.sqlite").is_file()
    assert (package_dir / "geonames-readme.txt").is_file()

    service = mapget.Service()
    service.go("127.0.0.1")
    try:
        query = urllib.parse.urlencode({"name": "Munich", "limit": 1})
        with urllib.request.urlopen(
            f"http://127.0.0.1:{service.port()}/location?{query}", timeout=10
        ) as response:
            matches = json.loads(response.read().decode("utf-8"))

        assert matches
        assert matches[0]["name"] == "Munich, DE"
        assert matches[0]["source"] == "geonames-cities5000"
    finally:
        service.stop()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
