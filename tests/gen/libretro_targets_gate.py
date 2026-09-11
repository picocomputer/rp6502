#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The two CI files against each other.
#
# .gitlab-ci.yml is what the libretro buildbot runs, and what it builds is what
# the Core Downloader serves. .github/workflows/ci.yml is where we find out a
# platform stopped compiling. A target in one file and not the other is either
# a platform nobody here builds or a platform we build for nobody, and neither
# file says so on its own.
#
# GitLab names a target by the ci-template each job extends and GitHub names it
# in the matrix, so the two sides are compared through the table below. A job
# extending a libretro template that is not in the table stops this test, which
# is what happens the first time a platform is added.

import argparse
import sys

import yaml

TEMPLATES = {
    ".libretro-linux-cmake-x86_64": "linux-x86_64",
    ".libretro-linux-cmake-aarch64": "linux-aarch64",
    ".libretro-windows-msvc19-cmake-x86_64": "windows-x86_64",
    ".libretro-osx-cmake-x86_64": "macos-x86_64",
    ".libretro-osx-cmake-arm64": "macos-arm64",
    ".libretro-ios-cmake-arm64": "ios-arm64",
    ".libretro-tvos-cmake-arm64": "tvos-arm64",
    ".libretro-android-cmake-arm64-v8a": "android-arm64",
    ".libretro-android-cmake-armeabi-v7a": "android-armeabi-v7a",
    ".libretro-android-cmake-x86_64": "android-x86_64",
}


def load(path):
    with open(path, encoding="utf-8") as f:
        return yaml.safe_load(f)


def gitlab_targets(path):
    """Every job that extends a libretro build template, by target."""
    found = {}
    for job, body in load(path).items():
        if job.startswith(".") or not isinstance(body, dict):
            continue
        for base in body.get("extends", []):
            if base in TEMPLATES:
                found[TEMPLATES[base]] = job
            elif base.startswith(".libretro-"):
                print(f"{path}: {job} extends {base}, which this test has no "
                      f"name for. Add it to TEMPLATES.", file=sys.stderr)
                raise SystemExit(1)
    return found


def github_targets(path):
    """Every target named by a libretro job's matrix."""
    found = {}
    for job, body in load(path)["jobs"].items():
        if not job.startswith("libretro"):
            continue
        matrix = body.get("strategy", {}).get("matrix", {})
        for entry in matrix.get("include", []):
            if "target" in entry:
                found[entry["target"]] = job
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--gitlab", required=True)
    ap.add_argument("--github", required=True)
    a = ap.parse_args()

    buildbot = gitlab_targets(a.gitlab)
    actions = github_targets(a.github)

    # A file that parsed but yielded nothing is a rename or a rewrite, not an
    # agreement, and two empty sets are equal.
    for path, found in ((a.gitlab, buildbot), (a.github, actions)):
        if not found:
            print(f"{path}: no libretro targets found", file=sys.stderr)
            return 1

    theirs = sorted(set(buildbot) - set(actions))
    ours = sorted(set(actions) - set(buildbot))
    for target in theirs:
        print(f"{buildbot[target]} builds {target}, and no job here does",
              file=sys.stderr)
    for target in ours:
        print(f"{actions[target]} builds {target}, and the buildbot does not",
              file=sys.stderr)
    if theirs or ours:
        return 1

    print(f"libretro targets: {len(buildbot)}, the same in both files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
