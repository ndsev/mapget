#!/usr/bin/env python3
"""Exercise the native MCP HTTP/interactive boundary without Python bindings or an external IdP."""

import base64
import http.client
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import uuid


class ViewerConnection:
    """A minimal real WebSocket peer; application behavior remains explicitly controlled by tests."""

    def __init__(self, port, endpoint="/interactive"):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=6)
        self.buffer = b""
        key = base64.b64encode(os.urandom(16)).decode()
        self.socket.sendall((
            f"GET {endpoint} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n"
            f"Origin: http://127.0.0.1:{port}\r\nUpgrade: websocket\r\n"
            f"Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
            f"Sec-WebSocket-Key: {key}\r\n\r\n").encode())
        while b"\r\n\r\n" not in self.buffer:
            self.buffer += self.socket.recv(4096)
        headers, self.buffer = self.buffer.split(b"\r\n\r\n", 1)
        assert headers.startswith(b"HTTP/1.1 101"), headers
        context = self.receive(6)
        assert context["requestId"] == 0
        self.client_id = context["clientId"]
        assert str(uuid.UUID(self.client_id, version=4)) == self.client_id

    def close(self):
        """Close the transport even if an action has not produced a result."""
        self.socket.close()

    def exact(self, size):
        """Retain bytes already read with the handshake or a preceding frame."""
        while len(self.buffer) < size:
            data = self.socket.recv(max(4096, size - len(self.buffer)))
            if not data:
                raise EOFError("Viewer socket closed")
            self.buffer += data
        result, self.buffer = self.buffer[:size], self.buffer[size:]
        return result

    def send_frame(self, payload, opcode=1):
        """Client frames are always masked, including pong replies."""
        mask = os.urandom(4)
        size = len(payload)
        header = bytes([0x80 | opcode])
        if size < 126:
            header += bytes([0x80 | size])
        elif size <= 65535:
            header += bytes([0x80 | 126]) + struct.pack("!H", size)
        else:
            header += bytes([0x80 | 127]) + struct.pack("!Q", size)
        self.socket.sendall(header + mask + bytes(
            value ^ mask[index % 4] for index, value in enumerate(payload)))

    def send(self, message):
        """Send one browser-owned action control as a JSON text frame."""
        self.send_frame(json.dumps(message).encode())

    def receive(self, kind=9):
        """Read through incidental status/ping frames to the requested mapget control type."""
        while True:
            flags, size = self.exact(2)
            assert flags & 0x80 and not size & 0x80
            size &= 127
            if size == 126:
                size = struct.unpack("!H", self.exact(2))[0]
            elif size == 127:
                size = struct.unpack("!Q", self.exact(8))[0]
            assert size < 1024 * 1024
            payload = self.exact(size)
            opcode = flags & 15
            if opcode == 9:
                self.send_frame(payload, 10)
                continue
            if opcode == 8:
                raise EOFError("Viewer received a close frame")
            assert opcode == 2
            major, minor, patch, message_type, length = struct.unpack_from("<HHHBI", payload)
            assert (major, minor) == (5, 3)
            assert len(payload) == 11 + length
            if message_type == kind:
                return json.loads(payload[11:])


