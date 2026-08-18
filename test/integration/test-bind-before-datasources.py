#!/usr/bin/env python3
"""Verify listener binding precedes config-backed datasource construction."""

import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 2:
        raise RuntimeError("Expected the mapget executable path.")

    mapget = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="mapget-bind-order-") as temp_dir_name:
        temp_dir = Path(temp_dir_name)
        marker = temp_dir / "datasource-started"
        helper = temp_dir / "mark-started.py"
        helper.write_text(
            "from pathlib import Path\n"
            "import sys\n"
            "Path(sys.argv[1]).write_text('started', encoding='utf-8')\n",
            encoding="utf-8",
        )

        command = f'"{sys.executable}" "{helper}" "{marker}"'
        config = temp_dir / "mapget.yaml"
        config.write_text(
            "sources:\n"
            "  - type: DataSourceProcess\n"
            f"    cmd: {json.dumps(command)}\n",
            encoding="utf-8",
        )

        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            port = listener.getsockname()[1]
            result = subprocess.run(
                [
                    str(mapget),
                    "--config",
                    str(config),
                    "serve",
                    "--host",
                    "127.0.0.1",
                    "--port",
                    str(port),
                    "--wait-ms",
                    "2000",
                ],
                capture_output=True,
                text=True,
                timeout=10,
                check=False,
            )

        output = result.stdout + result.stderr
        if result.returncode != 1:
            raise AssertionError(
                f"Expected occupied-port exit code 1, got {result.returncode}.\n{output}"
            )
        if "Bind address failed" not in output:
            raise AssertionError(f"Expected listener bind failure output.\n{output}")
        if marker.exists():
            raise AssertionError(
                "Config-backed datasource process started before listener binding completed."
            )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
