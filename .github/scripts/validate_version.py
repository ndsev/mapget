#!/usr/bin/env python3
"""Validate release identities before mapget starts an expensive wheel build."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping


CMAKE_VERSION_PATTERN = re.compile(r"set\(MAPGET_VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)\)")
RELEASE_VERSION_PATTERN = re.compile(r"^v?([0-9]+\.[0-9]+\.[0-9]+)$")
PLAIN_RELEASE_VERSION_PATTERN = re.compile(r"^([0-9]+\.[0-9]+\.[0-9]+)$")
SCM_BASE_VERSION_PATTERN = re.compile(r"^([0-9]+\.[0-9]+\.[0-9]+)")


@dataclass(frozen=True)
class ReleaseContext:
    """Describes which release identity, if any, the current CI event requires."""

    expected_version: str | None
    build_type: str
    require_scm_match: bool = False


def normalize_release_version(
    value: str,
    source: str,
    *,
    allow_leading_v: bool = True,
) -> str:
    """Return a strict three-component release version with controlled `v` handling."""
    pattern = RELEASE_VERSION_PATTERN if allow_leading_v else PLAIN_RELEASE_VERSION_PATTERN
    match = pattern.fullmatch(value)
    if not match:
        expected_format = "X.Y.Z or vX.Y.Z" if allow_leading_v else "X.Y.Z"
        raise ValueError(
            f"Invalid {source} version {value!r}; expected {expected_format}."
        )
    return match.group(1)


def get_cmake_version(path: Path = Path("CMakeLists.txt")) -> str:
    """Extract the default `MAPGET_VERSION` from the root CMake project."""
    match = CMAKE_VERSION_PATTERN.search(path.read_text(encoding="utf-8"))
    if not match:
        raise ValueError(f"Could not find MAPGET_VERSION in {path}.")
    return match.group(1)


def get_scm_version() -> str:
    """Resolve the PEP 440 package version from the current Git checkout."""
    try:
        import setuptools_scm

        return setuptools_scm.get_version(local_scheme="no-local-version")
    except ImportError:
        result = subprocess.run(
            [sys.executable, "-m", "setuptools_scm"],
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            raise ValueError(f"setuptools_scm failed: {result.stderr.strip()}")
        return result.stdout.strip().split("+", 1)[0]


def get_base_version(version: str) -> str:
    """Extract the X.Y.Z release base from a PEP 440 SCM version."""
    match = SCM_BASE_VERSION_PATTERN.match(version)
    if not match:
        raise ValueError(f"Could not derive a release base from SCM version {version!r}.")
    return match.group(1)


def determine_release_context(
    environment: Mapping[str, str],
    explicit_version: str | None = None,
) -> ReleaseContext:
    """Derive the expected version from a tag, release PR, or release-workflow input."""
    expected = (
        normalize_release_version(
            explicit_version,
            "requested release",
            allow_leading_v=False,
        )
        if explicit_version
        else None
    )
    build_type = "Requested release" if expected else "Development build"
    require_scm_match = False

    github_ref = environment.get("GITHUB_REF", "")
    if github_ref.startswith("refs/tags/"):
        tag_version = normalize_release_version(
            github_ref.removeprefix("refs/tags/"),
            "tag",
        )
        if expected and expected != tag_version:
            raise ValueError(
                f"Requested release {expected} does not match tag version {tag_version}."
            )
        expected = tag_version
        build_type = "Tagged release"
        require_scm_match = True
    elif (
        environment.get("GITHUB_EVENT_NAME") == "pull_request"
        and environment.get("GITHUB_BASE_REF") == "main"
        and environment.get("GITHUB_HEAD_REF", "").startswith("release/")
    ):
        branch_version = normalize_release_version(
            environment["GITHUB_HEAD_REF"].removeprefix("release/"),
            "release branch",
        )
        if expected and expected != branch_version:
            raise ValueError(
                f"Requested release {expected} does not match release branch {branch_version}."
            )
        expected = branch_version
        build_type = "Release pull request"

    return ReleaseContext(expected, build_type, require_scm_match)


def validate_versions(
    cmake_version: str,
    scm_version: str,
    context: ReleaseContext,
) -> list[str]:
    """Return all release-version contract violations for the supplied context."""
    scm_base_version = get_base_version(scm_version)
    errors: list[str] = []

    if context.expected_version:
        if cmake_version != context.expected_version:
            errors.append(
                f"CMake version {cmake_version} does not match "
                f"{context.build_type.lower()} version {context.expected_version}."
            )
        if context.require_scm_match and scm_base_version != context.expected_version:
            errors.append(
                f"SCM base version {scm_base_version} does not match "
                f"tag version {context.expected_version}."
            )
    elif scm_base_version.startswith("0."):
        errors.append(
            f"Invalid development SCM version {scm_version}; Git history may be unavailable."
        )

    return errors


def main() -> int:
    """Validate the current checkout and publish resolved values to GitHub Actions."""
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--expected-version",
        help="Release version requested by the guarded release workflow.",
    )
    args = parser.parse_args()

    try:
        cmake_version = get_cmake_version()
        scm_version = get_scm_version()
        scm_base_version = get_base_version(scm_version)
        context = determine_release_context(os.environ, args.expected_version)
        errors = validate_versions(cmake_version, scm_version, context)
    except ValueError as error:
        print(f"ERROR: {error}")
        return 1

    print(f"CMake version: {cmake_version}")
    print(f"Setuptools SCM version: {scm_version}")
    print(f"Setuptools SCM base version: {scm_base_version}")
    print(f"Build type: {context.build_type}")
    print(f"GitHub ref: {os.environ.get('GITHUB_REF', '')}")
    if context.expected_version:
        print(f"Expected release version: {context.expected_version}")

    if errors:
        print("\nERROR: Release version validation failed:")
        for error in errors:
            print(f"  - {error}")
        return 1

    if not context.expected_version:
        print("Development build: strict release identity checks do not apply.")
    print("Version validation passed.")

    if output_path := os.environ.get("GITHUB_OUTPUT"):
        with open(output_path, "a", encoding="utf-8") as output:
            output.write(f"version={scm_version}\n")
            output.write(f"base_version={scm_base_version}\n")
            output.write(f"cmake_version={cmake_version}\n")

    return 0


if __name__ == "__main__":
    sys.exit(main())
