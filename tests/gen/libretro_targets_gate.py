#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause

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
