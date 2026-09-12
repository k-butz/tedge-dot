# TODO

## In flight / next

* [ ] Ship profibus in the `tedge-dot-rs` package: the `profibus` cargo feature is excluded
      from the goreleaser builds because its serial dependency (`serialport` via `profirust`)
      has a native libudev build script that does not cross-compile with cargo-zigbuild.
      Options: disable the libudev feature upstream, vendor a libudev stub per target, or build
      the Linux packages natively per architecture. (`tedge-dot-c` already ships PROFIBUS, over
      `tcp://` only.)

* [ ] Close the remaining C parity gaps listed in `impl/c/README.md`. Each has a capability
      name already wired into the test tagging (`C_MISSING_CAPABILITIES` in the justfile), so
      implementing one means removing it from that list and adding the test that was waiting
      for it:
      - `opcua-security` — open62541 supports `Basic256Sha256` and friends; needs config +
        certificate plumbing, and a secured endpoint in the e2e stack to test against.
      - `canbus-fd` — classic frames only today; the Rust build has a `canbus-fd` feature.
      - `profibus-serial` — the C module speaks `tcp://` only (no serial PHY, no FDL token
        timing), so it cannot yet drive a multi-master RS-485 bus.
      Also: CAN bus push delivery (the C module renders the push-based bus as drain-into-cache
      polling — same samples, worse latency, so it is not tagged), and the 64-byte cap on
      string/raw values (`TDOT_RAW_MAX`).

* [ ] Cloud Fieldbus increments 3 + 4 (see `doc/rfc/0002-cloud-fieldbus-integration.md`;
      increments 1 + 2 shipped and verified live 2026-07-02): generalise the device-type
      translator per protocol, and the export path / UI-placeholder reconciliation (needs a
      tenant-side actor — a device cannot own or delete the UI-created managed object).
* [ ] Conformance suite implementation (`doc/conformance/conformance-suite.md` is spec'd,
      harness not built yet).
* [ ] Per-point `meta` support for the remaining flows: `ot-alarm` should read thresholds
      from `sample.meta`/measurement context so alarm limits can live next to the signal.
* [ ] File the upstream async-opcua issues (drafts ready in
      `doc/upstream/async-opcua-stranded-sample.md` and
      `doc/upstream/async-opcua-null-session-nonce.md`); drop `vendor/async-opcua-crypto`
      and the `[patch.crates-io]` entry once the nonce fix ships.
* [ ] c8y-fieldbus-import deferred items (script header TODOs): alarm/event/status mappings
      (gap G4), RTU serial-port resolution from `[connection.serial]`, signed and
      multi-register bit fields.
* [ ] Device parameters (RFC 0003, prototype implemented): persist the
      last-commanded value of write-only parameters across mapper restarts; derive
      `meta.parameter.set` from the Cloud Fieldbus device type name in `c8y-fieldbus-import`;
      optional Modbus FC16 fast path for `write-batch` on contiguous registers; optional
      `c8y_ParameterUpdate` audit event flow on top of the twin; gateway-level connector
      settings (poll_interval) as a tedge-parameter-plugin set script issuing `set-config`.
* [ ] Legacy write-payload compatibility (gap G2): accept explicit-address
      (`register`/`coil`/`address`/`ipAddress`) and name-based `metrics[]` payloads for
      `c8y_SetRegister`/`c8y_SetCoil`, not only `{point, value}`.

## Connector candidates

* ethercat — https://github.com/ethercrab-rs/ethercrab (MIT/Apache-2.0)
* EtherNet/IP — https://github.com/sergiogallegos/rust-ethernet-ip
* BACnet — spec sketch exists in `doc/connectors/_template-connector-spec.md`
* DNP3 — https://github.com/stepfunc/dnp3 — NOT possible (non-OSS license)

Score each implementation (functionality + maintainability incl. upstream library activity)
before promoting it past experimental.

## Validation policy

* Connectors must be validated with unit + integration tests and e2e simulator tests, and the
  tests must be proven by running them (see `doc/testing.md`).
* Shared SDK decode logic requires property-based tests; parsers of external input require a
  fuzz target.
