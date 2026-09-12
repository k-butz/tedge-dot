# Build / package recipes for the Rust tedge-dot.

set dotenv-load := true

# Default cross-compilation target and matching package architecture.
TARGET := "aarch64-unknown-linux-musl"
PKG_ARCH := "arm64"
VERSION := `awk -F '"' '/^version = /{print $2; exit}' Cargo.toml`

# Create/refresh the single Python virtualenv used by every system test (and by the editor,
# see .vscode/settings.json).
venv:
    #!/usr/bin/env bash
    set -euo pipefail
    # A .venv copied from another checkout keeps that checkout's path in its scripts, so its
    # `pip` would install into the OTHER project. Recreate unless this venv was made here.
    if [ -d .venv ] && ! grep -q "venv $PWD/.venv\$" .venv/pyvenv.cfg 2>/dev/null; then
        echo "recreating ./.venv (it was not created for this checkout)" >&2
        rm -rf .venv
    fi
    [ -d .venv ] || python3 -m venv .venv
    # Always go through `python -m pip`: immune to a stale shebang in .venv/bin/pip.
    ./.venv/bin/python -m pip install -q --upgrade pip
    ./.venv/bin/python -m pip install -q -r requirements-test.txt
    # Fail loudly if anything landed outside this venv.
    ./.venv/bin/python - <<'EOF'
    import pathlib, sys, DeviceLibrary, robot
    here = pathlib.Path(".venv").resolve()
    for mod in (DeviceLibrary, robot):
        path = pathlib.Path(mod.__file__).resolve()
        if here not in path.parents:
            sys.exit(f"{mod.__name__} resolved outside ./.venv: {path}")
    print("venv ready: ./.venv (robot, DeviceLibrary, robotframework-c8y)")
    EOF

# Run the Rust unit + integration tests
test *args="":
    cargo test --workspace {{args}}

# Lint
lint:
    cargo clippy --workspace --all-targets -- -D warnings

# Run the SDK property-based tests only (proptest; part of `just test` too).
test-properties:
    cargo test -p tedge-dot-sdk --test properties

# Run the OT connector conformance suite (layers 1-3; built-in broker + simulator, no
# hardware). Usage: just conformance modbus [extra ot-conformance flags]
conformance protocol="modbus" *args="":
    cargo run -p ot-conformance -- check --spec connectors/{{protocol}}/conformance.toml {{args}}

# The same conformance suite against the C build (poc-c/), launched as an external connector
# through the `[harness] command` of connectors/<proto>/conformance-c.toml. Build the C binary
# first: cmake -S poc-c -B poc-c/build && cmake --build poc-c/build
# Usage: just conformance-c modbus
conformance-c protocol="modbus" *args="":
    cargo run -p ot-conformance -- check --spec connectors/{{protocol}}/conformance-c.toml {{args}}

# Compile-check the Linux-only code paths (SocketCAN connectors are cfg-gated and silently
# skipped by a macOS `cargo build`). profibus is excluded: its serial dependency has a native
# build script that needs Linux headers — it is covered by the Docker e2e build instead.
check-linux target=TARGET:
    cargo check -p connector-canbus -p connector-canopen --target {{target}}

# Fuzz one SDK target (decode_primitive, config_toml, transform, sample_envelope).
# Requires: rustup nightly + `cargo install cargo-fuzz`.
# Usage: just fuzz decode_primitive 60
fuzz target="decode_primitive" seconds="60":
    cd crates/sdk && cargo +nightly fuzz run {{target}} -- -max_total_time={{seconds}}

# Fuzz every SDK target briefly (CI smoke; ~2 min total).
fuzz-all seconds="30":
    cd crates/sdk && for t in decode_primitive config_toml transform sample_envelope; do \
        cargo +nightly fuzz run $t -- -max_total_time={{seconds}} || exit 1; done

# Validate the thin-edge flows offline with `tedge flows test` (no broker/device/cloud).
test-flows:
    ./flows/test-flows.sh

