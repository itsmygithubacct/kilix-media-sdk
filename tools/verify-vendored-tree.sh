#!/bin/sh
# Verify the vendored third-party tree against manifest.toml.
#
# Two independent ways to fail, because a digest alone does not notice a file
# that is no longer there:
#
#   1. closure  -- the set of files on disk must equal vendored_tree_files
#                  exactly. Catches an added file and a removed one.
#   2. digest   -- sha256sum over those files, in the manifest's order, hashed
#                  again, must equal vendored_tree_sha256. Catches a changed byte.
#
# The order comes from the manifest rather than from `sort`, because `sort`
# honours the operator's locale and the two orders give different digests for
# the same unchanged tree. See the comment above vendored_tree_files.
set -eu

# Fix the collation for every comparison in this script. `sort` and `comm` must
# agree, and both honour the operator's locale by default -- with LANG unset to
# something else, `comm` rejects a C-sorted list as unsorted. The digest order
# comes from the manifest, so nothing here depends on which collation we pick;
# it only has to be the same one throughout.
LC_ALL=C
export LC_ALL

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
manifest="$root_dir/manifest.toml"
test -f "$manifest" || { echo "verify-vendor: manifest.toml is absent" >&2; exit 2; }

field() {
    sed -n 's/^'"$1"' = "\([^"]*\)"$/\1/p' "$manifest"
}

expected_digest=$(field vendored_tree_sha256)
tree_root_rel=$(field vendored_tree_root)
test -n "$expected_digest" || { echo "verify-vendor: vendored_tree_sha256 is absent" >&2; exit 2; }
test -n "$tree_root_rel"   || { echo "verify-vendor: vendored_tree_root is absent" >&2; exit 2; }

tree_root="$root_dir/$tree_root_rel"
test -d "$tree_root" || { echo "verify-vendor: vendored tree is absent: $tree_root_rel" >&2; exit 2; }

# The declared, ordered file list.
declared=$(sed -n '/^vendored_tree_files = \[$/,/^\]$/p' "$manifest" \
           | sed -n 's/^    "\(.*\)",$/\1/p')
test -n "$declared" || { echo "verify-vendor: vendored_tree_files is absent or empty" >&2; exit 2; }
declared_count=$(printf '%s\n' "$declared" | wc -l | tr -d ' ')

# 1. Closure, both directions.
observed=$(cd "$tree_root" && find . -type f | sed 's|^\./||' | sort)
observed_count=$(printf '%s\n' "$observed" | wc -l | tr -d ' ')
declared_sorted=$(printf '%s\n' "$declared" | sort)

work=$(mktemp -d "${TMPDIR:-/tmp}/verify-vendor.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM
printf '%s\n' "$declared_sorted" > "$work/declared"
printf '%s\n' "$observed" > "$work/observed"
missing=$(comm -23 "$work/declared" "$work/observed")
extra=$(comm -13 "$work/declared" "$work/observed")

status=0
if [ -n "$missing" ]; then
    echo "verify-vendor: FAIL closure -- declared but absent from the tree:" >&2
    printf '    %s\n' $missing >&2
    status=1
fi
if [ -n "$extra" ]; then
    echo "verify-vendor: FAIL closure -- present in the tree but not declared:" >&2
    printf '    %s\n' $extra >&2
    status=1
fi
if [ "$status" -ne 0 ]; then
    echo "verify-vendor: closure $observed_count/$declared_count files agree" >&2
    exit 1
fi

# 2. Digest, in the manifest's declared order.
set --
for path in $declared; do set -- "$@" "./$path"; done
observed_digest=$(cd "$tree_root" && sha256sum "$@" | sha256sum | cut -d ' ' -f 1)

if [ "$observed_digest" != "$expected_digest" ]; then
    echo "verify-vendor: FAIL digest" >&2
    echo "    expected $expected_digest" >&2
    echo "    observed $observed_digest" >&2
    exit 1
fi

echo "verify-vendor: PASS closure $observed_count/$declared_count files, digest 1/1 ($tree_root_rel)"
