#!/usr/bin/env python3
"""Focused tests for mapget's release-version preflight."""

import tempfile
import unittest
from pathlib import Path

from validate_version import (
    ReleaseContext,
    determine_release_context,
    get_base_version,
    get_cmake_version,
    validate_versions,
)


class ValidateVersionTest(unittest.TestCase):
    """Covers each CI event that introduces a release identity."""

    def test_extracts_cmake_and_scm_base_versions(self):
        with tempfile.TemporaryDirectory() as directory:
            cmake_file = Path(directory) / "CMakeLists.txt"
            cmake_file.write_text(
                "if(NOT DEFINED MAPGET_VERSION)\n"
                "  set(MAPGET_VERSION 2026.3.5)\n"
                "endif()\n",
                encoding="utf-8",
            )
            self.assertEqual(get_cmake_version(cmake_file), "2026.3.5")

        self.assertEqual(get_base_version("2026.3.5.dev123"), "2026.3.5")

    def test_tag_requires_cmake_and_scm_to_match(self):
        context = determine_release_context(
            {
                "GITHUB_EVENT_NAME": "push",
                "GITHUB_REF": "refs/tags/v2026.3.5",
            }
        )

        self.assertEqual(
            context,
            ReleaseContext("2026.3.5", "Tagged release", require_scm_match=True),
        )
        self.assertEqual(validate_versions("2026.3.5", "2026.3.5", context), [])
        self.assertEqual(len(validate_versions("2026.3.4", "2026.3.5", context)), 1)
        self.assertEqual(len(validate_versions("2026.3.5", "2026.3.4", context)), 1)

    def test_release_pr_requires_branch_and_cmake_to_match(self):
        context = determine_release_context(
            {
                "GITHUB_EVENT_NAME": "pull_request",
                "GITHUB_REF": "refs/pull/123/merge",
                "GITHUB_BASE_REF": "main",
                "GITHUB_HEAD_REF": "release/2026.3.5",
            }
        )

        self.assertEqual(
            context,
            ReleaseContext("2026.3.5", "Release pull request"),
        )
        # SCM can still infer from the preceding tag before the release tag exists.
        self.assertEqual(validate_versions("2026.3.5", "2026.3.4.dev7", context), [])
        self.assertEqual(len(validate_versions("2026.3.4", "2026.3.4.dev7", context)), 1)

    def test_guarded_release_accepts_explicit_version(self):
        context = determine_release_context(
            {
                "GITHUB_EVENT_NAME": "workflow_dispatch",
                "GITHUB_REF": "refs/heads/main",
            },
            "2026.3.5",
        )

        self.assertEqual(context, ReleaseContext("2026.3.5", "Requested release"))
        self.assertEqual(validate_versions("2026.3.5", "2026.3.6.dev1", context), [])

    def test_rejects_malformed_or_conflicting_release_identity(self):
        with self.assertRaisesRegex(ValueError, "Invalid release branch"):
            determine_release_context(
                {
                    "GITHUB_EVENT_NAME": "pull_request",
                    "GITHUB_BASE_REF": "main",
                    "GITHUB_HEAD_REF": "release/not-a-version",
                }
            )

        with self.assertRaisesRegex(ValueError, "does not match tag"):
            determine_release_context(
                {"GITHUB_REF": "refs/tags/v2026.3.5"},
                "2026.3.6",
            )

        with self.assertRaisesRegex(ValueError, "expected X.Y.Z"):
            determine_release_context({}, "v2026.3.5")


if __name__ == "__main__":
    unittest.main()