# --- All-in-one demo ---------------------------------------------------------
# Start every OT simulator (modbus, opcua, canbus, canopen, profibus) from one
# compose file. Pairs with the connectors installed from the tedge-dot package
# and run by the single tedge-dot.service. See demo/README.md.

# Bring up all simulators (build + start).
demo-sims-up:
    docker compose -f demo/docker-compose.yaml up -d --build
    @echo "All OT simulators are up. Install the tedge-dot package and start tedge-dot.service to run the connectors."

# Tear down all simulators.
demo-sims-down:
    docker compose -f demo/docker-compose.yaml down -v

# Show simulator status / logs.
demo-sims-status:
    docker compose -f demo/docker-compose.yaml ps

demo-sims-logs *args="":
    docker compose -f demo/docker-compose.yaml logs -f {{args}}

# --- Local exploration -------------------------------------------------------
# Spin up a single simulator and poke it with the CLI (no MQTT broker / cloud).
# See demo/README.md and connectors/README.md for the full quickstart.

# Start the protocol simulator in Docker (pairs with demo/config/<proto>.toml), pinning the
# host port the demo configs expect — the e2e stacks leave it ephemeral so they stay
# parallel-safe. Usage: just sim modbus   just sim opcua
sim proto:
    #!/usr/bin/env bash
    set -euo pipefail
    export $(just _sim-port {{proto}})
    docker compose -p tedge-dot-sim-{{proto}} -f connectors/{{proto}}/docker-compose.yaml up -d --build --wait simulator
    echo "{{proto}} simulator ready — see demo/config/{{proto}}.toml for usage"

# Stop the protocol simulator container.
sim-down proto:
    #!/usr/bin/env bash
    set -euo pipefail
    export $(just _sim-port {{proto}})
    docker compose -p tedge-dot-sim-{{proto}} -f connectors/{{proto}}/docker-compose.yaml rm -sf simulator

# The fixed simulator host port a protocol's demo config expects (empty = protocol has none).
_sim-port proto:
    #!/usr/bin/env bash
    case "{{proto}}" in
        modbus)   echo "MODBUS_SIM_PORT=5020" ;;
        opcua)    echo "OPCUA_SIM_PORT=4840" ;;
        profibus) echo "PROFIBUS_SIM_PORT=9200" ;;
        *)        echo "UNUSED_SIM_PORT=" ;;
    esac

# Run the MQTT end-to-end suite for a protocol. The Robot suite starts and stops the stack
# itself (DeviceLibrary, see connectors/_shared/stack.resource) — no `docker compose up` first,
# and every run gets its own randomly named compose project.
# Usage: just test-e2e modbus   just test-e2e opcua
test-e2e proto *args="":
    just _e2e {{proto}} rust "{{args}}"

# Same suite, same stack, but the connector is the C implementation (poc-c/): the Rust and
# C connectors are maintained to the same contract, so they get the same e2e coverage.
# Usage: just test-e2e-c modbus
test-e2e-c proto *args="":
    just _e2e {{proto}} c "{{args}}"

# Shared body of test-e2e / test-e2e-c. `impl` is "rust" (the stack's own Dockerfile.connector)
# or "c" (connectors/_shared/Dockerfile.connector-c, selected via CONNECTOR_DOCKERFILE).
_e2e proto impl args:
    #!/usr/bin/env bash
    set -euo pipefail
    outdir=connectors/{{proto}}/output
    export IMPL={{impl}}
    if [ "{{impl}}" = "c" ]; then
        outdir=connectors/{{proto}}/output-c
        export CONNECTOR_DOCKERFILE=connectors/_shared/Dockerfile.connector-c
    fi
    just venv
    ./.venv/bin/python -m robot \
        --outputdir "$outdir" --variable IMPL:{{impl}} {{args}} \
        connectors/{{proto}}/tests/

