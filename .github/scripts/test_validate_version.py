#!/usr/bin/env python3
"""Focused tests for mapget's release-version preflight."""

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from zipfile import ZipFile

from validate_version import (
    ReleaseContext,
    check_upload,
    determine_release_context,
    get_base_version,
    get_cmake_version,
    next_candidate,
    validate_versions,
    verify_tag_checkout,
)


class ValidateVersionTest(unittest.TestCase):
    """Covers each CI event that introduces a release identity."""

    def test_candidate_only_on_release_branch_dispatch(self):
        event = {"GITHUB_EVENT_NAME": "workflow_dispatch", "GITHUB_REF": "refs/heads/release/2026.6.0"}
        context = determine_release_context(event)
        self.assertEqual(context, ReleaseContext("2026.6.0", "Release candidate"))
        self.assertTrue(validate_versions("2026.5.0", "2026.5.1.dev1", context))
        for ref in ["refs/heads/main", "refs/heads/feature/x", "refs/heads/release/bad",
                    "refs/tags/v2026.6.0", "refs/tags/v2026.6.0rc1"]:
            with self.subTest(ref=ref), self.assertRaises(ValueError):
                determine_release_context(dict(event, GITHUB_REF=ref))

    def test_next_candidate_uses_only_matching_releases(self):
        self.assertEqual(next_candidate("1.0.0", {}), "1.0.0rc1")
        releases = dict.fromkeys(["1.0.0rc2", "1.0.0rc10", "1.0.0.dev500", "1.0.1rc90"])
        self.assertEqual(next_candidate("1.0.0", releases), "1.0.0rc11")
        with self.assertRaises(ValueError):
            next_candidate("1.0.0", {"1.0.0": []})

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

    def test_published_stable_requires_exact_tag_cmake_and_scm_versions(self):
        release = {"tag_name": "v2026.3.5", "draft": False, "prerelease": False}
        event = {"GITHUB_EVENT_NAME": "release", "GITHUB_REF": "refs/tags/v2026.3.5"}
        payload = {"action": "published", "release": release}
        context = determine_release_context(event, payload)
        self.assertEqual(context, ReleaseContext("2026.3.5", "Stable release"))
        self.assertEqual(validate_versions("2026.3.5", "2026.3.5", context), [])
        self.assertTrue(validate_versions("2026.3.4", "2026.3.5", context))
        for scm in ["2026.3.4", "2026.3.5.dev1", "2026.3.5rc1", "2026.3.5.post1", "2026.3.5+local"]:
            with self.subTest(scm=scm):
                self.assertTrue(validate_versions("2026.3.5", scm, context))
        for patch_release in [{"draft": True}, {"prerelease": True}, {"tag_name": "v2026.3.6"}]:
            with self.subTest(patch_release=patch_release), self.assertRaises(ValueError):
                determine_release_context(event, dict(payload, release=release | patch_release))
        for action in ["created", "edited", "released", "prereleased", "deleted"]:
            with self.subTest(action=action), self.assertRaises(ValueError):
                determine_release_context(event, dict(payload, action=action))
        for ref in ["refs/heads/main", "refs/tags/2026.3.5", "refs/tags/v2026.03.5",
                    "refs/tags/v2026.3.5rc1", "refs/tags/v2026.3.5.1", "refs/tags/v2026.3.5\u0661"]:
            with self.subTest(ref=ref), self.assertRaises(ValueError):
                determine_release_context(dict(event, GITHUB_REF=ref), payload)

    def test_tag_and_checked_out_commit_must_match_the_event_sha(self):
        ref = "refs/tags/v2026.3.5"
        sha = "a" * 40
        with patch("validate_version.subprocess.check_output", side_effect=[sha, sha]) as git:
            verify_tag_checkout(ref, sha)
            self.assertEqual(git.call_args_list[0].args[0][-1], f"{ref}^{{commit}}")
        for tag_commit, head in [(sha, "b" * 40), ("b" * 40, sha), ("b" * 40, "b" * 40)]:
            with patch("validate_version.subprocess.check_output", side_effect=[tag_commit, head]):
                with self.subTest(tag=tag_commit, head=head), self.assertRaisesRegex(ValueError, "SHA mismatch"):
                    verify_tag_checkout(ref, sha)

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

    def test_build_only_events_cannot_authorize_wheels(self):
        pr = {"GITHUB_EVENT_NAME": "pull_request", "GITHUB_BASE_REF": "main", "GITHUB_HEAD_REF": "release/1.2.3"}
        with self.assertRaisesRegex(ValueError, "Invalid release ref"):
            determine_release_context(dict(pr, GITHUB_HEAD_REF="release/bad"))
        for env in [pr, dict(pr, GITHUB_BASE_REF="other"), dict(pr, GITHUB_HEAD_REF="feature/x"),
                    {"GITHUB_EVENT_NAME": "push", "GITHUB_REF": "refs/tags/v1.2.3"},
                    {"GITHUB_EVENT_NAME": "push", "GITHUB_REF": "refs/heads/main"}]:
            with self.subTest(env=env), self.assertRaisesRegex(ValueError, "does not authorize"):
                check_upload(determine_release_context(env), "1.2.3", Path("dist"))

    def test_upload_checks_every_wheel_against_the_selected_version(self):
        for kind, version in [("Stable release", "1.2.3"), ("Release candidate", "1.2.3rc7")]:
            context = ReleaseContext("1.2.3", kind)
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as tmp:
                directory = Path(tmp)
                with self.assertRaisesRegex(ValueError, "No wheels"):
                    check_upload(context, version, directory)
                other_channel = "1.2.3" if kind == "Release candidate" else "1.2.3rc1"
                for wrong in ["1.2.4", "1.2.3.dev1", "1.2.3rc0", other_channel]:
                    with self.subTest(wrong=wrong), self.assertRaisesRegex(ValueError, "does not authorize"):
                        check_upload(context, wrong, directory)

                def wheel(platform, name="mapget", metadata_version=version):
                    path = directory / f"mapget-{version}-cp310-abi3-{platform}.whl"
                    with ZipFile(path, "w") as archive:
                        archive.writestr(f"mapget-{version}.dist-info/METADATA",
                                         f"Name: {name}\nVersion: {metadata_version}\n")
                    return path

                wheel("win_amd64")
                check_upload(context, version, directory)
                for name, embedded in [("another-project", version), ("mapget", "9.9.9")]:
                    wheel("manylinux", name, embedded)
                    with self.assertRaisesRegex(ValueError, "not a wheel for"):
                        check_upload(context, version, directory)
                wheel("manylinux").rename(directory / "mapget-9.9.9-cp310-abi3-manylinux.whl")
                with self.assertRaisesRegex(ValueError, "not a wheel for"):
                    check_upload(context, version, directory)


if __name__ == "__main__":
    unittest.main()
