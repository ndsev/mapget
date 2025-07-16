#!/usr/bin/env python3
"""
Validate that the setuptools_scm version matches the CMake version for release tags.

This script is used in the CI/CD pipeline to ensure version consistency
between the CMake configuration and Git tags before deployment to PyPI.

The script compares:
- Base version from CMakeLists.txt (e.g., "2025.3.0")
- Base version from setuptools_scm (e.g., "2025.3.0" from tag or "2025.3.0.dev3" from commit)

For release tags: versions must match exactly
For dev builds: no validation, just ensure we got a reasonable version
"""
import re
import sys
import os
import subprocess


def get_cmake_version():
    """Extract MAPGET_VERSION from CMakeLists.txt"""
    with open('CMakeLists.txt', 'r') as f:
        content = f.read()
    
    match = re.search(r'set\(MAPGET_VERSION\s+([0-9.]+)\)', content)
    if not match:
        raise ValueError("Could not find MAPGET_VERSION in CMakeLists.txt")
    
    return match.group(1)


def get_scm_version():
    """Get version from setuptools_scm"""
    try:
        import setuptools_scm
        # Use no-local-version to ensure PyPI compatibility
        version = setuptools_scm.get_version(
            local_scheme="no-local-version"
        )
        return version
    except ImportError:
        # Fallback to command line
        result = subprocess.run(
            [sys.executable, '-m', 'setuptools_scm'],
            capture_output=True,
            text=True
        )
        
        if result.returncode != 0:
            raise ValueError(f"setuptools_scm failed: {result.stderr}")
        
        # Strip local version identifier if present
        version = result.stdout.strip()
        if '+' in version:
            version = version.split('+')[0]
        return version


def get_base_version(version):
    """Extract base version (without dev/post parts)"""
    # Remove everything after + (local version)
    version = version.split('+')[0]
    # Remove dev/post/rc parts
    parts = version.split('.')
    if len(parts) > 3:
        # Check if last part is dev/post/rc
        if parts[3].startswith('dev') or parts[3].startswith('post'):
            parts = parts[:3]
    return '.'.join(parts[:3])


def main():
    cmake_version = get_cmake_version()
    scm_version = get_scm_version()
    scm_base_version = get_base_version(scm_version)
    
    print(f"CMake version: {cmake_version}")
    print(f"SetupTools SCM version: {scm_version}")
    print(f"SetupTools SCM base version: {scm_base_version}")
    
    # Check if this is a tagged release
    github_ref = os.environ.get('GITHUB_REF', '')
    is_tag = github_ref.startswith('refs/tags/')
    is_dev = '.dev' in scm_version
    
    print(f"\nBuild type: {'Tagged release' if is_tag else 'Development build'}")
    print(f"GitHub ref: {github_ref}")
    
    # Only validate version match for tagged releases
    if is_tag:
        # For tagged releases, version must match exactly
        if cmake_version != scm_base_version:
            print(f"\nERROR: Version mismatch for tagged release!")
            print(f"  CMake:    {cmake_version}")
            print(f"  SCM base: {scm_base_version}")
            print(f"  This is a tagged release - versions must match exactly")
            sys.exit(1)
    else:
        # For dev builds, just ensure we got a reasonable version
        if scm_base_version.startswith('0.'):
            print(f"\nERROR: Invalid dev version detected!")
            print(f"  SCM version: {scm_version}")
            print(f"  This suggests git history is not available")
            sys.exit(1)
        print(f"\nDev build - skipping strict version validation")
    
    print("\nVersion validation passed!")
    
    # Output for GitHub Actions
    if 'GITHUB_OUTPUT' in os.environ:
        with open(os.environ['GITHUB_OUTPUT'], 'a') as f:
            f.write(f"version={scm_version}\n")
            f.write(f"base_version={scm_base_version}\n")
            f.write(f"cmake_version={cmake_version}\n")


if __name__ == '__main__':
    main()