# tedge-dot C proof of concept

A C11 reimplementation of the tedge-dot SDK framework plus all five
connectors — **Modbus** (libmodbus), **OPC UA** (open62541), **CAN bus**
(SocketCAN + a minimal DBC parser), **CANopen** (expedited SDO client
directly over SocketCAN), and **PROFIBUS-DP** (a minimal built-in DP-V0
class-1 master, TCP transport) — exploring two questions:

1. how much smaller do the binaries get compared to the Rust implementation?
2. how feasible is a path to microcontrollers?

It speaks the same [OT Connector Contract](../doc/contract/) as the Rust
implementation: same TOML config files (the untouched configs in
[demo/config/](../demo/config/) work as-is), same MQTT topics, same JSON
sample/command envelopes (including the `access` echo and the runtime-provided
`write-batch` verb that the device-parameter flows rely on, see
[RFC 0003](../doc/rfc/0003-parameter-writes.md), whose parameter derivation and
`tedge-dot describe` rendering this build shares), the SDK management verbs
(`set-config`, `define-device`, `remove-device`: the runtime patches the
config document, validates it with the loader and the module, persists it and
live-reloads — comments are not preserved, tomlc99 being read-only), `raw`
mode and Modbus bit fields, same decode semantics (validated against the Rust
SDK's golden vectors). The one deliberate gap is push delivery (`subscribe`):
the PoC polls only, which its conformance manifests declare.

## Tests

The C build is held to the same coverage as the Rust one:

- **unit tests** — `just c-test` (`ctest --test-dir poc-c/build`): the golden decode vectors
  shared with the Rust SDK, and the device-parameter/`describe` checks, which assert the same
  facts over the same fixture config as `crates/sdk/src/descriptor.rs`;
- **describe parity** — [`ci/describe-parity.sh`](ci/describe-parity.sh) (`just
  c-describe-parity`) renders the Cumulocity DTM definitions of every connector config in the
  repo with both binaries and compares them parsed, so the tenant-side declaration cannot drift
  between the implementations; CI runs it in the `c-poc` job;
- **e2e Robot suites** — the per-protocol stacks under [connectors/](../connectors/) run
  their Robot suite against the C connector with `just test-e2e-c <proto>` (the stack's
  `connector` service is swapped for [`connectors/_shared/Dockerfile.connector-c`](../connectors/_shared/Dockerfile.connector-c));
  CI runs it for every protocol (`e2e-c` matrix);
- **cloud Robot suites** — the live Cumulocity harness under [cloud/](../cloud/) builds this
  implementation into the thin-edge demo image with `just test-cloud-c <proto>` (`IMPL=c`
  selects a connector-install stage that compiles poc-c/ instead of installing the Rust
  package), so child registration, measurements, operations, the Cloud Fieldbus import and the
  device-parameter round-trip are all covered for the C build too;
- **smoke** — [`ci/smoke.sh <proto>`](ci/smoke.sh), a fast broker-and-simulator check
  without Docker for the connector itself (used by the `c-poc` CI job).

- **conformance** — the contract conformance suite (layers 1-3, built-in broker and
  simulators) runs against the C binary as an external connector:
  `just conformance-c modbus|opcua` (manifests `connectors/<proto>/conformance-c.toml`,
  same claims as the Rust ones except `subscribe`); CI runs it in the `c-poc` job. Both
  connectors are fully conformant, hot reload through the management verbs included.

Liveness: the C runtime has no equivalent of the Rust SDK's `operation_timeout` /
`stall_timeout`. It relies on the response timeouts of libmodbus and open62541, which is enough
for conformance check B5's silent peer (verified for both protocols), but a call that hangs
*inside* a library would still wedge the C loop. Those two config keys are accepted and ignored.

Debugging: `TDOT_OPCUA_DEBUG=1` keeps open62541's client handshake log on stdout.

## Packaging & releases

`.github/workflows/release-c.yaml` builds, tests and packages the C build as a
**drop-in replacement** for the Rust `tedge-dot` package: same `/usr/bin/tedge-dot`
binary, same `tedge-dot.service`, same `/etc/tedge/plugins/ot/` config layout,
so the C package (`tedge-dot-c`) *conflicts* with the Rust one — install one or
the other. It additionally ships the **PROFIBUS** connector, which the Rust
package omits.

- Trigger it manually (**workflow_dispatch**) to refresh the rolling
  **`c-snapshot`** pre-release, so the latest build is always one download away.
- Push a **`c-v*`** tag (e.g. `c-v0.1.0`) for a versioned pre-release. (The Rust
  release workflow ignores `c-v*` tags.)

Both publish `.deb` / `.rpm` / `.apk` + tarballs for `amd64`, `arm64` and
`armhf`, plus a `SHA256SUMS` covering every asset.

Packaging is done with **nfpm** rather than goreleaser: goreleaser's headline
feature is cross-compiling Go/Rust with zig, and it has no notion of a C build
that also links *system* shared libraries. The build itself does use zig — see
below.

### Cross-compilation

Every architecture is cross-compiled on one machine by [cross/](cross/): a
Debian container where **zig supplies the compiler and libc** and **Debian
supplies the target-architecture shared libraries** (`libmodbus`,
`libmosquitto`, `libcjson`) via multiarch. open62541 is not packaged by Debian,
so it is still built from source — cross-compiled with the same toolchain and
statically linked.

The point is not only that one runner builds every architecture. zig lets us
**pin the minimum glibc** (2.17 — Debian 8 / RHEL 7 era), which a native build
fundamentally cannot: a binary built on `ubuntu-24.04` hard-requires glibc 2.39
on the target, which rules out most OT gateways in the field.

```sh
just c-cross-image            # build the toolchain image
just c-cross arm64            # -> poc-c/dist/arm64/tedge-dot
just c-verify arm64           # golden vectors on Debian bullseye (glibc 2.31)
just c-package arm64 0.1.0    # -> poc-c/dist/packages/*.{deb,rpm,apk}
just c-cross-all              # every architecture in C_ARCHS
```

`just c-verify` for a non-native architecture needs binfmt/qemu registered:
`docker run --privileged --rm tonistiigi/binfmt --install all`.

The packaged systemd unit runs `tedge-dot run /etc/tedge/plugins/ot` — a
**directory**, so one process runs every connector config it contains (one
worker thread per file), matching the Rust single-service model.

## Layout

| Path | Contents | Rust counterpart |
|---|---|---|
| `sdk/include/tedge_dot/` | public headers: model, config, connector vtable, decode, runtime | `crates/sdk` |
| `sdk/src/` | config loader (tomlc99), decode/encode, envelope builder (cJSON), poll-loop runtime (mosquitto), device parameters + Cumulocity DTM rendering | `crates/sdk` |
| `connectors/modbus/` | libmodbus connector (TCP + RTU, 4 tables, typed decode, writes) | `crates/connector-modbus` |
| `connectors/opcua/` | open62541 connector (client session, node-id points, typed reads/writes) | `crates/connector-opcua` |
| `connectors/canbus/` | SocketCAN connector + minimal DBC parser (BO_/SG_, Intel & Motorola layouts) | `crates/connector-canbus` |
| `connectors/canopen/` | expedited SDO client over raw SocketCAN (no CANopen library) | `crates/connector-canopen` |
| `connectors/profibus/` | minimal DP-V0 master (Diag→Prm→Cfg→Data_Exchange, bus thread, tcp:// transport) | `crates/connector-profibus` |
| `src/main.c` | `read` / `write` / `run` / `describe` CLI | `src/main.rs` |
| `tests/golden.c` | conformance runner for `crates/sdk/conformance/vectors.json` | `tests/golden_vectors.rs` |
| `tests/describe.c` | device-parameter derivation + DTM rendering checks | `crates/sdk/src/descriptor.rs` tests |
| `ci/describe-parity.sh` | `tedge-dot describe` output compared between the Rust and C binaries | — |
| `ci/smoke.sh` | e2e smoke: connector ⇄ simulator ⇄ broker, per protocol (used by the `c-poc` CI job) | conformance/e2e suites |
| `cross/` | zig + Debian-multiarch cross-compilation image, build and verify scripts | goreleaser/zig |
| `third_party/tomlc99/` | vendored TOML parser (MIT) | serde/toml |

The Rust `Connector` trait maps to a C vtable (`tdot_connector_t` in
[connector.h](sdk/include/tedge_dot/connector.h)): `configure`,
`connect_device`, `read_point`, `write_point`, `disconnect_device`, plus the
optional `device_info` (the link status `info` descriptor, which the c8y
registration flow turns into the `c8y_ModbusDevice` twin fragment). Protocol
modules are selected by `tdot_connector_factory(protocol)` and compiled in
behind CMake options (`-DTDOT_MODBUS=ON/OFF`, `-DTDOT_OPCUA=ON/OFF`) —
the C analogue of the cargo feature flags.

## Build & run

Dependencies: cmake, pkg-config, libmodbus, mosquitto (client lib), cJSON.
open62541 is used from the system when installed, otherwise CMake builds
v1.5.5 from source (client-only feature set) via FetchContent. The CAN
connectors are Linux-only (SocketCAN) and compile-gated automatically.
On macOS: `brew install libmodbus open62541 mosquitto cjson`.

```sh
cmake -B build -G Ninja        # MinSizeRel by default
cmake --build build

# against the demo simulators (`just sim modbus`, `just sim opcua`):
./build/tedge-dot read  -c ../demo/config/modbus.toml
./build/tedge-dot read  -c ../demo/config/opcua.toml -d opc1 -p temperature --json
./build/tedge-dot write -c ../demo/config/modbus.toml -d plc1 -p coil_rw --value true
./build/tedge-dot run   -c ../demo/config/modbus.toml --output stdout --duration 10s
./build/tedge-dot run   -c ../demo/config/opcua.toml   # publishes to MQTT broker

# the writable points as Cumulocity DTM property definitions, for a tenant admin to
# register once (needs no device, broker or protocol module):
./build/tedge-dot describe -c ../demo/config/modbus.toml
./build/tedge-dot describe -c ../demo/config/modbus.toml -d plc1 --compact

# tests
./build/tedge-dot-golden ../crates/sdk/conformance/vectors.json
./build/tedge-dot-describe
```

## What was verified (2026-07-02/03, against the demo simulators)

- **Modbus**: all six demo points read correctly (uint16 17001, scaled 17.001 °C
  with `decimal_shift = -3`, uint32 617001, float32 404.17, coil), Modbus
  exception on `bad_point` → `quality: "bad"` with error reason, register +
  coil write round-trips.
- **OPC UA**: float64/uint32/int32/bool reads, `BadNodeIdUnknown` →
  `quality: "bad"`, int32 + bool write round-trips.
- **CAN bus** (Linux/vcan0): RPM 2500, coolant 85, brake true decoded from the
  sim's ENGINE_STATUS broadcasts via the DBC; a write sends the encoded
  read-modify-write frame on the bus. DBC bit-layout logic (Intel + Motorola,
  signed, encode round-trips) covered by a self-test against `sim/test.dbc`.
- **CANopen** (Linux/vcan0): SDO expedited uploads (uint16 1234, int16 -100,
  uint8 1) and a download round-trip (`digital_out` 1→0→1); SDO aborts map to
  `quality: "bad"` with the abort code.
- **PROFIBUS-DP** (TCP): full Diag→Set_Prm→Chk_Cfg→Data_Exchange bring-up
  against the DP-V0 slave sim; seeded inputs decode (0x0A, bit 3, 0x1234,
  100), output-byte write lands in the slave's PI_Q, and back-to-back
  sessions reconnect cleanly.
- **MQTT contract**: retained health (`up`/`down` + last-will), retained
  capability descriptor, retained per-device link status, non-retained samples
  on `te/device/<dev>/ot/<protocol>/sample/<point>`, and inbound
  `cmd/write/<id>` commands answered with retained
  `{"status":"successful"|"failed"}` results.
- **Recovery**: killing the simulator flips the link to `disconnected` and
  starts the 1s→60s exponential reconnect backoff.
- **Decode conformance**: all 73 golden vectors pass (every datatype,
  endianness/word-order combination, NaN/±Inf, out-of-JS-safe-range 64-bit →
  string, bitfields, round-trips).

## Size comparison

Apples-to-apples is tricky (the Rust binary is one static binary with five
protocols; the C PoC dynamically links two protocol libraries), but the
totals on this machine:

| | size |
|---|---|
| Rust `tedge-dot` release binary (aarch64-linux, 5 protocols, static) | **12 MB** |
| **C PoC binary, all 5 protocols, open62541 statically linked (x86_64 Linux, MinSizeRel)** | **457 KB** |
| C PoC binary, modbus + opcua, shared open62541 (arm64 macOS) | 108 KB |
| C PoC binary, modbus only | 91 KB |
| + libmodbus.dylib | 75 KB |
| + libmosquitto.dylib | 160 KB |
| + libcjson.dylib | 88 KB |

The headline number: **all five protocols in 457 KB** (open62541 built
client-only and statically linked; libmodbus/mosquitto/cJSON still dynamic,
~320 KB more if counted) — about **25× smaller** than the Rust binary. The
canbus/canopen/profibus connectors add ~40 KB total because their protocol
logic (DBC parsing, SDO framing, DP-V0 master) is implemented directly rather
than pulled in as libraries.

## Microcontroller path

What this PoC shows about the MCU question:

- **The contract layer is MCU-ready.** The decode/encode/scaling core and the
  envelope model have no OS dependencies beyond libc (the golden-vector suite
  would run on bare metal). tomlc99 and cJSON are portable C99 with small
  footprints.
- **open62541 explicitly supports MCUs** (it has ports for FreeRTOS+lwIP,
  Zephyr) — the OPC UA connector logic here uses only the high-level client
  API that exists on those ports.
- **The Modbus connector would swap libmodbus for a bare-metal Modbus layer**
  on MCUs (libmodbus assumes POSIX sockets/termios). Modbus framing is simple
  enough that this is a small, well-bounded port.
- **CANopen and PROFIBUS already have zero library dependencies** — the SDO
  client and the DP-V0 master are plain C over a socket/byte stream, so on an
  MCU only the transport (CAN driver / UART) needs swapping. The DBC parser
  is libc-only.
- **The runtime (poll loop, backoff, scheduler) is a single thread with no
  dynamic task machinery** — it maps naturally onto an RTOS task. The MQTT
  output would use an embedded client (e.g. coreMQTT) behind the same
  `publish()` seam in `runtime.c`.

## PoC scope cuts (vs. the Rust implementation)

- **Polling only** — no OPC UA monitored-item push subscriptions, and the
  push-based canbus connector is rendered as drain-into-cache polling
  (`read_point` waits up to ~1.2 s for the first broadcast of a frame).
- **CANopen is expedited-SDO only** (values ≤ 4 bytes; segmented transfers
  report a bad sample), and **PROFIBUS is tcp:// transport only** (no serial
  PHY, no FDL token timing — fine for the sim, not yet for a multi-master
  RS-485 bus).
- **No OPC UA security** — `security_policy = "None"` only (open62541
  supports Basic256Sha256 etc.; wiring it up is config + cert plumbing).
- `stale` quality (last-good cache) is not implemented.
- **No stall watchdog** — the Rust runtime bounds every protocol call and
  restarts a connector that stops making progress (`operation_timeout` /
  `stall_timeout`, contract §8.1); here the protection is whatever response
  timeout the protocol library enforces (libmodbus and open62541 both have
  one), which is why the conformance silent-peer checks pass.
- Fixed-size buffers cap strings/raw values at 64 bytes.

## Licensing

Everything linked here is copyleft-safe for the intended packaging:
libmodbus (LGPL-2.1+, dynamically linked), open62541 (MPL-2.0),
mosquitto client library (EPL/EDL dual), cJSON (MIT), tomlc99 (MIT, vendored).
