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
#
# Before anything is changed, the commits of chip-player (local and origin/chip-player, after
# fetching origin) are checked: a commit that is in none of the feature branches (compared by
# patch, so a cherry-picked copy counts) would be lost by the rebuild, and the script stops and
# lists it. Move it to its feature branch (git cherry-pick), or run with FORK_SYNC_DROP=1 to
# discard it on purpose.
set -eu
# the branch switches below replace this file, so run from a copy
if [ -z "${FORK_SYNC_COPY:-}" ]; then
	t=$(mktemp); cp "$0" "$t"; FORK_SYNC_COPY=$t exec sh "$t" "$@"
fi
known=$(mktemp)
trap 'rm -f "$FORK_SYNC_COPY" "$known"' EXIT
cd "$(git rev-parse --show-toplevel)"
[ -z "$(git status --porcelain --untracked-files=no)" ] || { echo "working tree not clean" >&2; exit 1; }
if [ "${1:-}" != --no-fetch ]; then
	git fetch upstream
	git remote get-url origin >/dev/null 2>&1 && git fetch origin
fi
BRANCHES=$(git show fork-tools:fork/branches | sed -e 's/#.*//' -e '/^ *$/d')

# commits of chip-player that the rebuild would drop: in no feature branch, by patch id
for b in $BRANCHES; do
	git log -p --no-merges "upstream/master..$b"
done | git patch-id --stable | cut -d' ' -f1 | sort -u > "$known"
lost=""
for ref in chip-player origin/chip-player; do
	git rev-parse -q --verify "$ref" >/dev/null || continue
	for c in $(git rev-list --no-merges "upstream/master..$ref"); do
		p=$(git show "$c" | git patch-id --stable | cut -d' ' -f1)
		[ -z "$p" ] && continue                      # empty commit: nothing to lose
		grep -qx "$p" "$known" && continue
		case " $lost " in *" $c "*) ;; *) lost="$lost $c" ;; esac
	done
done
if [ -n "$lost" ]; then
	echo "chip-player has commits that are in no feature branch; rebuilding would drop them:" >&2
	for c in $lost; do
		echo "  $(git log -1 --format='%h %s' "$c")  (in:$(git branch -a --contains "$c" --format=' %(refname:short)' | tr -d '\n'))" >&2
	done
	if [ "${FORK_SYNC_DROP:-}" = 1 ]; then
		echo "FORK_SYNC_DROP=1: dropping them (the backup tag below keeps them)" >&2
	else
		echo "move each one to its feature branch (git switch <branch>; git cherry-pick <commit>)," >&2
		echo "or rerun with FORK_SYNC_DROP=1 to drop them" >&2
		exit 1
	fi
fi

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
