# RFC 0003: Writing to OT devices from the cloud — device parameters

Status: proposed — implemented as a working prototype alongside this RFC (SDK, CLI, flows, operations, e2e and cloud suites)

## Problem

Reading OT points into the cloud is solved (samples → flows → measurements). Writing back is
only half solved: the connector contract has a `write` verb and Cumulocity's legacy
`c8y_SetRegister`/`c8y_SetCoil` operations are shimmed onto it, but

* those operations are Modbus-flavoured, need a per-protocol shim, and have no UI beyond a raw
  operation dialog;
* an operator has no way to *see* the current value of a writable point next to the control
  that changes it;
* nothing tells the cloud *which* points are editable, with which type and limits.

Cumulocity has a generic answer — **device parameters**: Digital Twin Manager (DTM) property
definitions declare a parameter set as a JSON schema, the device reports the set's current
values as a fragment on its managed object (kept fresh through `c8y_ParameterUpdate` events),
and edits in the device's *Parameters* tab arrive as one `c8y_ParameterUpdate` operation per set.
This RFC decides how tedge-dot plugs into that, and settles the four questions the feature
raises.

## Decision in one paragraph

Keep the connector a dumb, cloud-agnostic driver and bridge parameters in flows. The
configuration already says which points are parameters (`access = "read_write"` /
`"write"`, optionally refined by `meta.parameter`); the runtime echoes each point's `access`
in its samples, so a single flow, `ot-parameter-state`, can publish the current values of
every parameter as one retained twin fragment per *parameter set*
(`te/device/<device>///twin/<set>`, keyed by point id) — that fragment is what a cloud UI
displays and edits. The SDK runtime gains one protocol-neutral verb, **`write-batch`**, built on
the module's `write`, so an edit of several parameters is one command with one result; the
existing command flows carry it (`ot-command-forward` reshapes an `ot_parameter_update`
command into a `write-batch`, `ot-command-result` completes it). The Cumulocity glue is one
generic operation template (`c8y_ParameterUpdate` → `ot_parameter_update`). The cloud-side
declaration of the sets (Digital Twin Manager property definitions) is a tenant admin's
one-off act; `tedge-dot describe` prints it from the same TOML so the config remains the single
source of truth, but the device never talks to the DTM service. Writes travel as thin-edge
commands over MQTT, never through the `tedge-dot write` CLI.

## The four questions

### 1. Where does the DTM definition come from?

From the connector configuration, rendered on demand for a tenant admin to register once:

```sh
tedge-dot describe -c /etc/tedge/plugins/ot/modbus.toml --compact > modbus_parameters.json
C8Y_SETTINGS_CI=true c8y api POST /service/dtm/definitions/properties --data @modbus_parameters.json
```

The device does not push the definition: device users do not hold the DTM roles, and
declaring what a fleet may edit is a tenant decision anyway. The Parameters tab only renders
fragments that match a definition, so without this step the values are still visible in the
device's inventory but not editable — an acceptable degraded mode.

A *parameter* is every point whose `access` permits writes, plus points that opt in with
`meta.parameter` (read-only points then render `readOnly: true`, so the tab can *display*
state next to the controls); `meta.parameter = false` opts a writable point out. Parameters are
grouped into *sets* by `meta.parameter.set` (default `<protocol>_parameters`), one set = one
DTM identifier = one twin fragment. Datatype → JSON-schema type and integer bounds are
automatic; `meta.parameter.{title, description, min, max, enum, default, order}` enrich the
schema. The keys of a set are the point ids themselves, so parameter ids must be plain
identifiers (`[A-Za-z0-9_]`; Cumulocity rejects dots in keys) — `describe` refuses others.

Three places could own the definition; the config wins:

| Source of the definition | Verdict |
| --- | --- |
| **Connector TOML** (this RFC) | Already the single source of truth for points; offline, versionable, deterministic; the same `meta` table that drives flows drives the schema. |
| Cloud Fieldbus device type (RFC 0002) | Only Modbus, and the import already lands in the TOML — so it feeds this path for free rather than replacing it. A device type could later gain a "parameter" flag that the import turns into `meta.parameter`. |
| Hand-written in the DTM UI | Works (the flows only need the identifier to match), but drifts from the config. Kept as the escape hatch. |

### 2. Should the tedge-dot service react to the commands?

