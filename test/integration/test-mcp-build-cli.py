"""Check the MCP build boundary through the real CLI and its YAML loader."""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


BINARY = sys.argv.pop(1)
ENABLED = sys.argv.pop(1) == "1"


class McpBuildCliTest(unittest.TestCase):
    def run_cli(self, *arguments):
        return subprocess.run(
            [BINARY, *arguments], capture_output=True, text=True, timeout=5,
        )

    def test_help_matches_build(self):
        result = self.run_cli("serve", "--help")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual("--mcp " in result.stdout, ENABLED)
        self.assertEqual("--mcp-endpoint" in result.stdout, ENABLED)

    @unittest.skipIf(ENABLED, "Native MCP integration tests exercise the enabled build")
    def test_disabled_build_rejects_cli_options(self):
        for arguments in (("--mcp", "local"), ("--mcp", "oauth"), ("--mcp", "off"),
                          ("--mcp-endpoint", "http://localhost:8099/mcp")):
            with self.subTest(arguments=arguments):
                result = self.run_cli("serve", *arguments)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("mcp", result.stdout + result.stderr)

    @unittest.skipIf(ENABLED, "Native MCP integration tests exercise the enabled build")
    def test_disabled_build_rejects_yaml_options(self):
        with tempfile.TemporaryDirectory(prefix="mapget-no-mcp-") as directory:
            config = Path(directory) / "config.yaml"
            for setting in ("mcp: local", "mcp: oauth", "mcp: off",
                            "mcp-endpoint: http://localhost:8099/mcp"):
                with self.subTest(setting=setting):
                    config.write_text("mapget:\n  serve:\n    " + setting + "\nsources: []\n")
                    result = self.run_cli("--config", str(config), "serve")
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("mcp", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
