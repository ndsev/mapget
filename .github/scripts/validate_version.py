#!/usr/bin/env python3
"""Validate release identities before mapget starts an expensive wheel build."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from email.parser import BytesHeaderParser
from pathlib import Path
from typing import Mapping
from urllib.request import urlopen
from zipfile import BadZipFile, ZipFile


CMAKE_VERSION_PATTERN = re.compile(r"set\(MAPGET_VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)\)")
RELEASE_VERSION_PATTERN = re.compile(r"(?:0|[1-9][0-9]*)(?:\.(?:0|[1-9][0-9]*)){2}")
SCM_BASE_VERSION_PATTERN = re.compile(r"^([0-9]+\.[0-9]+\.[0-9]+)")


@dataclass(frozen=True)
class ReleaseContext:
    """Describes which release identity, if any, the current CI event requires."""

    expected_version: str | None
    build_type: str

    @property
    def is_stable(self) -> bool:
        return self.build_type == "Stable release"


def release_version(ref: str, prefix: str) -> str:
    version = ref.removeprefix(prefix)
    if not ref.startswith(prefix) or not RELEASE_VERSION_PATTERN.fullmatch(version):
        raise ValueError(f"Invalid release ref {ref!r}; expected {prefix}X.Y.Z.")
    return version


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
    payload: Mapping | None = None,
) -> ReleaseContext:
    """Only a published stable release or a manual release-branch candidate can upload."""
    event = environment.get("GITHUB_EVENT_NAME")
    ref = environment.get("GITHUB_REF", "")
    if event == "release":
        version = release_version(ref, "refs/tags/v")
        payload = payload or {}
        release = payload.get("release") or {}
        if (
            payload.get("action") != "published"
            or release.get("draft") is not False
            or release.get("prerelease") is not False
            or release.get("tag_name") != f"v{version}"
        ):
            raise ValueError(f"Expected a published, non-prerelease GitHub Release for {ref}.")
        return ReleaseContext(version, "Stable release")
    if event == "workflow_dispatch":
        return ReleaseContext(release_version(ref, "refs/heads/release/"), "Release candidate")
    if (
        event == "pull_request"
        and environment.get("GITHUB_BASE_REF") == "main"
        and environment.get("GITHUB_HEAD_REF", "").startswith("release/")
    ):
        return ReleaseContext(release_version(environment["GITHUB_HEAD_REF"], "release/"), "Release pull request")
    return ReleaseContext(None, "Development build")


def verify_tag_checkout(tag_ref: str, event_sha: str) -> None:
    """Compare commits, not tag objects (an annotated tag has its own SHA)."""
    tag_commit = subprocess.check_output(
        ["git", "rev-parse", "--verify", f"{tag_ref}^{{commit}}"], text=True
    ).strip()
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    if not event_sha or tag_commit != event_sha or head != event_sha:
        raise ValueError(f"Release SHA mismatch: {tag_ref}={tag_commit}, HEAD={head}, event={event_sha}.")


def next_candidate(base: str, releases: Mapping) -> str:
    """Count only published candidates for this exact final version."""
    if base in releases:
        raise ValueError(f"Final release {base} is already on PyPI.")
    pattern = re.compile(re.escape(base) + r"rc([0-9]+)")
    numbers = (int(match[1]) for name in releases if (match := pattern.fullmatch(name)))
    return f"{base}rc{max(numbers, default=0) + 1}"


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
        if context.is_stable and scm_version != context.expected_version:
            errors.append(
                f"SCM version {scm_version} does not exactly match "
                f"tag version {context.expected_version}."
            )
    elif scm_base_version.startswith("0."):
        errors.append(
            f"Invalid development SCM version {scm_version}; Git history may be unavailable."
        )

    return errors


def check_upload(context: ReleaseContext, version: str, directory: Path) -> None:
    """Check what Twine will actually upload, not just the version passed to CMake."""
    if context.build_type == "Release candidate" and context.expected_version:
        allowed = re.fullmatch(re.escape(context.expected_version) + r"rc[1-9][0-9]*", version) is not None
    else:
        allowed = context.is_stable and version == context.expected_version
    if not allowed:
        raise ValueError(f"{context.build_type} does not authorize uploading {version!r}.")
    wheels = list(directory.glob("*.whl"))
    if not wheels:
        raise ValueError(f"No wheels to upload in {directory}.")
    for wheel in wheels:
        with ZipFile(wheel) as archive:
            metadata = BytesHeaderParser().parsebytes(archive.read(f"mapget-{version}.dist-info/METADATA"))
        if (
            not wheel.name.startswith(f"mapget-{version}-")
            or metadata["Name"] != "mapget"
            or metadata["Version"] != version
        ):
            raise ValueError(f"{wheel.name} is not a wheel for mapget=={version}.")


def main() -> int:
    """Validate the current checkout and publish resolved values to GitHub Actions."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--resolve-candidate", action="store_true",
                        help="Choose the package version for a candidate build from PyPI.")
    parser.add_argument("--check-upload", metavar="VERSION",
                        help="Recheck publication eligibility and every wheel in dist/ before upload.")
    args = parser.parse_args()

    try:
        cmake_version = get_cmake_version()
        scm_version = get_scm_version()
        scm_base_version = get_base_version(scm_version)
        payload = None
        if os.getenv("GITHUB_EVENT_NAME") == "release":
            payload = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
        context = determine_release_context(os.environ, payload)
        if context.is_stable:
            verify_tag_checkout(os.environ["GITHUB_REF"], os.environ["GITHUB_SHA"])
        errors = validate_versions(cmake_version, scm_version, context)
        if not errors and args.check_upload is not None:
            check_upload(context, args.check_upload, Path("dist"))
    except (ValueError, KeyError, OSError, BadZipFile, subprocess.CalledProcessError) as error:
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
        version = scm_version
        if args.resolve_candidate and context.build_type == "Release candidate":
            with urlopen("https://pypi.org/pypi/mapget/json", timeout=30) as response:
                version = next_candidate(cmake_version, json.load(response)["releases"])
        with open(output_path, "a", encoding="utf-8") as output:
            output.write(f"version={version}\n")
            output.write(f"base_version={scm_base_version}\n")
            output.write(f"cmake_version={cmake_version}\n")

    return 0


if __name__ == "__main__":
    sys.exit(main())