To **connector commands** (`te/device/<d>/ot/<protocol>/cmd/<verb>/<id>`): yes, it already
does, and this RFC adds `write-batch` there. To **thin-edge/cloud commands**
(`te/device/<d>///cmd/ot_parameter_update/<id>`, `c8y_ParameterUpdate`): no. The connector
stays ignorant of thin-edge command types and cloud fragments; the bridge is flows, hot-reloaded
and protocol-neutral, exactly like measurements and the existing `ot_write` bridge. Nor does
the runtime publish the twin itself: that would be the driver's first thin-edge-model output
and every runtime (Rust SDK, the C proof of concept) would have to replicate the set logic.
What *does* belong in the runtime is what every requester would otherwise re-implement:
writing N points as one ordered request with one result (`write-batch`), and echoing the
point's `access` in samples so a flow can recognise parameters without the TOML file.

### 3. How is the current value of a writable parameter reported, and what about output-only points?

`ot-parameter-state` publishes `te/device/<d>///twin/<set>` on change; the c8y mapper mirrors
it into the managed object, which is exactly what the Parameters tab reads (the
`c8y_ParameterUpdate` *event* path of the device-parameter service is not needed for that and
is left out). Values come from:

* **readable parameters** — every good sample, so the twin follows the device even when a
  value is changed locally on the HMI;
* **read/write parameters** — additionally updated optimistically from an acknowledged write,
  then confirmed by the next sample;
* **write-only parameters** (`access = "write"`) — the last *acknowledged* write. They never
  produce samples, so their set is learned from nothing: they land in the default set.

