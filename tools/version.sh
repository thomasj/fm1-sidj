#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The version of this tree, for a build without --release (the firmware, the simulator, the
# browser emulator; the intro shows it). From the last release tag (vX.Y.Z in the public repo,
# dev/vX.Y.Z on master in the private one), so it never needs editing:
#   1.0.0                 the release itself, unchanged
#   1.0.0+3.abc1234       3 commits after it, at commit abc1234
#   1.0.0+dirty           with changes to tracked files not committed (also: 1.0.0+3.abc1234.dirty)
#   1.0.0+abc1234         before the first release (no tag yet)
#   1.0.0+nogit           not a git checkout (a source archive): nothing tells whether it was changed
set -e
cd "$(dirname "$0")/.."
BASE=1.0.0                  # (before any release tag)
if ! git rev-parse --verify -q HEAD >/dev/null 2>&1; then
    echo "$BASE+nogit"
    exit 0
fi
git update-index -q --refresh >/dev/null 2>&1 || true
dirty=
git diff-index --quiet HEAD -- || dirty=dirty
hash=$(git rev-parse --short=7 HEAD)
if tag=$(git describe --tags --abbrev=0 --match 'v[0-9]*' --match 'dev/v[0-9]*' 2>/dev/null); then
    base=${tag#dev/}
    base=${base#v}
    n=$(git rev-list --count "$tag..HEAD")
    meta=
    [ "$n" -gt 0 ] && meta="$n.$hash"
else
    base=$BASE
    meta=$hash
fi
[ -n "$dirty" ] && meta="${meta:+$meta.}$dirty"
echo "$base${meta:++$meta}"
