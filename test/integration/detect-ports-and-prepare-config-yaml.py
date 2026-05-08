#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import re
import socket
from pathlib import Path


_PORT_RANGE_START = 20000
_PORT_RANGE_END = 30000


def _pick_free_tcp_ports(count: int, seed: str) -> list[int]:
    if count <= 0:
        raise ValueError("count must be > 0")
    if count > (_PORT_RANGE_END - _PORT_RANGE_START):
        raise ValueError("count exceeds available port range")

    sockets: list[socket.socket] = []
    ports: list[int] = []
    port_span = _PORT_RANGE_END - _PORT_RANGE_START
    start_offset = (sum(seed.encode("utf-8")) + os.getpid()) % port_span
    try:
        for candidate_offset in range(port_span):
            if len(ports) == count:
                break

            port = _PORT_RANGE_START + ((start_offset + candidate_offset) % port_span)
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                s.bind(("127.0.0.1", port))
            except OSError:
                s.close()
                continue

            sockets.append(s)
            ports.append(port)
    finally:
        for s in sockets:
            try:
                s.close()
            except Exception:
                pass

    if len(ports) != count:
        raise RuntimeError(
            f"Could not reserve {count} TCP ports in range {_PORT_RANGE_START}-{_PORT_RANGE_END - 1}"
        )
    if len(set(ports)) != len(ports):
        raise RuntimeError(f"Port picker returned duplicates: {ports}")

    return ports


def _patch_sample_service_yaml(text: str, mapget_port: int, datasource_cpp_port: int, datasource_py_port: int) -> str:
    text = re.sub(
        r"(?m)^(\s*port:\s*)\d+(\s*)$",
        rf"\g<1>{mapget_port}\g<2>",
        text,
        count=1,
    )
    text = text.replace("127.0.0.1:61853", f"127.0.0.1:{datasource_cpp_port}")
    text = text.replace("127.0.0.1:61854", f"127.0.0.1:{datasource_py_port}")
    return text


def _patch_cache_dir(text: str, cache_path: str) -> str:
    # Prefer updating an existing cache-dir value, otherwise insert after cache-type.
    escaped_path = cache_path.replace("'", "''")
    cache_value = f"'{escaped_path}'"
    if re.search(r"(?m)^\s*cache-dir:\s*.*$", text):
        return re.sub(
            r"(?m)^(\s*cache-dir:\s*).*$",
            lambda match: f"{match.group(1)}{cache_value}",
            text,
            count=1,
        )
    match = re.search(r"(?m)^(\s*)cache-type:\s*.*$", text)
    if not match:
        return text
    indent = match.group(1)
    insert_line = f"{indent}cache-dir: {cache_value}"
    return re.sub(
        r"(?m)^(\s*cache-type:\s*.*)$",
        lambda match: f"{match.group(1)}\n{insert_line}",
        text,
        count=1,
    )


def _patch_sample_fetch_yaml(text: str, mapget_port: int) -> str:
    return re.sub(
        r"(?m)^(\s*server:\s*127\.0\.0\.1:)\d+(\s*)$",
        rf"\g<1>{mapget_port}\g<2>",
        text,
        count=1,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-dir", required=True, help="Output directory for generated files (created if needed).")
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    # Pick ports at test runtime (reduces collision risk vs. configure-time selection).
    mapget_port, datasource_cpp_port, datasource_py_port = _pick_free_tcp_ports(3, str(out_dir))

    ports_env = out_dir / "ports.env"
    ports_env.write_text(
        "\n".join(
            [
                f"export MAPGET_SERVER_PORT={mapget_port}",
                f"export DATASOURCE_CPP_PORT={datasource_cpp_port}",
                f"export DATASOURCE_PY_PORT={datasource_py_port}",
                "",
            ]
        ),
        encoding="utf-8",
        newline="\n",
    )

    repo_root = Path(__file__).resolve().parents[2]
    examples_config = repo_root / "examples" / "config"

    sample_service = (examples_config / "sample-service.yaml").read_text(encoding="utf-8")
    cache_file = out_dir / "mapget-cache.db"
    for stale_cache_file in (cache_file, out_dir / "mapget-cache.db-shm", out_dir / "mapget-cache.db-wal"):
        stale_cache_file.unlink(missing_ok=True)
    cache_path = str(cache_file.resolve())
    (out_dir / "sample-service.yaml").write_text(
        _patch_cache_dir(
            _patch_sample_service_yaml(sample_service, mapget_port, datasource_cpp_port, datasource_py_port),
            cache_path,
        ),
        encoding="utf-8",
        newline="\n",
    )

    sample_first = (examples_config / "sample-first-datasource.yaml").read_text(encoding="utf-8")
    (out_dir / "sample-first-datasource.yaml").write_text(
        _patch_sample_fetch_yaml(sample_first, mapget_port),
        encoding="utf-8",
        newline="\n",
    )

    sample_second = (examples_config / "sample-second-datasource.yaml").read_text(encoding="utf-8")
    (out_dir / "sample-second-datasource.yaml").write_text(
        _patch_sample_fetch_yaml(sample_second, mapget_port),
        encoding="utf-8",
        newline="\n",
    )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