# Bring a stack up manually for inspection, with the host ports pinned (the test stacks use
# ephemeral ones). Tear it down with `just e2e-down <proto> [impl]`.
# Usage: just e2e-up modbus [c]
e2e-up proto impl="rust":
    #!/usr/bin/env bash
    set -euo pipefail
    export IMPL={{impl}} BROKER_PORT=1884
    export $(just _sim-port {{proto}})
    [ "{{impl}}" = "c" ] && export CONNECTOR_DOCKERFILE=connectors/_shared/Dockerfile.connector-c || true
    docker compose -p tedge-dot-{{proto}}-manual -f connectors/{{proto}}/docker-compose.yaml up -d --build --wait
    echo "stack up: broker on localhost:1884 (canbus/canopen: see the compose file)"

# Tear down a manually started stack.
e2e-down proto impl="rust":
    #!/usr/bin/env bash
    set -euo pipefail
    export IMPL={{impl}}
    docker compose -p tedge-dot-{{proto}}-manual -f connectors/{{proto}}/docker-compose.yaml down -v

# Cross-compile + build all packages
build:
    goreleaser release --snapshot --clean

# Build the deb fully inside Docker (no host toolchain needed); writes to ../tests/data
test-data-docker pkg_arch=PKG_ARCH:
    @mkdir -p ../tests/data
    docker build -f Dockerfile.package --build-arg PKG_ARCH={{pkg_arch}} --target export --output ../tests/data .

# Start a shell in the tedge container of a manually started cloud stack
# (after `just cloud-up <proto>`).
shell proto *args='bash':
    docker compose -p tedge-dot-cloud-{{proto}} -f cloud/{{proto}}/docker-compose.yaml exec tedge {{args}}

# Full Cumulocity end-to-end for a protocol. The Robot suite starts the stack, bootstraps a
# freshly named device and deletes it from the tenant again (DeviceLibrary, see
# cloud/_shared/device.resource) — no compose up and no manual cleanup.
# Requires C8Y_BASEURL / C8Y_USER / C8Y_PASSWORD / C8Y_TENANT in the env or .env.
# Run `just build` first so dist/ holds the packages the image installs.
# Usage: just test-cloud modbus
test-cloud proto *args="":
    #!/usr/bin/env bash
    set -euo pipefail
    just venv
    ./.venv/bin/python -m robot \
        --outputdir cloud/{{proto}}/output {{args}} \
        cloud/{{proto}}/tests/

# Bring a cloud stack up manually for inspection (fixed project name, DEVICE_ID from the env),
# e.g. to poke at the mapper or run bootstrap.sh by hand. The test suites do NOT need this.
cloud-up proto:
    #!/usr/bin/env bash
    set -euo pipefail
    just build
    docker compose -p tedge-dot-cloud-{{proto}} -f cloud/{{proto}}/docker-compose.yaml up -d --build --wait
    docker compose -p tedge-dot-cloud-{{proto}} -f cloud/{{proto}}/docker-compose.yaml exec -T tedge bootstrap.sh

# Tear down a manually started cloud stack.
cloud-down proto:
    docker compose -p tedge-dot-cloud-{{proto}} -f cloud/{{proto}}/docker-compose.yaml down -v

# Delete leftover test devices from the tenant. The suites clean up after themselves; this is
# the fallback for runs that crashed before their teardown. Device ids generated by
# DeviceLibrary all start with "TST_".
cleanup PATTERN="TST_*" $CI="true":
    #!/usr/bin/env bash
    set -euo pipefail
    echo "Removing devices matching '{{PATTERN}}' (and their certificates + users)"
    tenant="$(c8y currenttenant get --select name --output csv)"
    c8y devicemanagement certificates list -n --tenant "$tenant" --filter "name like {{PATTERN}}" --pageSize 2000 \
        | c8y devicemanagement certificates delete --tenant "$tenant" --silentStatusCodes 404 || true
    c8y inventory find -n --query "name eq '{{PATTERN}}'" -p 100 | c8y inventory delete --silentStatusCodes 404 || true
    c8y users list -n --tenant "$tenant" --filter "userName like device_{{PATTERN}}" --pageSize 2000 \
        | c8y users delete --tenant "$tenant" --silentStatusCodes 404 || true

