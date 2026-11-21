#!/usr/bin/env bash
# rebuild-integration.sh - rebase the fork's integration branch onto upstream
# main after upstream merges something (FORK.md, "Working rules").
#
#   tools/fork/rebuild-integration.sh [--drop <commit>...] [--dry-run]
#
# 1. Fetches origin (upstream) and fork, fast-forwards local main to
#    origin/main (refuses if main has commits of its own).
# 2. Tags the integration branch's tip integration-<version>, the upstream
#    release it was built on (v0.17.0 gives integration-0.17.0; -2, -3... if
#    the name is taken; the date when no release tag is found), so commits
#    the games pin stay reachable.
# 3. Rebases the branch onto main. Commits upstream took unchanged drop out
#    by themselves; --drop removes ones upstream took in another form (a PR
#    with a test, a reworded fix). A merge commit is flattened.
# 4. Prints the range-diff to review and the push commands. Never pushes.
#
# --dry-run does steps 1-3 on a throwaway branch and deletes it and the tag
# afterwards, to see what a rebuild would do.
#
# Environment: BRANCH (default integration), UPSTREAM (origin), FORK (fork).
set -euo pipefail

BRANCH=${BRANCH:-integration}
UPSTREAM=${UPSTREAM:-origin}
FORK=${FORK:-fork}
drops=()
dry=0
while [ $# -gt 0 ]; do
    case $1 in
        --drop) shift; while [ $# -gt 0 ] && [ "${1#--}" = "$1" ]; do drops+=("$1"); shift; done ;;
        --dry-run) dry=1; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

die() { echo "rebuild-integration: $*" >&2; exit 1; }

[ -z "$(git status --porcelain --untracked-files=no)" ] || die "the tree has changes; commit them first"
[ "$(git symbolic-ref --short -q HEAD)" = "$BRANCH" ] || die "check out $BRANCH first"

git fetch -q "$UPSTREAM"
git fetch -q "$FORK"

# 1. main follows upstream only.
if git rev-parse -q --verify refs/heads/main >/dev/null; then
    git merge-base --is-ancestor main "$UPSTREAM/main" \
        || die "main has commits upstream lacks; main must only fast-forward"
fi
git branch -f main "$UPSTREAM/main"
old=$(git rev-parse --short HEAD)
if git merge-base --is-ancestor main HEAD && [ ${#drops[@]} -eq 0 ]; then
    echo "$BRANCH ($old) already sits on main ($(git rev-parse --short main)): nothing to rebuild"
    exit 0
fi

# 2. Tag the old tip, named for the upstream release it was built on.
base=$(git describe --tags --abbrev=0 --match 'v[0-9]*' HEAD 2>/dev/null) \
    || base=$(date +%Y-%m-%d)
tag=integration-${base#v}
n=2
while git rev-parse -q --verify "refs/tags/$tag" >/dev/null; do
    tag=integration-${base#v}-$n
    n=$((n + 1))
done
git tag -a "$tag" -m "$BRANCH before its rebuild onto $(git rev-parse --short main)" HEAD

work=$BRANCH
if [ $dry = 1 ]; then
    work=rebuild-dry-run
    git switch -q -c "$work"
fi

# 3. Rebase. The todo list is edited by a script, so nothing is interactive.
script=
for c in "${drops[@]}"; do
    h=$(git rev-parse "$c^{commit}") || die "no such commit: $c"
    h=${h:0:7}
    script="$script -e 's/^pick \\($h[0-9a-f]*\\) /drop \\1 /'"
done
if [ -n "$script" ]; then
    GIT_SEQUENCE_EDITOR="sed -i $script" git rebase -q -i main \
        || die "rebase stopped: resolve, git rebase --continue, then review as below (tag $tag holds the old tip)"
else
    git rebase -q main \
        || die "rebase stopped: resolve, git rebase --continue, then review as below (tag $tag holds the old tip)"
fi

new=$(git rev-parse --short HEAD)
echo "rebuilt: $old -> $new on main $(git rev-parse --short main); old tip tagged $tag"
echo "commits left on top of main: $(git rev-list --count main..HEAD) (were $(git rev-list --count --no-merges main.."$tag"))"
echo
# One line per commit: = unchanged, ! changed, < gone (upstream took it or
# --drop), > new.
git --no-pager range-diff -s main "$tag" HEAD
echo
echo "review in full:   git range-diff main $tag $work"

if [ $dry = 1 ]; then
    git switch -q "$BRANCH"
    git branch -q -D "$work"
    git tag -d "$tag" >/dev/null
    echo "(dry run: $work and $tag deleted; $BRANCH unchanged)"
    exit 0
fi

pushed=$(git rev-parse -q --verify "$FORK/$BRANCH" || true)
echo "then:     git push $FORK main $tag"
echo "          git push --force-with-lease=$BRANCH:${pushed:-} $FORK $BRANCH"
echo "and update FORK.md's table, and each game's TOOLKIT_COMMIT when it next moves."
