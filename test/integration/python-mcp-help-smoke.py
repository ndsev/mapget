#!/usr/bin/env python3
"""Verify that an installed wheel serves its bundled help from an unrelated working directory."""

import http.client
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

import mapget


def main():
    """Discover the module-relative Markdown tree through the actual Python CLI/MCP endpoint."""
    package = Path(mapget.__file__).resolve().parent
    assert (package / "mcp-help/mapget/mapget-mcp.md").is_file()
    assert (package / "mcp-help/simfil/simfil-language.md").is_file()
    assert not (package / ".mcp-help-sources.json").exists()
    with tempfile.TemporaryDirectory(prefix="mapget-wheel-help-") as temp:
        config = Path(temp) / "service.yaml"
        config.write_text("sources: []\n")
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        with tempfile.TemporaryFile(mode="w+") as log:
            process = subprocess.Popen([
                sys.executable, "-m", "mapget", "--config", str(config), "serve",
                "--host", "127.0.0.1", "--port", str(port), "--mcp", "local",
                "--no-location", "--worker-count", "2"], cwd=temp, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 20
                while process.poll() is None:
                    try:
                        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
                        connection.request("POST", "/mcp", json.dumps({
                            "jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {
                                "name": "mapget_docs", "arguments": {"query": "cardinality"}}}), {
                            "Content-Type": "application/json", "Accept": "application/json, text/event-stream",
                            "MCP-Protocol-Version": "2025-11-25"})
                        response = connection.getresponse()
                        payload = response.read().decode()
                        connection.close()
                        assert response.status == 200, payload
                        message = json.loads(next(line[6:] for line in payload.splitlines() if line.startswith("data: ")))
                        result = message["result"]["structuredContent"]
                        assert result["complete"], result
                        assert result["revision"]
                        assert any(item.get("source", "").startswith("simfil/") for item in result["items"]), result
                        return
                    except OSError:
                        if time.monotonic() >= deadline:
                            break
                        time.sleep(0.05)
                log.seek(0)
                raise AssertionError(log.read())
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == "__main__":
    main()