class NativeMcpTest(unittest.TestCase):
    """Own an isolated loopback CLI, trusted catalog and real HTTP/WS connections per test."""

    def setUp(self):
        """Use explicit local opt-in; never touch a developer's live server or MCP configuration."""
        self.directory = tempfile.TemporaryDirectory(prefix="mapget-mcp-test-")
        self.addCleanup(self.directory.cleanup)
        self.log = tempfile.TemporaryFile(mode="w+", encoding="utf-8")
        self.addCleanup(self.log.close)
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            self.port = listener.getsockname()[1]
        catalog_path = Path(__file__).parent.parent / "unit/data/viewer-actions/web-mcp-actions.json"
        self.catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
        self.fixtures = json.loads((catalog_path.parent / "fixtures.json").read_text(encoding="utf-8"))
        self.catalog_path = Path(self.directory.name) / "web-mcp-actions.json"
        self.catalog_path.write_bytes(catalog_path.read_bytes())
        webapp_options = ["--webapp", self.directory.name]
        if self._testMethodName == "test_headless_native_catalog":
            webapp_options = []
            self.catalog = {"catalogId": "sha256:" + "0" * 64, "actions": []}
        self.config_path = Path(self.directory.name) / "service.yaml"
        self.config_path.write_text('sources: []\nprivate-host-setting: preserve-me\n')
        if self._testMethodName == "test_native_grid_extraction":
            self.config_path.write_text(json.dumps({"sources": [{
                "type": "GridDataSource", "mapId": "NativeGrid", "layers": [
                    {"name": "Road", "featureType": "DevSrc-Road", "geometry": {"type": "line"}},
                    {"name": "Intersections", "featureType": "DevSrc-Intersection",
                     "geometry": {"type": "point"}}]}]}))
        if self._testMethodName == "test_screenshot_complete_output_budget":
            self.extra_options = ["--mcp-result-bytes", "8192"]
        if self._testMethodName == "test_native_config_revision_and_persistence":
            self.extra_options = ["--allow-post-config", "--mcp-config-read", "true",
                                  "--mcp-config-write", "true", "--mcp-direct-config-persistence", "true"]
            self.config_path.write_text(json.dumps({"sources": [{"type": "GridDataSource", "enabled": False,
                                                               "password": "fake-regression-secret"}],
                                                   "private-host-setting": "preserve-me"}))
        self.process = subprocess.Popen([
            str(MAPGET_BINARY), "--config", str(self.config_path), "serve", "--host", "127.0.0.1", "-p", str(self.port),
            "--no-location", "--worker-count", "2", "--mcp", "local",
            "--mcp-timeout-ms", "2000", *webapp_options,
            *getattr(self, "extra_options", [])],
            stdout=self.log, stderr=subprocess.STDOUT)
        self.addCleanup(self.stop_service)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline and self.process.poll() is None:
            try:
                status, body = self.http("GET", "/mcp/info")
                self.assertEqual(status, 200)
                self.assertEqual(body["catalogId"], self.catalog["catalogId"])
                return
            except OSError:
                time.sleep(0.05)
        self.fail("Native MCP service did not start")

    def stop_service(self):
        """Bound shutdown and expose native logs when this standalone test is run by CI."""
        self.process.terminate()
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        self.log.seek(0)
        print(self.log.read(), file=sys.stderr)

    def connection(self):
        """Register each HTTP connection for cleanup, including unfinished SSE streams."""
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=6)
        self.addCleanup(connection.close)
        return connection

    def http(self, method, path, body=None, headers=None):
        """Read one finite JSON response, preserving status for protocol rejection assertions."""
        connection = self.connection()
        connection.request(method, path, body, headers or {})
        response = connection.getresponse()
        payload = response.read()
        connection.close()
        self.assertEqual(response.getheader("Cache-Control"), "no-store")
        return response.status, json.loads(payload) if payload else None

    def begin_rpc(self, method, params=None, headers=None, protocol="2026-07-28", request_id=1):
        """Start either supported MCP lifecycle; callers may defer reading its SSE result."""
        params = dict(params or {})
        defaults = {"Content-Type": "application/json",
                    "Accept": "application/json, text/event-stream",
                    "MCP-Protocol-Version": protocol}
        if protocol == "2026-07-28":
            params["_meta"] = {
                "io.modelcontextprotocol/protocolVersion": protocol,
                "io.modelcontextprotocol/clientInfo": {"name": "mapget-test", "version": "1"},
                "io.modelcontextprotocol/clientCapabilities": {}}
            defaults["Mcp-Method"] = method
            if method == "tools/call":
                defaults["Mcp-Name"] = params["name"]
        defaults.update(headers or {})
        connection = self.connection()
        message = {"jsonrpc": "2.0", "method": method, "params": params}
        if request_id is not None:
            message["id"] = request_id
        connection.request("POST", "/mcp", json.dumps(message), defaults)
        return connection, connection.getresponse()

    def rpc_result(self, response):
        """Require one terminal JSON-RPC response, not just successful HTTP/SSE headers."""
        payload = response.read().decode()
        if response.getheader("Content-Type").startswith("text/event-stream"):
            messages = [line[6:] for line in payload.splitlines() if line.startswith("data: ")]
            self.assertEqual(len(messages), 1)
            payload = messages[0]
        return json.loads(payload)

    def call(self, action, arguments):
        """Read a native/immediately rejected tool call without a browser-side result."""
        _, response = self.begin_rpc("tools/call", {"name": action, "arguments": arguments})
        self.assertEqual(response.status, 200)
        return self.rpc_result(response)["result"]

    def viewer(self, endpoint="/interactive"):
        """Register only trusted action names, never browser-supplied schemas or identity."""
        viewer = ViewerConnection(self.port, endpoint)
        self.addCleanup(viewer.close)
        viewer.send({"type": "mapget.actions.register", "version": 1,
                     "catalogId": self.catalog["catalogId"], "label": "Native regression",
                     "actions": [action["name"] for action in self.catalog["actions"]]})
        self.assertEqual(viewer.receive()["type"], "mapget.actions.registered")
        return viewer

    def arguments(self, viewer, action):
        """Use the same positive contract fixtures as frontend and native schema validation."""
        fixture = next(item for item in self.fixtures["actions"]
                       if item["action"] == action and item["valid"])
        return dict(fixture["arguments"], clientId=viewer.client_id)

    def finish(self, viewer, invocation, result=None):
        """Send a fixture or explicit test result for exactly the dispatched call identity."""
        if result is None:
            result = next(item["value"] for item in self.fixtures["results"]
                          if item["action"] == invocation["action"] and item["valid"])
        message = {"type": "mapget.actions.result", "version": 1,
                   "callId": invocation["callId"], "result": result}
        viewer.send(message)
        return message

    def test_protocol_and_exposure(self):
        """Check discovery, native and viewer tools, malformed metadata and local Host/Origin boundaries."""
        for method in ("server/discover", "tools/list"):
            _, response = self.begin_rpc(method)
            self.assertEqual(response.status, 200)
            result = self.rpc_result(response)["result"]
            self.assertEqual(result["resultType"], "complete")
            if method == "tools/list":
                self.assertEqual(len(result["tools"]), len(self.catalog["actions"]) + 13)
            else:
                self.assertEqual(result["supportedVersions"], ["2026-07-28"])
        self.assertEqual(self.call("viewer_list_sessions", {})["structuredContent"], {"sessions": []})
        for headers, code in (({"Mcp-Method": "tools/call"}, -32020),
                              ({"MCP-Protocol-Version": "2024-11-05"}, -32022)):
            _, response = self.begin_rpc("tools/list", headers=headers)
            self.assertEqual(response.status, 400)
            self.assertEqual(self.rpc_result(response)["error"]["code"], code)
        for headers in ({"Host": "attacker.example"}, {"Origin": "https://attacker.example"},
                        {"X-Forwarded-For": "127.0.0.1"}):
            self.assertEqual(self.http("GET", "/mcp/info", headers=headers)[0], 403)
        self.assertEqual(self.http("GET", "/mcp")[0], 405)

    def test_catalog_reload_without_restart(self):
        """A rebuild changes discovery/registration on the same process and interactive socket."""
        viewer = self.viewer()
        old_catalog_id = self.catalog["catalogId"]
        args = self.arguments(viewer, "viewer_get_app_state")
        _, pending = self.begin_rpc("tools/call", {"name": "viewer_get_app_state", "arguments": args})
        invocation = viewer.receive()
        self.catalog["catalogId"] = "sha256:" + "b" * 64
        self.catalog["actions"] = [action for action in self.catalog["actions"]
                                   if action["name"] != "viewer_get_app_state"]
        self.catalog_path.write_text(json.dumps(self.catalog), encoding="utf-8")
        self.assertEqual(self.http("GET", "/mcp/info")[1]["catalogId"], self.catalog["catalogId"])
        retired = viewer.receive()
        self.assertEqual(retired["error"]["reason"], "catalog_changed")
        self.assertEqual(self.call("viewer_list_sessions", {})["structuredContent"]["sessions"], [])
        # Even an action removed from the replacement catalog can finish its accepted invocation.
        self.finish(viewer, invocation)
        self.assertFalse(self.rpc_result(pending)["result"]["isError"])
        _, response = self.begin_rpc("tools/list")
        names = {tool["name"] for tool in self.rpc_result(response)["result"]["tools"]}
        self.assertNotIn("viewer_get_app_state", names)
        self.assertIn("mapget_convert_tile_id", names)
        registration = {"type": "mapget.actions.register", "version": 1,
                        "catalogId": old_catalog_id, "label": "Old build", "actions": []}
        viewer.send(registration)
        self.assertEqual(viewer.receive()["type"], "mapget.actions.error")
        registration.update(catalogId=self.catalog["catalogId"], label="Rebuilt viewer",
                            actions=[action["name"] for action in self.catalog["actions"]])
        viewer.send(registration)
        self.assertEqual(viewer.receive()["type"], "mapget.actions.registered")
        sessions = self.call("viewer_list_sessions", {})["structuredContent"]["sessions"]
        self.assertEqual([session["clientId"] for session in sessions], [viewer.client_id])
        # Fresh calls also succeed without a new HTTP service or WebSocket handshake.
        _, response = self.begin_rpc("tools/call", {"name": "viewer_set_app_state",
                                    "arguments": self.arguments(viewer, "viewer_set_app_state")})
        self.finish(viewer, viewer.receive())
        self.assertFalse(self.rpc_result(response)["result"]["isError"])
        self.assertIsNone(self.process.poll())

    def test_catalog_reload_keeps_last_good_and_recovers(self):
        """Incomplete/invalid builds cannot invalidate working tabs; discovery retries changed files."""
        viewer = self.viewer()
        original = self.catalog_path.read_bytes()
        invalid_schema = json.loads(original)
        invalid_schema["catalogId"] = "sha256:" + "c" * 64
        invalid_schema["actions"][0]["inputSchema"]["additionalProperties"] = True
        for data in (None, b'{"formatVersion":', json.dumps(invalid_schema).encode()):
            with self.subTest(data="missing" if data is None else "invalid"):
                if data is None:
                    self.catalog_path.unlink()
                else:
                    self.catalog_path.write_bytes(data)
                for _ in range(2):
                    self.assertEqual(self.http("GET", "/mcp/info")[1]["catalogId"], self.catalog["catalogId"])
                    sessions = self.call("viewer_list_sessions", {})["structuredContent"]["sessions"]
                    self.assertEqual([s["clientId"] for s in sessions], [viewer.client_id])
        self.catalog_path.write_bytes(original)
        self.assertEqual(self.http("GET", "/mcp/info")[1]["catalogId"], self.catalog["catalogId"])
        # A byte-identical rebuild does not retire registration or enqueue any notification.
        _, response = self.begin_rpc("tools/call", {"name": "viewer_get_app_state",
                                    "arguments": self.arguments(viewer, "viewer_get_app_state")})
        invocation = viewer.receive()
        self.assertEqual(invocation["type"], "mapget.actions.invoke")
        self.finish(viewer, invocation)
        self.assertFalse(self.rpc_result(response)["result"]["isError"])
        # Neither discovery nor registration depends on a preceding /mcp/info fetch.
        self.catalog["catalogId"] = "sha256:" + "d" * 64
        self.catalog["actions"][0]["description"] = "Reloaded browser action"
        self.catalog_path.write_text(json.dumps(self.catalog), encoding="utf-8")
        _, response = self.begin_rpc("tools/list")
        tools = self.rpc_result(response)["result"]["tools"]
        self.assertEqual(next(t for t in tools if t["name"] == self.catalog["actions"][0]["name"])["description"],
                         "Reloaded browser action")
        self.assertEqual(viewer.receive()["error"]["reason"], "catalog_changed")
        self.catalog["catalogId"] = "sha256:" + "e" * 64
        self.catalog_path.write_text(json.dumps(self.catalog), encoding="utf-8")
        self.viewer()
        self.assertEqual(self.http("GET", "/mcp/info")[1]["catalogId"], self.catalog["catalogId"])

    def test_native_tools_without_browser(self):
        """Exercise actual native SSE replies in every supported protocol revision."""
        for protocol in ("2025-06-18", "2025-11-25", "2026-07-28"):
            _, response = self.begin_rpc("tools/call", {
                "name": "mapget_convert_tile_id", "arguments": {"tileId": 131073}}, protocol=protocol)
            result = self.rpc_result(response)["result"]
            self.assertFalse(result.get("isError", False))
            self.assertEqual(result["structuredContent"]["items"][0]["tileId"], 131073)
            self.assertEqual(json.loads(result["content"][0]["text"]), result["structuredContent"])
        self.assertTrue(self.call("mapget_lookup_place", {"name": "Munich"})["isError"])
        self.assertTrue(self.call("mapget_get_config", {})["isError"])
        self.assertEqual(self.call("mapget_list_sources", {})["structuredContent"]["items"], [])
        diagnostics = self.call("mapget_get_diagnostics", {"sections": ["workers"]})["structuredContent"]
        self.assertTrue(diagnostics["items"])

    def test_documentation_hot_reload_and_protocols(self):
        """Source edits are visible on the next call without CMake, reconnect or a browser."""
        folder = Path(self.directory.name) / "mcp-help"
        folder.mkdir()
        guide = folder / "guide.md"
        guide.write_text("<!-- mcp: -->\n# Quokkaroute\nFirst version.\n", encoding="utf-8")
        for protocol in ("2025-06-18", "2025-11-25", "2026-07-28"):
            _, response = self.begin_rpc("tools/call", {
                "name": "mapget_docs", "arguments": {"query": "quokkaroute"}}, protocol=protocol)
            result = self.rpc_result(response)["result"]
            self.assertFalse(result.get("isError", False), result)
            docs = result["structuredContent"]
            self.assertTrue(docs["complete"], docs)
            self.assertEqual(len(docs["items"]), 1)
            self.assertIn("First version", docs["items"][0]["content"])
            self.assertEqual(json.loads(result["content"][0]["text"]), docs)
        title = docs["items"][0]["title"]
        guide.write_text("<!-- mcp: -->\n# Quokkaroute\nLater version.\n", encoding="utf-8")
        updated = self.call("mapget_docs", {"title": title})["structuredContent"]
        self.assertNotEqual(updated["revision"], docs["revision"])
        self.assertIn("Later version", updated["items"][0]["content"])
        guide.write_text("<!-- mcp:\nbroken: annotation\n-->\n# Quokkaroute\n", encoding="utf-8")
        failed = self.call("mapget_docs", {"title": title})["structuredContent"]
        self.assertEqual(failed["revision"], updated["revision"])
        self.assertEqual(failed["items"], updated["items"])
        self.assertEqual(failed["reason"], "docs_reload_failed")
        guide.unlink()
        deleted = self.call("mapget_docs", {"title": title})["structuredContent"]
        self.assertTrue(deleted["complete"])
        self.assertEqual(deleted["items"], [])

    def test_documentation_full_sections_fit_actual_wire_budget(self):
        """Several useful Markdown sections must not pay generic scalar worst-case escaping six times."""
        folder = Path(self.directory.name) / "mcp-help"
        folder.mkdir()
        for i in range(3):
            (folder / f"large-{i}.md").write_text(
                f"<!-- mcp: -->\n# Capybaraguide {i}\n" + ('Example "quoted" field.\n' * 400), encoding="utf-8")
        result = self.call("mapget_docs", {"query": "capybaraguide"})
        self.assertFalse(result.get("isError", False), result)
        docs = result["structuredContent"]
        self.assertTrue(docs["complete"], docs)
        self.assertEqual(len(docs["items"]), 3)
        self.assertTrue(all(len(item["content"]) > 8000 for item in docs["items"]))
        self.assertEqual(json.loads(result["content"][0]["text"]), docs)
        # Actual overflow still returns bounded partial evidence, never a false complete reply.
        (folder / "large-0.md").write_text("<!-- mcp: -->\n# Capybaraguide huge\n" + 'x' * 100000)
        huge = self.call("mapget_docs", {"query": "capybaraguide huge"})
        self.assertFalse(huge.get("isError", False), huge)
        self.assertFalse(huge["structuredContent"]["complete"])
        self.assertEqual(huge["structuredContent"]["reason"], "byte_limit")

    def test_documentation_input_and_work_budgets(self):
        """Help follows native read/schema/budget contracts rather than a separate protocol."""
        _, response = self.begin_rpc("tools/call", {"name": "mapget_docs", "arguments": {
            "query": "arrays", "title": "Arrays"}})
        self.assertIn("error", self.rpc_result(response))
        partial = self.call("mapget_docs", {"query": "arrays", "maxWork": 1})["structuredContent"]
        self.assertFalse(partial["complete"])
        self.assertEqual(partial["reason"], "work_limit")
        complete = self.call("mapget_docs", {"query": "cardinality"})["structuredContent"]
        self.assertTrue(complete["complete"], complete)
        self.assertTrue(any(item.get("source", "").startswith("simfil/") for item in complete["items"]))

    def test_headless_native_catalog(self):
        """A native-only server needs neither a webapp nor a generated browser catalog."""
        _, response = self.begin_rpc("tools/list")
        names = {tool["name"] for tool in self.rpc_result(response)["result"]["tools"]}
        self.assertEqual(len(names), 13)
        self.assertIn("mapget_docs", names)
        self.assertIn("mapget_extract_source_data", names)
        self.assertIn("mapget_get_coverage", names)
        self.assertIn("viewer_list_sessions", names)
        self.assertNotIn("viewer_set_app_state", names)
        self.assertEqual(self.call("mapget_list_sources", {})["structuredContent"]["items"], [])

    def test_native_grid_extraction(self):
        """Load a real datasource through HTTP, then reuse its canonical feature ID for locate."""
        selection = {"mapId": "NativeGrid", "layerId": "Road"}
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            sources = self.call("mapget_list_sources", selection)["structuredContent"]["items"]
            if sources and sources[0]["status"] == "ready":
                break
            time.sleep(0.05)
        else:
            self.fail("Native Grid datasource did not initialize")
        coverage = self.call("mapget_get_coverage", selection)
        self.assertFalse(coverage.get("isError", False), coverage)
        self.assertIn("coverageKnown", coverage["structuredContent"]["items"][0])
        self.assertIn("uniqueIdCompositions", sources[0]["layers"][0]["featureTypes"][0])
        compact = self.call("mapget_list_sources", {**selection, "details": False})
        self.assertNotIn("uniqueIdCompositions", compact["structuredContent"]["items"][0]["layers"][0]["featureTypes"][0])
        detailed = self.call("mapget_list_sources", {**selection, "details": True})
        self.assertIn("uniqueIdCompositions", detailed["structuredContent"]["items"][0]["layers"][0]["featureTypes"][0])
        validation = self.call("mapget_validate_expression", {
            **selection, "expression": "typeId == 'DevSrc-Road'"})
        self.assertFalse(validation.get("isError", False), validation)
        self.assertTrue(validation["structuredContent"]["items"][0]["valid"], validation)
        converted = self.call("mapget_convert_tile_id", {
            "longitude": 11.5, "latitude": 48.1, "level": 13})
        tile_id = converted["structuredContent"]["items"][0]["tileId"]
        reply = self.call("mapget_extract_features", {
            **selection, "partitions": [{"kind": "tile", "id": tile_id}],
            "expressions": ["id", "_"], "limit": 2})
        self.assertFalse(reply.get("isError", False), reply)
        result = reply["structuredContent"]
        self.assertEqual(len(result["items"]), 2, reply)
        self.assertFalse(result["complete"])
        first = result["items"][0]
        self.assertEqual(first["values"][0], [first["featureId"]])
        self.assertIsInstance(first["values"][1][0], dict)
        self.assertIn("nextCursor", result)
        resumed = self.call("mapget_extract_features", {"cursor": result["nextCursor"]})
        self.assertFalse(resumed.get("isError", False), resumed)
        repeated = self.call("mapget_extract_features", {"cursor": result["nextCursor"]})
        self.assertEqual(resumed["structuredContent"]["items"], repeated["structuredContent"]["items"])
        first_ids = {row["featureId"] for row in result["items"] if row["rowComplete"]}
        next_ids = {row["featureId"] for row in resumed["structuredContent"]["items"]}
        self.assertTrue(first_ids.isdisjoint(next_ids))
        located = self.call("mapget_extract_features", {
            **selection, "featureIds": [first["featureId"]], "query": "id"})
        self.assertFalse(located.get("isError", False), located)
        self.assertTrue(located["structuredContent"]["complete"], located)
        self.assertEqual(len(located["structuredContent"]["items"]), 1)
        self.assertEqual(located["structuredContent"]["items"][0]["featureId"], first["featureId"])

    def test_native_config_revision_and_persistence(self):
        """Verify masked read, optimistic write, durable file and preserved unrelated settings."""
        current = self.call("mapget_get_config", {})["structuredContent"]["items"][0]
        self.assertNotIn("fake-regression-secret", json.dumps(current))
        self.assertNotIn("private-host-setting", current["model"])
        model = current["model"]
        model["sources"][0]["ttl"] = 60
        reply = self.call("mapget_set_config", {"model": model, "expectedRevision": current["revision"]})
        self.assertFalse(reply.get("isError", False), reply)
        self.assertTrue(reply["structuredContent"]["items"][0]["persisted"])
        self.assertIn("fake-regression-secret", self.config_path.read_text())

        self.assertIn("preserve-me", self.config_path.read_text())
        conflict = self.call("mapget_set_config", {"model": model, "expectedRevision": current["revision"]})
        self.assertEqual(conflict["structuredContent"]["error"]["code"], "conflict")
        latest = self.call("mapget_get_config", {})["structuredContent"]["items"][0]
        denied = self.call("mapget_set_config", {"model": {"mapget": {"serve": {"mcp": "off"}}},
                                               "expectedRevision": latest["revision"]})
        self.assertTrue(denied["isError"])
        model["sources"][0]["password"] = "MASKED:unknown"
        rejected = self.call("mapget_set_config", {"model": model, "expectedRevision": latest["revision"]})
        self.assertTrue(rejected["isError"])
        self.assertIn("fake-regression-secret", self.config_path.read_text())
        cleared = self.call("mapget_set_config", {"model": {"sources": []}, "expectedRevision": latest["revision"]})
        self.assertFalse(cleared.get("isError", False), cleared)
        empty = self.call("mapget_get_config", {})["structuredContent"]["items"][0]
        self.assertEqual(empty["model"]["sources"], [])

    def test_native_rejects_browser_routing_and_identity_fields(self):
        """Reject extra routing/identity inputs rather than treating them as browser actions."""
        _, response = self.begin_rpc("tools/call", {"name": "mapget_list_sources", "arguments": {"clientId": str(uuid.uuid4())}})
        self.assertEqual(self.rpc_result(response)["error"]["code"], -32602)
        self.assertEqual(self.http("GET", "/.well-known/oauth-protected-resource/mcp")[0], 404)

    def test_screenshot_image_content(self):
        """Image bytes occur once, outside metadata/text, in every supported MCP revision."""
        viewer = self.viewer()
        _, listed = self.begin_rpc("tools/list")
        tool = next(t for t in self.rpc_result(listed)["result"]["tools"]
                    if t["name"] == "viewer_screenshot")
        definition = next(a for a in self.catalog["actions"] if a["name"] == tool["name"])
        self.assertEqual(tool["outputSchema"], definition["outputSchema"]["properties"]["metadata"])
        original = next(r["value"] for r in self.fixtures["results"]
                        if r["action"] == "viewer_screenshot" and r["valid"])
        for protocol in ("2025-06-18", "2025-11-25", "2026-07-28"):
            _, response = self.begin_rpc("tools/call", {
                "name": "viewer_screenshot", "arguments": self.arguments(viewer, "viewer_screenshot")},
                protocol=protocol)
            invocation = viewer.receive()
            payload = json.loads(json.dumps(original))
            if protocol == "2026-07-28":
                # Exercise the maximum encoded payload without a recursive base64 regex.
                payload["image"]["data"] = base64.b64encode(b"x" * 180000).decode()
            self.finish(viewer, invocation, payload)
            result = self.rpc_result(response)["result"]
            self.assertFalse(result.get("isError", False), result.get("structuredContent"))
            self.assertEqual(result["structuredContent"], payload["metadata"])
            self.assertEqual(len(result["content"]), 2)
            image = next(c for c in result["content"] if c["type"] == "image")
            self.assertEqual(image, {"type": "image", **payload["image"]})
            text = next(c["text"] for c in result["content"] if c["type"] == "text")
            self.assertEqual(json.loads(text), payload["metadata"])
            self.assertNotIn("image", result["structuredContent"])

    def test_screenshot_rejects_noncanonical_encoding(self):
        """Invalid padding/alphabet fails one call, without returning the supplied byte string."""
        viewer = self.viewer()
        original = next(r["value"] for r in self.fixtures["results"]
                        if r["action"] == "viewer_screenshot" and r["valid"])
        for invalid in ("AB==", "YQ===", "$not-base64$"):
            _, response = self.begin_rpc("tools/call", {
                "name": "viewer_screenshot", "arguments": self.arguments(viewer, "viewer_screenshot")})
            invocation = viewer.receive()
            payload = json.loads(json.dumps(original))
            payload["image"]["data"] = invalid
            self.finish(viewer, invocation, payload)
            result = self.rpc_result(response)["result"]
            self.assertTrue(result["isError"])
            self.assertEqual(result["structuredContent"]["error"]["code"], "internal_error")
            self.assertFalse(any(c["type"] == "image" for c in result["content"]))

    def test_screenshot_complete_output_budget(self):
        """Metadata duplication cannot bypass the outgoing budget when the input frame fits."""
        viewer = self.viewer()
        _, response = self.begin_rpc("tools/call", {
            "name": "viewer_screenshot", "arguments": self.arguments(viewer, "viewer_screenshot")})
        invocation = viewer.receive()
        original = next(r["value"] for r in self.fixtures["results"]
                        if r["action"] == "viewer_screenshot" and r["valid"])
        payload = json.loads(json.dumps(original))
        payload["image"]["data"] = base64.b64encode(b"x" * 2500).decode()
        payload["metadata"]["warnings"] = ["w" * 3000]
        message = self.finish(viewer, invocation, payload)
        self.assertLess(len(json.dumps(message).encode()), 8192)
        result = self.rpc_result(response)["result"]
        self.assertTrue(result["isError"])
        self.assertEqual(result["structuredContent"]["error"]["code"], "result_too_large")
        self.assertFalse(any(c["type"] == "image" for c in result["content"]))

    def test_real_routing_duplicate_and_reconnect(self):
        """A duplicate terminal reply cannot disable the tab or touch the tile-request protocol."""
        viewer = self.viewer()
        sessions = self.call("viewer_list_sessions", {})["structuredContent"]["sessions"]
        self.assertEqual([item["clientId"] for item in sessions], [viewer.client_id])
        arguments = self.arguments(viewer, "viewer_get_app_state")
        _, response = self.begin_rpc("tools/call", {"name": "viewer_get_app_state", "arguments": arguments})
        invocation = viewer.receive()
        self.assertEqual(invocation["type"], "mapget.actions.invoke")
        self.assertNotIn("clientId", invocation["arguments"])
        terminal = self.finish(viewer, invocation)
        self.assertFalse(self.rpc_result(response)["result"]["isError"])
        viewer.send(terminal)
        # If the adapter incorrectly converts a duplicate into register-error, it appears before
        # the next invocation and fails this assertion (the real UI would become unavailable).
        _, response = self.begin_rpc("tools/call", {"name": "viewer_get_app_state", "arguments": arguments})
        invocation = viewer.receive()
        self.assertEqual(invocation["type"], "mapget.actions.invoke")
        self.finish(viewer, invocation)
        self.assertFalse(self.rpc_result(response)["result"]["isError"])
        old_id = viewer.client_id
        viewer.close()
        replacement = self.viewer("/tiles")
        self.assertNotEqual(replacement.client_id, old_id)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            result = self.call("viewer_get_app_state", arguments)
            if result["isError"]:
                self.assertEqual(result["structuredContent"]["error"]["code"], "not_available")
                break
        else:
            self.fail("Disconnected UUID remained routable")

    def test_initialize_clients_are_stateless_and_cancellation_never_guesses_ownership(self):
        """2025 clients get their own wire shape, without inventing server-side protocol sessions."""
        viewer = self.viewer()
        _, discovery = self.begin_rpc("server/discover")
        instructions = self.rpc_result(discovery)["result"]["instructions"]
        self.assertIsInstance(instructions, str)
        self.assertTrue(instructions.strip())
        for protocol in ("2025-06-18", "2025-11-25"):
            _, response = self.begin_rpc("initialize", {
                "protocolVersion": protocol, "capabilities": {},
                "clientInfo": {"name": "initialize-client", "version": "1"}},
                headers={"MCP-Protocol-Version": ""}, protocol=protocol)
            self.assertEqual(response.status, 200)
            self.assertIsNone(response.getheader("MCP-Session-Id"))
            initialized = self.rpc_result(response)["result"]
            self.assertEqual(initialized["protocolVersion"], protocol)
            self.assertEqual(initialized["instructions"], instructions)
            _, response = self.begin_rpc("notifications/initialized", protocol=protocol, request_id=None)
            self.assertEqual(response.status, 202)
            self.assertEqual(response.read(), b"")
            _, response = self.begin_rpc("tools/list", protocol=protocol)
            tools = self.rpc_result(response)["result"]
            self.assertNotIn("resultType", tools)
            self.assertEqual(len(tools["tools"]), len(self.catalog["actions"]) + 13)
            _, response = self.begin_rpc("tools/call", {
                "name": "viewer_set_app_state", "arguments": self.arguments(viewer, "viewer_set_app_state")},
                protocol=protocol)
            invocation = viewer.receive()
            _, notification = self.begin_rpc("notifications/cancelled", {"requestId": 1},
                                              protocol=protocol, request_id=None)
            self.assertEqual(notification.status, 202)
            self.assertEqual(notification.read(), b"")
            # An ignored cancellation must neither unlock the mutation nor manufacture a
            # cancellation control for a potentially different client using the same RPC ID.
            blocked = self.call("viewer_set_app_state", self.arguments(viewer, "viewer_set_app_state"))
            self.assertEqual(blocked["structuredContent"]["error"]["code"], "busy")
            self.finish(viewer, invocation)
            result = self.rpc_result(response)["result"]
            self.assertNotIn("resultType", result)
            self.assertFalse(result["isError"])
        # A normal read proves no unexpected register-error/cancel frame was left on the viewer.
        _, response = self.begin_rpc("tools/call", {
            "name": "viewer_get_app_state", "arguments": self.arguments(viewer, "viewer_get_app_state")})
        invocation = viewer.receive()
        self.assertEqual(invocation["type"], "mapget.actions.invoke")
        self.finish(viewer, invocation)
        self.assertFalse(self.rpc_result(response)["result"]["isError"])

    def test_timeout_retains_mutation_until_browser_ack(self):
        """An HTTP deadline is not evidence that a camera mutation has stopped executing."""
        viewer = self.viewer()
        args = self.arguments(viewer, "viewer_set_app_state")
        _, response = self.begin_rpc("tools/call", {"name": "viewer_set_app_state", "arguments": args})
        invocation = viewer.receive()
        result = self.rpc_result(response)["result"]
        self.assertEqual(result["structuredContent"]["error"]["code"], "timeout")
        self.assertEqual(viewer.receive()["type"], "mapget.actions.cancel")
        blocked = self.call("viewer_set_app_state", args)
        self.assertEqual(blocked["structuredContent"]["error"]["code"], "busy")
        self.finish(viewer, invocation)
        # A registration update is an ordering barrier after the terminal acknowledgement.
        viewer.send({"type": "mapget.actions.update", "version": 1,
                     "catalogId": self.catalog["catalogId"], "label": "Still usable",
                     "actions": [action["name"] for action in self.catalog["actions"]]})
        self.assertEqual(viewer.receive()["type"], "mapget.actions.updated")
        _, response = self.begin_rpc("tools/call", {"name": "viewer_set_app_state", "arguments": args})
        invocation = viewer.receive()
        self.assertEqual(invocation["type"], "mapget.actions.invoke")
        self.finish(viewer, invocation)
        self.assertFalse(self.rpc_result(response)["result"]["isError"])

    def test_lost_http_caller_cancels_but_does_not_unlock_mutation(self):
        """Closing an SSE caller requests cancellation while the live browser owns termination."""
        viewer = self.viewer()
        args = self.arguments(viewer, "viewer_set_app_state")
        connection, response = self.begin_rpc("tools/call", {"name": "viewer_set_app_state", "arguments": args})
        invocation = viewer.receive()
        connection.sock.shutdown(socket.SHUT_RDWR)
        response.close()
        connection.close()
        cancelled = viewer.receive()
        self.assertEqual(cancelled["type"], "mapget.actions.cancel")
        blocked = self.call("viewer_set_app_state", args)
        self.assertEqual(blocked["structuredContent"]["error"]["code"], "busy")
        self.finish(viewer, invocation)


