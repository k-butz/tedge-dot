#!/usr/bin/env bash
# Keep the `requires:<capability>` test tags and the declared capability lists in step.
#
#   connectors/_shared/check-capability-tags.sh
#
# The parity mechanism has a quiet failure mode in each direction:
#
#   * a MISTYPED tag (`requires:subscibe`) is in no implementation's missing list, so it is
#     never skipped -- the test runs against a build that cannot pass it and fails for a
#     reason no one will connect to a typo;
#   * a capability declared missing but carried by NO test means the `--skip` does nothing.
#     The suite is green either way, so the parity table would be claiming an enforcement that
#     does not exist.
#
# This script fails on the first and reports the second, so both stay visible. Run by CI and
# by `just check-capability-tags`.
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"

# The declared lists live in the justfile, which is the single source of truth.
read_var() {
    sed -n "s/^$1 := \"\(.*\)\"$/\1/p" justfile
}
known=$(read_var KNOWN_CAPABILITIES)
c_missing=$(read_var C_MISSING_CAPABILITIES)

if [ -z "$known" ]; then
    echo "FAIL: KNOWN_CAPABILITIES not found in the justfile" >&2
    exit 2
fi

in_list() {
    local needle=$1 item
    shift
    for item in $1; do
        [ "$item" = "$needle" ] && return 0
    done
    return 1
}

fail=0

# 1. Every tag a suite uses must be a known capability.
tags=$(grep -rhoE "requires:[A-Za-z0-9_-]+" connectors cloud --include="*.robot" --include="*.resource" \
       | sort -u | sed 's/^requires://')
for tag in $tags; do
    if ! in_list "$tag" "$known"; then
        echo "FAIL: tests are tagged 'requires:$tag', which is not in KNOWN_CAPABILITIES." >&2
        echo "      A tag no implementation declares is never skipped -- check for a typo," >&2
        echo "      or add the capability to KNOWN_CAPABILITIES in the justfile." >&2
        fail=1
    fi
done

# 2. Every capability an implementation declares missing must also be known.
for cap in $c_missing; do
    if ! in_list "$cap" "$known"; then
        echo "FAIL: C_MISSING_CAPABILITIES lists '$cap', which is not in KNOWN_CAPABILITIES." >&2
        fail=1
    fi
done

# 3. Report capabilities nothing is tagged with. Not a failure: a gap can legitimately be
#    documented before anyone has built the rig to test it. But it must not be invisible --
#    the parity table in impl/c/README.md marks these "no test yet" and this is what keeps
#    that column honest.
untested=""
for cap in $known; do
    in_list "$cap" "$tags" || untested="$untested $cap"
done
if [ -n "$untested" ]; then
    echo "NOTE: no test is tagged with:$untested"
    echo "      Skipping these for an implementation that lacks them is therefore a no-op."
    echo "      They must be listed as 'documented, no test yet' in impl/c/README.md."
fi

if [ "$fail" != 0 ]; then
    echo "== capability tag check FAILED" >&2
    exit 1
fi
echo "== capability tags consistent (tagged:$(echo " $tags" | tr '\n' ' '))"
