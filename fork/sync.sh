#!/bin/sh
# Bring the fork up to date with upstream (ValleyBell/libvgm) and rebuild chip-player.
#
#   fork/sync.sh            fetch, rebase every feature branch, rebuild chip-player, build, regress
#   fork/sync.sh --no-fetch same without fetching
#
# chip-player is a throw-away integration branch: upstream/master plus a --no-ff merge of each
# branch in fork/branches. It is recreated from scratch every time (so pushing it needs --force);
# the work itself lives in the feature branches. A backup tag of the previous chip-player is kept.
# Stops at the first rebase or merge conflict, leaving the repository in that state.
set -eu
# the branch switches below replace this file, so run from a copy
if [ -z "${FORK_SYNC_COPY:-}" ]; then
	t=$(mktemp); cp "$0" "$t"; FORK_SYNC_COPY=$t exec sh "$t" "$@"
fi
trap 'rm -f "$FORK_SYNC_COPY"' EXIT
cd "$(git rev-parse --show-toplevel)"
[ -z "$(git status --porcelain --untracked-files=no)" ] || { echo "working tree not clean" >&2; exit 1; }
[ "${1:-}" = --no-fetch ] || git fetch upstream
BRANCHES=$(git show fork-tools:fork/branches | sed -e 's/#.*//' -e '/^ *$/d')

for b in $BRANCHES; do
	echo "== rebase $b"
	git rebase upstream/master "$b"
done

if git rev-parse -q --verify chip-player >/dev/null; then
	git tag -f "backup/chip-player-$(date +%Y%m%d-%H%M%S)" chip-player >/dev/null
fi
git switch -C chip-player upstream/master
for b in $BRANCHES; do
	echo "== merge $b"
	git merge --no-ff --no-edit "$b"
done

cmake --build build -j8
python3.14 fork/regress.py
