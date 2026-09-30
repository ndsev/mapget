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
        self.log = tempfile.TemporaryFile(mode="w+")
        self.addCleanup(self.log.close)
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            self.port = listener.getsockname()[1]
        catalog_path = Path(__file__).parent.parent / "unit/data/viewer-actions/viewer-actions.json"
        self.catalog = json.loads(catalog_path.read_text())
        self.fixtures = json.loads((catalog_path.parent / "fixtures.json").read_text())
        config = Path(self.directory.name) / "mcp.json"
        config.write_text(json.dumps({
            "authentication": "local", "endpoint": f"http://127.0.0.1:{self.port}/mcp",
            "catalogPath": str(catalog_path.resolve()),
            "allowedHosts": [f"127.0.0.1:{self.port}"],
            "allowedOrigins": [f"http://127.0.0.1:{self.port}"],
            "limits": {"timeoutMs": 2000}}))
        self.process = subprocess.Popen([
            str(MAPGET_BINARY), "serve", "--host", "127.0.0.1", "-p", str(self.port),
            "--no-location", "--worker-count", "2", "--mcp-config", str(config)],
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

    def finish(self, viewer, invocation):
        """Return a valid application result for exactly the dispatched call identity."""
        fixture = next(item for item in self.fixtures["results"]
                       if item["action"] == invocation["action"] and item["valid"])
        message = {"type": "mapget.actions.result", "version": 1,
                   "callId": invocation["callId"], "result": fixture["value"]}
        viewer.send(message)
        return message

    def test_protocol_and_exposure(self):
        """Check discovery, all four tools, malformed metadata and local Host/Origin boundaries."""
        for method in ("server/discover", "tools/list"):
            _, response = self.begin_rpc(method)
            self.assertEqual(response.status, 200)
            result = self.rpc_result(response)["result"]
            self.assertEqual(result["resultType"], "complete")
            if method == "tools/list":
                self.assertEqual(len(result["tools"]), 4)
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
        self.assertEqual(self.http("GET", "/.well-known/oauth-protected-resource/mcp")[0], 404)

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
        for protocol in ("2025-06-18", "2025-11-25"):
            _, response = self.begin_rpc("initialize", {
                "protocolVersion": protocol, "capabilities": {},
                "clientInfo": {"name": "initialize-client", "version": "1"}},
                headers={"MCP-Protocol-Version": ""}, protocol=protocol)
            self.assertEqual(response.status, 200)
            self.assertIsNone(response.getheader("MCP-Session-Id"))
            self.assertEqual(self.rpc_result(response)["result"]["protocolVersion"], protocol)
            _, response = self.begin_rpc("notifications/initialized", protocol=protocol, request_id=None)
            self.assertEqual(response.status, 202)
            self.assertEqual(response.read(), b"")
            _, response = self.begin_rpc("tools/list", protocol=protocol)
            tools = self.rpc_result(response)["result"]
            self.assertNotIn("resultType", tools)
            self.assertEqual(len(tools["tools"]), 4)
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


if __name__ == "__main__":
    MAPGET_BINARY = Path(sys.argv.pop(1)).resolve()
    unittest.main()