class NativeMcpConfigTest(unittest.TestCase):
    """Exercise the real CLI11/YAML path, including precedence and private startup settings."""

    def setUp(self):
        """Keep config-relative and command-line-relative artifacts in different directories."""
        directory = tempfile.TemporaryDirectory(prefix="mapget-mcp-config-")
        self.addCleanup(directory.cleanup)
        self.directory = Path(directory.name)
        self.config_path = self.directory / "config" / "service.yaml"
        self.config_path.parent.mkdir()
        self.catalog = Path(__file__).resolve().parent.parent / "unit/data/viewer-actions/web-mcp-actions.json"
        (self.config_path.parent / "catalog.json").write_bytes(self.catalog.read_bytes())
        self.log = tempfile.TemporaryFile(mode="w+", encoding="utf-8")
        self.addCleanup(self.log.close)
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            self.port = listener.getsockname()[1]
        self.settings = {"host": "127.0.0.1", "port": self.port, "no-location": True,
                         "worker-count": 2, "mcp": "local", "mcp-catalog": "catalog.json"}

    def command(self, *overrides):
        """Use actual YAML mappings with scalar and list option values, not a second config parser."""
        self.config_path.write_text("sources: []\nmapget:\n  serve:\n" + "".join(
            f"    {key}: {json.dumps(value)}\n" for key, value in self.settings.items()), encoding="utf-8")
        return [str(MAPGET_BINARY), "--config", str(self.config_path), "serve", *overrides]

    def start(self, *overrides):
        """Start a bounded isolated process and wait for its ordinary health endpoint."""
        process = subprocess.Popen(self.command(*overrides), cwd=self.directory,
                                   stdout=self.log, stderr=subprocess.STDOUT)
        self.addCleanup(self.stop, process)
        deadline = time.monotonic() + 20
        while process.poll() is None and time.monotonic() < deadline:
            try:
                if self.http("/status-data")[0] == 200:
                    return
            except OSError:
                time.sleep(0.05)
        self.log.seek(0)
        self.fail("MCP config service did not start: " + self.log.read())

    def stop(self, process):
        """Do not leave a config-watching child alive after a failed assertion."""
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()

    def http(self, path, headers=None, method="GET", body=None):
        """Return HTTP and parsed JSON where available; config writes return plain text."""
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=6)
        try:
            connection.request(method, path, body, headers or {})
            response = connection.getresponse()
            data = response.read().decode()
            return response.status, json.loads(data) if "json" in response.getheader("Content-Type", "") else data
        finally:
            connection.close()

    def test_yaml_relative_catalog_and_private_config(self):
        """Trust settings stay out of /config and survive datasource config writes unchanged."""
        self.settings["allow-post-config"] = True
        self.start()
        status, info = self.http("/mcp/info")
        self.assertEqual(status, 200)
        self.assertEqual(info["endpoint"], f"http://127.0.0.1:{self.port}/mcp")
        status, public = self.http("/config")
        self.assertEqual(status, 200)
        self.assertNotIn("mapget", public)
        self.assertNotIn("mapget", public["model"])
        model = {"sources": [{"type": "GridDataSource", "enabled": False}]}
        status, body = self.http("/config", {"Content-Type": "application/json"}, "POST", json.dumps(model))
        self.assertEqual(status, 200, body)
        status, _ = self.http("/config", {"Content-Type": "application/json"}, "POST", json.dumps({
            **model, "mapget": {"serve": {"mcp": "off"}}}))
        # A private section is either rejected or ignored; it must never be changed by this API.
        self.assertIn(status, (200, 400, 500))
        self.assertEqual(self.http("/mcp/info")[1]["enabled"], True)
        self.assertIn("mcp: local", self.config_path.read_text().replace('"local"', 'local'))
        self.assertIn("mcp-catalog: catalog.json", self.config_path.read_text().replace('"catalog.json"', 'catalog.json'))

    def test_cli_lists_replace_yaml_and_paths_use_cwd(self):
        """Explicit CLI settings replace complete YAML values; no stale trust entries are merged."""
        self.settings["mcp-endpoint"] = f"http://localhost:{self.port}/mcp"
        self.settings["mcp-allowed-hosts"] = [f"localhost:{self.port}"]
        self.settings["mcp-allowed-origins"] = [f"http://localhost:{self.port}"]
        (self.directory / "override.json").write_bytes(self.catalog.read_bytes())
        self.start("--mcp-catalog", "override.json", "--mcp-endpoint", f"http://127.0.0.1:{self.port}/mcp",
                   "--mcp-allowed-hosts", f"127.0.0.1:{self.port}",
                   "--mcp-allowed-origins", f"http://127.0.0.1:{self.port}")
        self.assertEqual(self.http("/mcp/info")[0], 200)
        self.assertEqual(self.http("/mcp/info", {"Host": f"localhost:{self.port}"})[0], 403)
        self.assertEqual(self.http("/mcp/info", {"Origin": f"http://localhost:{self.port}"})[0], 403)

    def help_query(self, query):
        """Read an MCP result through a real initialized-era SSE response."""
        status, payload = self.http("/mcp", {
            "Content-Type": "application/json", "Accept": "application/json, text/event-stream",
            "MCP-Protocol-Version": "2025-11-25"}, "POST", json.dumps({
                "jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {
                    "name": "mapget_docs", "arguments": {"query": query}}}))
        self.assertEqual(status, 200)
        message = json.loads(next(line[6:] for line in payload.splitlines() if line.startswith("data: ")))
        self.assertFalse(message["result"].get("isError", False), message)
        return message["result"]["structuredContent"]

    def test_help_yaml_paths_are_config_relative(self):
        """No caller cwd assumption or explicit catalog option leaks into help folder resolution."""
        self.settings["mcp-help-docs"] = ["help"]
        folder = self.config_path.parent / "help"
        folder.mkdir()
        (folder / "guide.md").write_text("<!-- mcp: -->\n# Quokkayaml\nYAML help.\n")
        self.start()
        self.assertEqual(len(self.help_query("quokkayaml")["items"]), 1)

    def test_help_cli_replaces_yaml_roots_and_is_cwd_relative(self):
        """Development roots are additive to bundles, but CLI replaces the YAML extra list."""
        self.settings["mcp-help-docs"] = ["help"]
        for parent, title in ((self.config_path.parent, "Quokkayaml"), (self.directory, "Quokkacli")):
            (parent / "help").mkdir()
            (parent / "help" / "guide.md").write_text(f"<!-- mcp: -->\n# {title}\nHelp.\n")
        self.start("--mcp-help-docs", "help")
        self.assertEqual(self.help_query("quokkayaml")["items"], [])
        self.assertEqual(len(self.help_query("quokkacli")["items"]), 1)

    def test_default_catalog_follows_webapp_mount(self):
        """The public manifest is found under the mounted directory, not the YAML or cwd."""
        del self.settings["mcp-catalog"]
        self.start("--webapp", "/viewer:" + str(self.catalog.parent))
        self.assertEqual(self.http("/mcp/info")[0], 200)
        self.assertEqual(self.http("/viewer/web-mcp-actions.json")[1]["catalogId"],
                         self.http("/mcp/info")[1]["catalogId"])

    def test_cli_off_ignores_retained_hosted_settings(self):
        """An administrator can disable MCP without deleting its deployment configuration."""
        self.settings.update({"mcp": "oauth", "mcp-issuer": "https://issuer.example", "mcp-catalog": "missing.json"})
        self.start("--mcp", "off")
        self.assertEqual(self.http("/mcp/info"), (200, {"enabled": False}))

    def oauth_settings(self):
        """Use a provider-neutral hosted resource, without making any network issuer requests."""
        self.settings.update({
            "mcp": "oauth", "mcp-endpoint": "https://viewer.example/mcp",
            "mcp-allowed-hosts": [f"127.0.0.1:{self.port}"], "mcp-allowed-origins": ["https://viewer.example"],
            "mcp-issuer": "https://issuer.example/realm", "mcp-jwks-url": "https://issuer.example/keys",
            "mcp-required-scopes": ["viewer"], "mcp-read-claim": "/roles", "mcp-read-value": "read",
            "mcp-trusted-proxy-addresses": ["127.0.0.1"], "mcp-browser-issuer-header": "test-issuer",
            "mcp-browser-subject-header": "test-subject", "mcp-browser-expiry-header": "test-expiry",
            "mcp-browser-permissions-header": "test-permissions"})

    def test_oauth_resource_and_scope_override(self):
        """CLI scopes replace YAML scopes in public resource discovery and bearer challenges."""
        self.oauth_settings()
        self.start("--mcp-required-scopes", "stage")
        status, metadata = self.http("/.well-known/oauth-protected-resource/mcp")
        self.assertEqual(status, 200)
        self.assertEqual(metadata["resource"], "https://viewer.example/mcp")
        self.assertEqual(metadata["scopes_supported"], ["stage"])
        status, _ = self.http("/mcp", {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"},
                              "POST", '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}')
        self.assertEqual(status, 401)

    def test_invalid_startup_configuration_fails_closed(self):
        """Misspellings, removed switches, public local listeners and invalid limits fail startup."""
        for overrides in [("--mcp", "automatic"), ("--mcp-config", "old.json"), ("--host", "0.0.0.0"),
                          ("--mcp-sessions", "0"), ("--mcp-timeout-ms", "-1"), ("--mcp-issuer", "https://issuer.example")]:
            with self.subTest(overrides=overrides):
                result = subprocess.run(self.command(*overrides), cwd=self.directory, capture_output=True, timeout=10)
                self.assertGreater(result.returncode, 0, result.stdout + result.stderr)
        self.settings["mcp-required-scopess"] = ["misspelled"]
        result = subprocess.run(self.command(), cwd=self.directory, capture_output=True, timeout=10)
        self.assertGreater(result.returncode, 0)

    def test_jwks_relative_paths(self):
        """Both artifact path options use identical YAML-vs-CLI precedence and base directories."""
        self.oauth_settings()
        del self.settings["mcp-jwks-url"]
        self.settings["mcp-jwks-file"] = "keys.json"
        # An empty key set distinguishes successful file loading from path-resolution failure.
        (self.config_path.parent / "keys.json").write_text('{"keys":[]}')
        (self.directory / "override-keys.json").write_text('{"keys":[]}')
        for overrides in [(), ("--mcp-jwks-file", "override-keys.json")]:
            result = subprocess.run(self.command(*overrides), cwd=self.directory, capture_output=True, timeout=10)
            self.assertGreater(result.returncode, 0)
            self.assertIn(b"MCP JWKS requires", result.stdout + result.stderr)


if __name__ == "__main__":
    MAPGET_BINARY = Path(sys.argv.pop(1)).resolve()
    unittest.main()
