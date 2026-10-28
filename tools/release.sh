#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# A public release of SiDJ: this checkout's history stays in the private repo (remote origin);
# the public repo (remote public) gets one commit per release, holding the tree of master as it
# is, with the firmware attached to a GitHub release and the site on its gh-pages branch.
#
#   tools/release.sh X.Y.Z            build, test, commit on branch public, ask, push
#   tools/release.sh X.Y.Z --dry-run  the same up to the commit (on branch public-dryrun,
#                                     deleted after), nothing pushed
#
# Branch public is the public repo's main: its commits are snapshots (git read-tree), never
# merged into master or from it. master gets a tag dev/vX.Y.Z at each release, where the next
# release's notes start. Needs gh (brew install gh; gh auth login) for the release itself.
set -euo pipefail
cd "$(dirname "$0")/.."

die() { echo "release: $*" >&2; exit 1; }
V=${1:-}; DRY=0
[ "${2:-}" = --dry-run ] && DRY=1
[[ $V =~ ^[0-9]{1,2}\.[0-9]{1,2}\.[0-9]{1,2}$ ]] || die "usage: tools/release.sh X.Y.Z [--dry-run]"
PKG=build/sidj-$V.fwsc
WT=build/release-tree
BR=public; [ $DRY = 1 ] && BR=public-dryrun
PY=python3; [ -x .venv/bin/python3 ] && PY=.venv/bin/python3
export PATH="$HOME/.docker/bin:$PATH"     # (Docker Desktop's binaries, for build.sh)

# ---- what must hold first
[ "$(git symbolic-ref --short HEAD)" = master ] || die "not on master"
git diff --quiet && git diff --cached --quiet || die "master has changes that are not committed"
git rev-parse -q --verify "refs/tags/v$V" >/dev/null && die "tag v$V exists already"
if [ $DRY = 0 ]; then
    URL=$(git remote get-url public 2>/dev/null) || die "no remote 'public' (git remote add public URL)"
    command -v gh >/dev/null || die "no gh (brew install gh; gh auth login)"
    SLUG=$(echo "$URL" | sed -E 's#^.*github\.com[:/]##; s#\.git$##')
    git ls-remote --exit-code --tags public "v$V" >/dev/null 2>&1 && die "the public repo has v$V already"
fi

# ---- build and test
echo "release: building SiDJ $V"
./build.sh --release "$V" > build/release-build.log 2>&1 || { tail -20 build/release-build.log; die "the build failed (build/release-build.log)"; }
grep -qi "failed" build/release-build.log && die "the build log says failed (build/release-build.log)"
[ -f "$PKG" ] || die "no $PKG after the build"
echo "release: testing"
tests/run_tests.sh > build/release-tests.log 2>&1 || { tail -20 build/release-tests.log; die "the tests failed (build/release-tests.log)"; }

# ---- the release commit, in a worktree of its own
git worktree remove --force "$WT" 2>/dev/null || true
rm -rf "$WT"
if [ $DRY = 0 ] && ! git rev-parse -q --verify refs/heads/public >/dev/null &&
   git ls-remote --exit-code --heads public main >/dev/null 2>&1; then
    git fetch -q public main && git branch public FETCH_HEAD   # (another checkout made the last release)
fi
OLD=""
if git rev-parse -q --verify refs/heads/public >/dev/null; then
    OLD=$(git rev-parse public)
    git worktree add -q -B "$BR" "$WT" public
else                                        # the first release: no history at all
    git worktree add -q --detach "$WT"
    git -C "$WT" switch -q --orphan "$BR"
fi
git -C "$WT" read-tree -u --reset master    # the tree of master exactly, deletions included

PREV=$(git describe --tags --abbrev=0 --match 'dev/v*' master 2>/dev/null || true)
NOTES=build/release-notes.txt
{
    echo "SiDJ $V"
    echo
    if [ -n "$PREV" ]; then
        echo "(What is new, in a few lines. The list below is the dev history since $PREV: keep, edit or cut.)"
        echo
        git log --reverse --format='- %s' "$PREV..master"
    else
        echo "(The first release: what SiDJ is and does, in a few lines. The dev history stays private.)"
    fi
} > "$NOTES"
if [ -t 0 ]; then
    git -C "$WT" commit -q -e -F "$PWD/$NOTES" || die "no release commit (nothing changed, or the message was left empty)"
else
    git -C "$WT" commit -q -F "$PWD/$NOTES" || die "no release commit (nothing changed since the last release?)"
fi
echo
git -C "$WT" show --stat --format='%B' HEAD | head -50

undo() {                                    # the release commit goes again
    git worktree remove --force "$WT" 2>/dev/null || true
    if [ -n "$OLD" ]; then git branch -f public "$OLD"; elif [ $DRY = 0 ]; then git branch -D public >/dev/null; fi
    [ $DRY = 1 ] && git branch -D public-dryrun >/dev/null 2>&1 || true
}

if [ $DRY = 1 ]; then
    "$PY" tools/make_site.py "$PKG" --out build/site | sed 's/^/release: /'
    undo
    echo "release: dry run done, nothing pushed (package $PKG, site build/site)"
    exit 0
fi

# ---- out, after a yes
read -r -p "release: push SiDJ $V to $URL? [y/N] " yes
[ "$yes" = y ] || { undo; die "not pushed; the release commit is gone again"; }
git -C "$WT" tag -a "v$V" -m "SiDJ $V"
git push -q public "public:main"
git push -q public "v$V"
git tag -a "dev/v$V" master -m "SiDJ $V released"
git remote get-url origin >/dev/null 2>&1 && git push -q origin "dev/v$V" master
git -C "$WT" log -1 --format='%b' > build/release-body.txt
gh release create "v$V" "$PKG" --repo "$SLUG" --title "SiDJ $V" --notes-file build/release-body.txt

"$PY" tools/make_site.py "$PKG" --out build/site | sed 's/^/release: /'
rm -rf build/site/.git
git -C build/site init -q
git -C build/site checkout -q -b gh-pages
git -C build/site add -A
git -C build/site commit -q -m "SiDJ $V"
git -C build/site push -q -f "$URL" gh-pages
rm -rf build/site/.git
git worktree remove --force "$WT"

echo "release: SiDJ $V is out: https://github.com/$SLUG/releases/tag/v$V"
if [ "$(gh repo view "$SLUG" --json visibility -q .visibility 2>/dev/null)" = PRIVATE ]; then
    echo "release: $SLUG is private: the site goes live when it is public and Pages serves gh-pages"
    echo "release:   gh repo edit $SLUG --visibility public --accept-visibility-change-consequences"
    echo "release: until then: python3 -m http.server -d build/site 8790"
fi
