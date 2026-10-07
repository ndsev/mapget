#!/usr/bin/env python3
"""Exercise documentation registration, fresh staging and relocatable installation without deps."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    """Added/deleted Markdown must appear in packaging without re-running configuration."""
    helper = (Path(sys.argv[1]) / "cmake/McpHelp.cmake").resolve()
    with tempfile.TemporaryDirectory(prefix="mapget help packaging ") as temp:
        root = Path(temp)
        source, build, installed = root / "source", root / "build", root / "installed"
        docs = source / "docs"
        docs.mkdir(parents=True)
        (docs / "old.md").write_text("<!-- mcp: -->\n# Old\nold\n")
        (docs / "private.txt").write_text("not documentation")
        (source / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.20)
project(help-packaging NONE)
set(CMAKE_INSTALL_BINDIR bin)
set(MAPGET_DEPLOY_DIR "${{CMAKE_BINARY_DIR}}/bin")
include("{helper.as_posix()}")
add_mcp_doc_folder(COMPONENT test DIRECTORY "${{CMAKE_CURRENT_SOURCE_DIR}}/docs")
''')
        subprocess.run(["cmake", "-S", str(source), "-B", str(build)], check=True)
        registry = build / "bin/.mcp-help-sources.json"
        assert json.loads(registry.read_text()) == {"test": docs.as_posix()}
        subprocess.run(["cmake", "--build", str(build)], check=True)
        bundle = build / "bin/mcp-help/test"
        assert (bundle / "old.md").is_file()
        assert not (bundle / "private.txt").exists()
        (docs / "old.md").unlink()
        (docs / "nested").mkdir()
        (docs / "nested/new.md").write_text("<!-- mcp: -->\n# New\nnew\n")
        subprocess.run(["cmake", "--build", str(build)], check=True)
        assert not (bundle / "old.md").exists()
        assert (bundle / "nested/new.md").is_file()
        subprocess.run(["cmake", "--install", str(build), "--prefix", str(installed)], check=True)
        assert (installed / "bin/mcp-help/test/nested/new.md").is_file()
        assert not list(installed.rglob(".mcp-help-sources.json"))
        assert not list(installed.rglob("private.txt"))
        assert not any(path.is_symlink() for path in installed.rglob("*"))


if __name__ == "__main__":
    main()
