#!/usr/bin/env bash
# Apply / revert / inspect / rebase the local patch series that live on top of
# the pinned upstream submodules in third_party/.
#
# Usage:
#   scripts/patches.sh apply [project...]
#   scripts/patches.sh revert [project...]
#   scripts/patches.sh status [project...]
#   scripts/patches.sh refresh <project>        # capture worktree diff as one patch
#   scripts/patches.sh rebase <project> <ref>   # move submodule to <ref>, re-apply series
#
# A "project" is a directory name under patches/; its tree defaults to
# third_party/<project>. When the patched code is a nested submodule (the browser
# engine sits inside third_party/TactileBrowser), patches/<project>/dir names that
# path instead. With no project argument every project is processed.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PATCH_ROOT="$ROOT/patches"
TP_ROOT="$ROOT/third_party"

die() { echo "error: $*" >&2; exit 1; }
info() { echo "==> $*"; }

projects() {
    if [ "$#" -gt 0 ]; then
        printf '%s\n' "$@"
    else
        # Portable across GNU and BSD find (no -printf).
        find "$PATCH_ROOT" -mindepth 1 -maxdepth 1 -type d | sed 's|.*/||' | sort
    fi
}

series_file() { echo "$PATCH_ROOT/$1/series"; }
subdir_file() { echo "$PATCH_ROOT/$1/dir"; }

submodule_dir() {
    local rel="$1"
    if [ -f "$(subdir_file "$1")" ]; then
        rel="$(cat "$(subdir_file "$1")")"
    fi
    echo "$TP_ROOT/$rel"
}

check_project() {
    local p="$1" dir
    dir="$(submodule_dir "$p")"
    [ -d "$dir" ] || die "no submodule at ${dir#$ROOT/}"
    [ -f "$(series_file "$p")" ] || die "no series file at patches/$p/series"
}

# Apply one patch, tolerating an already-applied patch.
apply_patch() {
    local dir="$1" patch="$2"
    if git -C "$dir" apply --check --reverse "$patch" >/dev/null 2>&1; then
        echo "    already applied: $(basename "$patch")"
        return 0
    fi
    # Plain apply keeps the submodule index untouched; fall back to a three-way
    # merge only when the patch no longer applies to the moved upstream.
    if ! git -C "$dir" apply "$patch"; then
        if ! git -C "$dir" apply --3way "$patch"; then
            die "failed to apply $(basename "$patch") in $dir (resolve manually, then 'patches.sh refresh')"
        fi
    fi
    echo "    applied: $(basename "$patch")"
}

revert_patch() {
    local dir="$1" patch="$2"
    if git -C "$dir" apply --check --reverse "$patch" >/dev/null 2>&1; then
        git -C "$dir" apply --reverse "$patch"
        echo "    reverted: $(basename "$patch")"
    else
        echo "    not applied: $(basename "$patch")"
    fi
}

do_apply() {
    local p="$1" dir patch
    check_project "$p"
    dir="$(submodule_dir "$p")"
    info "applying patches/$p"
    while IFS= read -r patch; do
        [ -n "$patch" ] || continue
        case "$patch" in \#*) continue ;; esac
        [ -f "$PATCH_ROOT/$p/$patch" ] || die "patches/$p/$patch listed in series but missing"
        apply_patch "$dir" "$PATCH_ROOT/$p/$patch"
    done < "$(series_file "$p")"
}

do_revert() {
    local p="$1" dir patch patches=()
    check_project "$p"
    dir="$(submodule_dir "$p")"
    info "reverting patches/$p"
    while IFS= read -r patch; do
        [ -n "$patch" ] || continue
        case "$patch" in \#*) continue ;; esac
        patches+=("$patch")
    done < "$(series_file "$p")"
    for (( i=${#patches[@]}-1; i>=0; i-- )); do
        revert_patch "$dir" "$PATCH_ROOT/$p/${patches[$i]}"
    done
    # A patch applied with --3way stages its changes; clearing the index keeps
    # `git status` honest about whether the series is really gone.
    git -C "$dir" reset --quiet 2>/dev/null || true
}

do_status() {
    local p="$1" dir patch
    check_project "$p"
    dir="$(submodule_dir "$p")"
    echo "-- $p ($(git -C "$dir" rev-parse --short HEAD) $(git -C "$dir" describe --tags --always 2>/dev/null || true))"
    while IFS= read -r patch; do
        [ -n "$patch" ] || continue
        case "$patch" in \#*) continue ;; esac
        if git -C "$dir" apply --check --reverse "$PATCH_ROOT/$p/$patch" >/dev/null 2>&1; then
            echo "   applied     $patch"
        elif git -C "$dir" apply --check "$PATCH_ROOT/$p/$patch" >/dev/null 2>&1; then
            echo "   not applied $patch"
        else
            echo "   conflicts   $patch"
        fi
    done < "$(series_file "$p")"
    if [ -n "$(git -C "$dir" status --porcelain)" ]; then
        echo "   note: submodule worktree is dirty (patched series or local edits)"
    fi
}

do_refresh() {
    local p="$1" dir out
    [ "$#" -eq 1 ] || die "usage: patches.sh refresh <project>"
    dir="$(submodule_dir "$p")"
    out="$PATCH_ROOT/$p/9999-worktree.patch"
    if [ -z "$(git -C "$dir" status --porcelain)" ]; then
        die "third_party/$p has no changes to capture"
    fi
    git -C "$dir" diff > "$out"
    info "captured worktree diff to ${out#$ROOT/}"
    echo "    review it, then replace the relevant numbered patch (or add it to the series)."
}

do_rebase() {
    local p="$1" ref="$2" dir
    [ -n "$ref" ] || die "usage: patches.sh rebase <project> <ref>"
    dir="$(submodule_dir "$p")"
    info "reverting current series before moving to $ref"
    do_revert "$p" || true
    # A dirty tree after revert means local edits: refuse rather than lose them.
    if [ -n "$(git -C "$dir" status --porcelain)" ]; then
        die "third_party/$p still dirty after revert; commit or discard before rebasing"
    fi
    git -C "$dir" fetch --tags origin
    git -C "$dir" checkout "$ref"
    info "submodule now at $(git -C "$dir" rev-parse --short HEAD); re-applying series"
    do_apply "$p"
    echo
    echo "Rebase done. If a hunk conflicted, resolve it in third_party/$p, then:"
    echo "    scripts/patches.sh refresh $p   # capture the resolved worktree"
    echo "    # move the captured change into the numbered patch and re-apply cleanly"
    echo "Finally record the new submodule commit: git add third_party/$p"
}

cmd="${1:-}"; shift || true
case "$cmd" in
    apply)   for p in $(projects "$@"); do do_apply "$p"; done ;;
    revert)  for p in $(projects "$@"); do do_revert "$p"; done ;;
    status)  for p in $(projects "$@"); do do_status "$p"; done ;;
    refresh) do_refresh "$@" ;;
    rebase)  do_rebase "$@" ;;
    *) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
