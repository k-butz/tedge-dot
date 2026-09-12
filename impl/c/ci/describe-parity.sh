#!/usr/bin/env bash
# `tedge-dot describe` parity check: the Rust and C binaries must render the
# same Cumulocity DTM property definitions for the same connector config.
#
#   impl/c/ci/describe-parity.sh [config.toml ...]
#
# With no arguments every connector config in the repo is checked (the demo
# configs, the e2e connector configs and the cloud harness config). Expects
# impl/c/build/tedge-dot and a Rust binary (impl/rust/target/{debug,release}/tedge-dot,
# or $RUST_BIN); the Rust one is built with cargo when neither exists.
#
# Key ORDER is allowed to differ — serde_json sorts object keys, cJSON keeps
# insertion order — so the documents are compared parsed, not as text. Numbers
# compare by value (0.0 == 0), which is what both JSON readers and the DTM
# service see.
set -euo pipefail

repo=$(cd "$(dirname "$0")/../../.." && pwd)

# Point libraries (contract §3.4) referenced by name resolve against the
# installed search path, which does not exist in a checkout — so point it at
# the ones in the repo. Every `points.d` here, since each stack and the demo
# carry their own.
library_path=""
for dir in "$repo"/demo/points.d "$repo"/connectors/*/points.d; do
    [ -d "$dir" ] || continue
    library_path="${library_path:+$library_path:}$dir"
done
export TEDGE_DOT_POINT_LIBRARY_PATH="$library_path"
c_bin="$repo/impl/c/build/tedge-dot"
rust_bin=${RUST_BIN:-}

if [ ! -x "$c_bin" ]; then
    echo "FAIL: $c_bin not built (cmake -B impl/c/build -S impl/c && cmake --build impl/c/build)" >&2
    exit 2
fi
if [ -z "$rust_bin" ]; then
    for candidate in "$repo/impl/rust/target/debug/tedge-dot" "$repo/impl/rust/target/release/tedge-dot"; do
        if [ -x "$candidate" ]; then
            rust_bin=$candidate
            break
        fi
    done
fi
if [ -z "$rust_bin" ]; then
    echo "== building the Rust binary (no impl/rust/target/{debug,release}/tedge-dot found)"
    (cd "$repo" && cargo build --quiet --manifest-path impl/rust/Cargo.toml)
    rust_bin="$repo/impl/rust/target/debug/tedge-dot"
fi

compare=$(mktemp)
trap 'rm -f "$compare"' EXIT
cat >"$compare" <<'PYEOF'
import json, os, sys

def docs(text):
    return [json.loads(line) for line in text.splitlines() if line.strip()]

name = os.environ["NAME"]
rust = docs(os.environ["RUST_OUT"])
c = docs(os.environ["C_OUT"])
if rust == c:
    print(f"OK   {name}: {len(rust)} definition(s) identical")
    sys.exit(0)
print(f"FAIL {name}: definitions differ", file=sys.stderr)
print("  rust:", json.dumps(rust, sort_keys=True, indent=2), file=sys.stderr)
print("  c:   ", json.dumps(c, sort_keys=True, indent=2), file=sys.stderr)
sys.exit(1)
PYEOF

configs=("$@")
if [ ${#configs[@]} -eq 0 ]; then
    configs=("$repo"/demo/config/*.toml "$repo"/connectors/*/connector.toml
             "$repo"/cloud/modbus/modbus.toml)
fi

# stdout is the JSON and stderr carries diagnostics (a config with no device `type` is
# warned about, §5.2) — merging them would feed a warning line to the JSON parser below.
rust_errs=$(mktemp)
c_errs=$(mktemp)
trap 'rm -f "$compare" "$rust_errs" "$c_errs"' EXIT

fail=0
for config in "${configs[@]}"; do
    name=${config#"$repo"/}
    if ! rust_out=$("$rust_bin" describe -c "$config" --compact 2>"$rust_errs"); then
        echo "FAIL $name: the Rust binary rejected the config:" >&2
        cat "$rust_errs" >&2
        fail=1
        continue
    fi
    if ! c_out=$("$c_bin" describe -c "$config" --compact 2>"$c_errs"); then
        echo "FAIL $name: the C binary rejected the config:" >&2
        cat "$c_errs" >&2
        fail=1
        continue
    fi
    # Diagnostics are part of the CLI contract too: the untyped-device warning (§5.2) must read
    # the same from either binary, or a user gets different advice depending on the package.
    if ! diff -u "$rust_errs" "$c_errs" >/dev/null; then
        echo "FAIL $name: the two binaries print different diagnostics:" >&2
        diff -u "$rust_errs" "$c_errs" >&2 || true
        fail=1
        continue
    fi
    if ! NAME="$name" RUST_OUT="$rust_out" C_OUT="$c_out" python3 "$compare"; then
        fail=1
    fi
done

if [ "$fail" != 0 ]; then
    echo "== describe parity FAILED" >&2
    exit 1
fi
echo "== describe parity passed"
