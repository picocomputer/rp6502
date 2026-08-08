#!/usr/bin/env python3
#
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Fetch the SingleStepTests wdc65c02 vectors into vendor/65x02.
#
# The upstream repository carries five CPU families and checks out at 4.8 GB, so
# this is a blobless sparse clone of the one directory we run. Still about a
# gigabyte on disk, which is why it is a task the developer runs once rather
# than a submodule, the same arrangement as the Emscripten SDK.

import os
import subprocess
import sys

REPO = 'https://github.com/SingleStepTests/65x02'
# Pinned: the July 2024 BBR/BBS correction is the whole reason this repo is used
# instead of the archived SingleStepTests/ProcessorTests.
COMMIT = '2f6980a2d95757486c7bee24355c360e40e2a224'
SPARSE = 'wdc65c02/v1'

HERE = os.path.dirname(os.path.abspath(__file__))
DEST = os.path.abspath(os.path.join(HERE, '..', '..', 'vendor', '65x02'))


def git(*args, **kwargs):
    subprocess.run(['git'] + list(args), check=True, **kwargs)


def main():
    if os.path.isdir(os.path.join(DEST, SPARSE)):
        print('Already present: %s' % os.path.join(DEST, SPARSE))
        return 0

    print('Fetching %s %s into %s' % (REPO, SPARSE, DEST))
    if not os.path.isdir(os.path.join(DEST, '.git')):
        os.makedirs(DEST, exist_ok=True)
        git('clone', '--filter=blob:none', '--no-checkout', REPO, DEST)
    git('-C', DEST, 'sparse-checkout', 'set', '--no-cone', SPARSE)
    git('-C', DEST, 'checkout', COMMIT)
    print('Done.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