Output-only points exist in every protocol (Modbus write-only registers behind FC06/16 on
devices that reject reads of them, OPC UA nodes with `AccessLevel = CurrentWrite`, CANopen
write-only objects). For those the honest answer is: the device cannot tell you the current
state; you only know what you last commanded and that the protocol acknowledged it. The bridge
reports exactly that, the DTM description says so ("write-only: shows the last value written"),
and the value is *absent* (not `null`) until the first successful write after the mapper
started, so the UI never shows an invented state. Persisting the last commanded value across
mapper restarts is a follow-up (a retained `state/<point>` topic owned by the runtime, or the
flow re-reading its own twin, which today's loop protection forbids).

### 4. CLI (`tedge-dot write`) or MQTT commands?

MQTT commands, always, while the service runs. `tedge-dot write` opens a *second* protocol
session to the device: impossible on Modbus RTU (the serial port is exclusive), refused or
disruptive on PLCs that cap Modbus TCP connections or OPC UA sessions, and invisible to the
retained command state machine that flows and the cloud complete against. The CLI stays what
it is — a commissioning and debugging tool for a stopped service. The parameter plugin's
"script per parameter set" model would have pushed writes through the CLI (see below), which
is the main reason not to build on it for point writes.

## tedge-parameter-plugin: assessed, not used for point writes

[tedge-parameter-plugin](https://github.com/thin-edge/tedge-parameter-plugin) maps
`c8y_ParameterUpdate` onto a `parameter_update` workflow whose `prepare` step picks the set
name out of the operation and whose `run` step executes
`/usr/share/tedge/parameter-plugins/<set> set <json>`. Current values are whatever a script
publishes to `te/device/main///twin/<set>` (a `tedge-inventory` script at boot).

| | tedge-parameter-plugin | this RFC |
| --- | --- | --- |
| Where it runs | main device, tedge-agent workflow, shell script per set | any registered OT child device, mapper flows |
| How a write reaches the device | the script decides — `tedge-dot write` (second session) or hand-rolled `tedge mqtt pub` + wait | one `write-batch` command on the connector's existing session |
| Current values | script publishes the twin when it feels like it | derived continuously from samples + acknowledged writes, per set |
| Definition | hand-written in DTM | rendered from the config (`describe`), same `meta` as the flows |
| Dependencies | jq, tedge-inventory, sudo/tedge-write | none beyond the mapper |

The two are complementary and coexist on one gateway: templates are bound per device by the
capability a device advertises, so the plugin keeps handling `parameter_update` on the **main
device** (system settings, and — a good fit — connector settings such as `poll_interval`
through the `set-config` verb) while `c8y_ParameterUpdate` on **OT child devices** maps to
`ot_parameter_update`. Should the plugin later grow a "publish an MQTT command and wait" set
type, it could front the same flows; nothing here prevents that.

## Contract additions (additive, SDK-provided)

* `access` in the sample envelope (contract §5): the point's declared access, echoed like
  `meta`. No new topic, no conformance change.
* `write-batch` verb (contract §6.4, `command.schema.json`): ordered, fail-fast, per-point
  `results`; advertised for every module with `write` (the conformance harness treats it as
  implied by `write`). Extra request fields (`origin`, `c8y-mapper`) are ignored by the
  connector and left on the retained `init` for the flows to correlate with.
* `meta.parameter` — the first documented `meta` convention beyond the measurement hints;
  still uninterpreted by the connector (contract §5.2).

## Prototype (this branch)

| Layer | What | Where |
| --- | --- | --- |
| SDK | parameter/set derivation from the config + DTM rendering | `crates/sdk/src/descriptor.rs` |
| SDK runtime | `access` in samples, `write-batch` | `crates/sdk/src/runtime.rs` |
| CLI | `tedge-dot describe [--set] [--device] [--compact]` | `src/main.rs` |
| Flows | `ot-parameter-state` (new); `ot-command-forward` reshapes `ot_parameter_update`, `ot-command-result` honours `origin.command`; `ot-registration` advertises `ot_parameter_update` | `flows/` |
| c8y glue | `c8y_ParameterUpdate` template | `operations/` |
| Tests | offline flow checks incl. the chain through shared mapper state (`just test-flows`); e2e: `access` in samples, batch semantics, and the flows-driven parameter round-trip on a cloud-free flows runner (`just test-e2e modbus|opcua`); cloud: DTM registration → fragment → operation → measurement (`cloud/modbus/tests/parameters_c8y.robot`) | |

A simplification pass removed an earlier retained point-descriptor topic and two dedicated
parameter flows in favour of the `access` sample field and the existing command flows.

The C proof of concept ([poc-c/](../../poc-c/)) implements the same two runtime pieces
(`access` in samples, `write-batch` with the `executing` transition) in ~120 lines of C, so the
flows and the Cumulocity glue work unchanged with either binary; its smoke tests
(`poc-c/ci/smoke.sh`) assert both.

The e2e stacks gained a `flows` service: thin-edge from the `main` channel running this repo's
flows as a user-defined mapper (`tedge-mapper ot`) against the stack's broker, so the full
device-side chain — registration, command bridge, parameter bridge, twin — is exercised
without a cloud. It needs a thin-edge build with user-defined mappers (2.0.2+ dev), which is
also the reason the demo image (2.0.1) only runs the flows inside the c8y mapper.

## Sequence

```text
Parameters tab ─ c8y_ParameterUpdate ─▶ c8y mapper ─▶ te/device/plc1///cmd/ot_parameter_update/<id> {operation:{...}}
                                                            │ ot-command-forward (keys are point ids)
                                                            ▼
                                        te/device/plc1/ot/modbus/cmd/write-batch/ot--<id> {writes:[...], origin:{command:...}}
                                                            │ tedge-dot: write, write, ... (fail-fast)
                                                            ▼
                                        ... {status:successful, results:[...]}
                          ┌─────────────────────────────────┴──────────────────────────────┐
                          │ ot-command-result (origin.command)                             │ ot-parameter-state
                          ▼                                                                ▼
   te/device/plc1///cmd/ot_parameter_update/<id> {successful, c8y-mapper}   te/device/plc1///twin/modbus_parameters {...}
                          │ c8y mapper                                                     │ c8y mapper
                          ▼                                                                ▼
                 operation SUCCESSFUL                                            managed object fragment (UI refreshes)
```

## Open items

* Persist last-commanded values of write-only parameters across mapper restarts (see Q3).
* One DTM identifier is tenant-wide: heterogeneous fleets must name sets per device type
  (`meta.parameter.set`), which the Cloud Fieldbus import could derive from the device type
  name automatically.
* `write-batch` is sequential and non-atomic; a Modbus FC16 fast path for contiguous registers
  is a connector-local optimisation with the same result shape.
* The twin is republished on every value change of a readable parameter (retained, one
  inventory update each). Noisy parameters are better modelled as measurements than as
  parameters.
* Cumulocity's device-parameter service and DTM are opt-in microservices; the twin fragment is
  still published (and inventory-visible) on tenants without them.
* A `c8y_ParameterUpdate` *event* (Cumulocity's audit trail for parameter changes) could be
  added as a tiny flow on top of the twin later; it is not needed for the UI.
