#!/usr/bin/env bash
# Keep the two packaging manifests agreeing on the installed layout.
#
#   packaging/check-manifest-parity.sh
#
# tedge-dot is packaged twice — .goreleaser.yaml builds tedge-dot-rs, and
# impl/c/packaging/nfpm.yaml builds tedge-dot-c — and the two are meant to be
# interchangeable: "a config, a flow or a cloud integration built against one
# works unchanged against the other" (README). That only holds if both install
# the same files to the same places.
#
# Nothing enforced it, and the same mistake happened twice while adding point
# libraries: the /etc/tedge/plugins/ot/points.d directory and then the packaged
# libraries themselves went into .goreleaser.yaml alone, leaving the C package
# shipping demo configs that referenced lists it did not install — a demo that
# could not start, on one package only. This script is the guard.
#
# Scope: it compares installed DESTINATIONS. It does not compare the source file
# behind each destination, nor file_info/type — parsing that out of two
# differently shaped YAML files in shell would be the fragile kind of check this
# script is meant to replace. Destinations are what broke, twice.
#
# Run by CI and by `just check-manifest-parity`.
set -euo pipefail

# A floor on what a healthy manifest yields. Without it this script's whole
# failure mode is silence: if a reformat or restructure ever stops the `sed`
# below from matching, every set comes out empty, every comparison passes, and
# the guard goes green while checking nothing. There are 28 shared paths today.
MIN_PATHS=10

repo=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo"

# Paths that differ on purpose. Anything else appearing on one side only is a
# packaging bug, so this list is deliberately short and each entry says why.
declare -a EXPECTED_C_ONLY=(
    # PROFIBUS-DP ships only in the C package: the Rust module's serial
    # dependency has a native libudev build script that does not cross-compile
    # (see impl/c/README.md's parity table).
    "/etc/tedge/plugins/ot/profibus.toml"
    "/usr/share/tedge-dot/demo/profibus.toml"
    # goreleaser installs the Rust binary through its own `builds`/nfpm bridge
    # rather than a `contents` entry, so it has no dst line to compare.
    "/usr/bin/tedge-dot"
)

installed_paths() {
    # Absolute dst paths only: goreleaser's archive section uses relative ones.
    sed -n 's/^[[:space:]]*[-]\{0,1\}[[:space:]]*dst:[[:space:]]*\(\/[^[:space:]]*\).*/\1/p' "$1" \
        | sort -u
}

rust=$(installed_paths .goreleaser.yaml)
c=$(installed_paths impl/c/packaging/nfpm.yaml)

for pair in "rust:.goreleaser.yaml" "c:impl/c/packaging/nfpm.yaml"; do
    var=${pair%%:*} file=${pair#*:}
    count=$(printf '%s\n' "${!var}" | grep -c . || true)
    if [ "$count" -lt "$MIN_PATHS" ]; then
        echo "FAIL: only $count installed path(s) found in $file (expected at least $MIN_PATHS)." >&2
        echo "  This script can no longer read that manifest, so it is not checking anything." >&2
        echo "  Fix the extraction in installed_paths() rather than lowering MIN_PATHS." >&2
        exit 2
    fi
done

expected_c_only=$(printf '%s\n' "${EXPECTED_C_ONLY[@]}" | sort -u)

rust_only=$(comm -23 <(printf '%s\n' "$rust") <(printf '%s\n' "$c"))
c_only=$(comm -13 <(printf '%s\n' "$rust") <(printf '%s\n' "$c"))
unexpected_c_only=$(comm -23 <(printf '%s\n' "$c_only") <(printf '%s\n' "$expected_c_only"))
stale_exceptions=$(comm -13 <(printf '%s\n' "$c_only") <(printf '%s\n' "$expected_c_only"))

fail=0
if [ -n "${rust_only//[[:space:]]/}" ]; then
    echo "FAIL: installed by tedge-dot-rs (.goreleaser.yaml) but NOT by tedge-dot-c (impl/c/packaging/nfpm.yaml):" >&2
    printf '  %s\n' $rust_only >&2
    fail=1
fi
if [ -n "${unexpected_c_only//[[:space:]]/}" ]; then
    echo "FAIL: installed by tedge-dot-c but NOT by tedge-dot-rs:" >&2
    printf '  %s\n' $unexpected_c_only >&2
    echo "  If the difference is intentional, add it to EXPECTED_C_ONLY in this script with the reason." >&2
    fail=1
fi
if [ -n "${stale_exceptions//[[:space:]]/}" ]; then
    # Not a failure: an exception that is no longer needed only means the lists
    # converged. Say so, so the list does not rot.
    echo "NOTE: EXPECTED_C_ONLY lists paths that are no longer C-only; drop them:" >&2
    printf '  %s\n' $stale_exceptions >&2
fi

if [ "$fail" != 0 ]; then
    echo "== packaging manifests disagree on the installed layout" >&2
    exit 1
fi
shared=$(comm -12 <(printf '%s\n' "$rust") <(printf '%s\n' "$c") | grep -c . || true)
echo "== packaging manifests agree ($shared shared paths, $(printf '%s\n' "${EXPECTED_C_ONLY[@]}" | grep -c .) documented C-only)"