# --- C proof of concept (poc-c/) ---------------------------------------------
#
# The C build is cross-compiled with zig inside a Debian multiarch container
# (poc-c/cross/), so one host builds every architecture and the binaries carry
# a glibc floor we choose (2.17 by default) rather than the build host's.

# Build the C PoC natively and run its unit tests: the golden decode vectors shared with
# the Rust SDK, plus the device-parameter/`describe` checks.
# Usage: just c-test [extra ctest flags]
c-test *args="":
    cmake -B poc-c/build -S poc-c
    cmake --build poc-c/build
    ctest --test-dir poc-c/build --output-on-failure {{args}}

# Check that `tedge-dot describe` renders identical Cumulocity DTM definitions in the Rust
# and C builds. With no argument every connector config in the repo is compared.
# Usage: just c-describe-parity [config.toml ...]
c-describe-parity *configs="":
    ./poc-c/ci/describe-parity.sh {{configs}}

# Debian architectures the C PoC is built and packaged for.
C_ARCHS := "amd64 arm64 armhf"
C_GLIBC_MIN := "2.17"

# Build the zig cross-compilation image.
c-cross-image:
    docker build -t tedge-dot-cross poc-c/cross

# Cross-build the C PoC for one architecture into poc-c/dist/<arch>/.
# Usage: just c-cross arm64 [extra cmake args]
c-cross arch="arm64" *args="": c-cross-image
    mkdir -p "poc-c/dist/{{arch}}"
    docker run --rm \
        -v "$PWD:/src:ro" -v "$PWD/poc-c/dist/{{arch}}:/out" \
        -e ARCH={{arch}} -e GLIBC_MIN={{C_GLIBC_MIN}} \
        tedge-dot-cross {{args}}

# Cross-build every architecture in C_ARCHS.
c-cross-all: c-cross-image
    #!/usr/bin/env bash
    set -euo pipefail
    for arch in {{C_ARCHS}}; do just c-cross "$arch"; done

# Run the golden decode vectors for a cross-built architecture on an old distro
# (Debian bullseye, glibc 2.31) — checks both the cross build and the glibc floor.
# Non-native architectures need binfmt/qemu:
#   docker run --privileged --rm tonistiigi/binfmt --install all
c-verify arch="arm64":
    docker run --rm --platform linux/{{ if arch == "armhf" { "arm/v7" } else { arch } }} \
        -v "$PWD/poc-c/dist/{{arch}}:/out" \
        -v "$PWD:/src:ro" \
        -v "$PWD/poc-c/cross/verify.sh:/verify.sh:ro" \
        debian:bullseye-slim /verify.sh

# Package one cross-built architecture as deb/rpm/apk into poc-c/dist/packages/.
# Usage: just c-package arm64 0.1.0
c-package arch="arm64" version="0.0.0-dev":
    #!/usr/bin/env bash
    set -euo pipefail
    # nfpm expands env vars in scalar fields but not in contents[].src, so stage
    # the architecture's binary at the fixed path nfpm.yaml points to.
    mkdir -p poc-c/dist/staged poc-c/dist/packages
    cp "poc-c/dist/{{arch}}/tedge-dot" poc-c/dist/staged/tedge-dot
    case "{{arch}}" in
        armhf) nfpm_arch=arm7 ;;
        *)     nfpm_arch="{{arch}}" ;;
    esac
    for format in deb rpm apk; do
        docker run --rm -v "$PWD:/work" -w /work \
            -e ARCH="$nfpm_arch" -e VERSION="{{version}}" \
            ghcr.io/goreleaser/nfpm:latest \
            pkg -f poc-c/packaging/nfpm.yaml -p "$format" -t poc-c/dist/packages/
    done
    ls -l poc-c/dist/packages
