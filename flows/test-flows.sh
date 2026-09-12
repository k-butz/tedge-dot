#!/usr/bin/env bash
# Validate every flow with `tedge flows test` (offline: no broker, no device, no cloud).
# Each case pipes a sample/measurement/command into a flow and checks the output topic+payload.
set -uo pipefail

cd "$(dirname "$0")"

pass=0
fail=0

# check <name> <flows-dir> <stdin> <expected-substring>
check() {
  local name="$1" dir="$2" input="$3" expect="$4"
  local out
  out="$(printf '%s\n' "$input" | tedge flows test --flows-dir "$dir" 2>/dev/null)"
  if [[ "$out" == *"$expect"* ]]; then
    echo "ok   - $name"
    pass=$((pass + 1))
  else
    echo "FAIL - $name"
    echo "       expected to contain: $expect"
    echo "       got:                 $out"
    fail=$((fail + 1))
  fi
}

# check_absent <name> <flows-dir> <stdin> <must-be-present> <must-be-absent>
# Asserts suppression: the output contains the first substring but NOT the second.
check_absent() {
  local name="$1" dir="$2" input="$3" present="$4" absent="$5"
  local out
  out="$(printf '%s\n' "$input" | tedge flows test --flows-dir "$dir" 2>/dev/null)"
  if [[ "$out" == *"$present"* && "$out" != *"$absent"* ]]; then
    echo "ok   - $name"
    pass=$((pass + 1))
  else
    echo "FAIL - $name"
    echo "       expected to contain: $present"
    echo "       and NOT contain:     $absent"
    echo "       got:                 $out"
    fail=$((fail + 1))
  fi
}

# check_empty <name> <flows-dir> <stdin>
check_empty() {
  local name="$1" dir="$2" input="$3"
  local out
  out="$(printf '%s\n' "$input" | tedge flows test --flows-dir "$dir" 2>/dev/null)"
  if [[ -z "$out" ]]; then
    echo "ok   - $name"
    pass=$((pass + 1))
  else
    echo "FAIL - $name (expected no output)"
    echo "       got: $out"
    fail=$((fail + 1))
  fi
}

# Build a temporary copy of a flow with overridden params so non-default config can be tested.
# Starts from the flow's params.toml.template (so every referenced param stays defined) and
# replaces the given "key = value" override lines. Echoes the temp dir; caller must `rm -rf`.
flow_with_params() {
  local src="$1" overrides="$2" tmp key
  tmp="$(mktemp -d)"
  cp "$src"/*.js "$src"/flow.toml "$tmp"/
  cp "$src/params.toml.template" "$tmp/params.toml"
  while IFS= read -r line; do
    [[ -z "$line" ]] && continue
    key="${line%%=*}"
    key="$(printf '%s' "$key" | tr -d '[:space:]')"
    sed -i.bak "/^[[:space:]]*${key}[[:space:]]*=/d" "$tmp/params.toml" && rm -f "$tmp/params.toml.bak"
    printf '%s\n' "$line" >> "$tmp/params.toml"
  done <<< "$overrides"
  printf '%s' "$tmp"
}

# check_params <name> <flow-src> <params> <stdin> <expected-substring> [extra tedge flags...]
check_params() {
  local name="$1" src="$2" params="$3" input="$4" expect="$5"
  shift 5
  local tmp out
  tmp="$(flow_with_params "$src" "$params")"
  out="$(printf '%s\n' "$input" | tedge flows test --flows-dir "$tmp" "$@" 2>/dev/null)"
  rm -rf "$tmp"
  if [[ "$out" == *"$expect"* ]]; then
    echo "ok   - $name"
    pass=$((pass + 1))
  else
    echo "FAIL - $name"
    echo "       expected to contain: $expect"
    echo "       got:                 $out"
    fail=$((fail + 1))
  fi
}

# check_multi <name> <flows (space-separated)> <stdin> <expected-substring> [--absent <substring>]
# Runs several flows together in one mapper-like flows dir (each with its template params), so
# cross-flow state through context.mapper is exercised the way a deployed mapper runs them.
check_multi() {
  local name="$1" flows="$2" input="$3" expect="$4" absent="${6:-}"
  local tmp out f
  tmp="$(mktemp -d)"
  for f in $flows; do
    mkdir -p "$tmp/$f"
    cp "$f"/*.js "$f"/flow.toml "$tmp/$f/"
    cp "$f/params.toml.template" "$tmp/$f/params.toml"
  done
  out="$(printf '%s\n' "$input" | tedge flows test --flows-dir "$tmp" 2>/dev/null)"
  rm -rf "$tmp"
  if [[ "$out" == *"$expect"* && ( -z "$absent" || "$out" != *"$absent"* ) ]]; then
    echo "ok   - $name"
    pass=$((pass + 1))
  else
    echo "FAIL - $name"
    echo "       expected to contain: $expect"
    [[ -n "$absent" ]] && echo "       and NOT contain:     $absent"
    echo "       got:                 $out"
    fail=$((fail + 1))
  fi
}

S='{"ts":"2026-05-30T10:00:00.000Z","device":"plc1","protocol":"modbus","point":"level_f32","mode":"typed","datatype":"float32","value":404.17,"value_repr":"number","raw":"43ca 15c3","quality":"good","addr":{}}'
SBAD='{"ts":"2026-05-30T10:00:00.000Z","device":"plc1","protocol":"modbus","point":"level_f32","mode":"typed","datatype":"float32","quality":"bad","error":"timeout","addr":{}}'
SBOOL='{"ts":"2026-05-30T10:00:00.000Z","device":"plc1","protocol":"modbus","point":"coil_rw","mode":"typed","datatype":"bool","value":true,"value_repr":"boolean","raw":"01","quality":"good","addr":{}}'
# Same contract envelope from a different protocol: the group is derived from sample.protocol.
SOPCUA='{"ts":"2026-05-30T10:00:00.000Z","device":"opc1","protocol":"opcua","point":"temperature","mode":"typed","datatype":"float32","value":21.5,"value_repr":"number","raw":"41ac0000","quality":"good","addr":{"node_id":"ns=2;s=Temperature"}}'

# --- ot-measurement (OT sample -> thin-edge measurement) ---
check "measurement: modbus float -> m/modbus" ot-measurement \
  "[te/device/plc1/ot/modbus/sample/level_f32] $S" \
  '[te/device/plc1///m/modbus] {"modbus":{"level_f32":404.17},"time":"2026-05-30T10:00:00.000Z"}'
check "measurement: opcua float -> m/opcua (generic)" ot-measurement \
  "[te/device/opc1/ot/opcua/sample/temperature] $SOPCUA" \
  '[te/device/opc1///m/opcua] {"opcua":{"temperature":21.5},"time":"2026-05-30T10:00:00.000Z"}'
check "measurement: bool coil -> 1" ot-measurement \
  "[te/device/plc1/ot/modbus/sample/coil_rw] $SBOOL" \
  '{"modbus":{"coil_rw":1}'
check_empty "measurement: bad quality dropped" ot-measurement \
  "[te/device/plc1/ot/modbus/sample/level_f32] $SBAD"

# --- ot-measurement extended config (on_change / point_separator / combine) ---
# Scaling is applied by the connector (per-point transform), so the sample already carries the
# final value; the flow passes it through unchanged.
SINT='{"ts":"2026-05-30T10:00:00.000Z","device":"plc1","protocol":"modbus","point":"temp_u16","mode":"typed","datatype":"uint16","value":17001,"value_repr":"number","raw":"4269","quality":"good","addr":{}}'

check_params "measurement: passes connector-scaled value through" ot-measurement \
  'include_boolean = "true"' \
  "[te/device/plc1/ot/modbus/sample/temp_u16] $SINT" \
  '[te/device/plc1///m/modbus] {"modbus":{"temp_u16":17001},"time":"2026-05-30T10:00:00.000Z"}'

# point_separator: a dotted point id remaps the signal to group/series without per-point config.
SDOTTED='{"ts":"2026-05-30T10:00:00.000Z","device":"plc1","protocol":"modbus","point":"Environment.Temperature","mode":"typed","datatype":"uint16","value":17001,"value_repr":"number","raw":"4269","quality":"good","addr":{}}'
check_params "measurement: point_separator remaps signal to group.series" ot-measurement \
  'point_separator = "."' \
  "[te/device/plc1/ot/modbus/sample/Environment.Temperature] $SDOTTED" \
  '[te/device/plc1///m/Environment] {"Environment":{"Temperature":17001},"time":"2026-05-30T10:00:00.000Z"}'

# on_change: same value twice -> only one emission (the first); assert the second is suppressed.
check_params "measurement: on_change suppresses unchanged" ot-measurement \
  'on_change = "true"' \
  "$(printf '[te/device/plc1/ot/modbus/sample/temp_u16] %s\n[te/device/plc1/ot/modbus/sample/temp_u16] %s' "$SINT" "$SINT")" \
  '"temp_u16":17001'

# --- ot-measurement per-signal meta (sample.meta overrides the flow params per point) ---
# The connector runtime echoes the point's `meta` table in every sample envelope; the flow
# applies it without any per-signal flow configuration.

# meta.on_change: identical value twice -> second suppressed (flow-wide on_change stays off).
MC1='{"ts":"2026-05-30T10:00:00.000Z","device":"plc1","protocol":"modbus","point":"m1","mode":"typed","datatype":"uint16","value":42,"value_repr":"number","raw":"002a","quality":"good","addr":{},"meta":{"on_change":true}}'
MC2='{"ts":"2026-05-30T10:00:07.000Z","device":"plc1","protocol":"modbus","point":"m1","mode":"typed","datatype":"uint16","value":42,"value_repr":"number","raw":"002a","quality":"good","addr":{},"meta":{"on_change":true}}'
check_absent "measurement: meta.on_change suppresses repeat" ot-measurement \
  "$(printf '[te/device/plc1/ot/modbus/sample/m1] %s\n[te/device/plc1/ot/modbus/sample/m1] %s' "$MC1" "$MC2")" \
  '"time":"2026-05-30T10:00:00.000Z"' '"time":"2026-05-30T10:00:07.000Z"'

# meta.deadband: change below the deadband suppressed, change above it emitted.
DB1='{"ts":"2026-05-30T10:00:00.000Z","device":"plc1","protocol":"modbus","point":"d1","mode":"typed","datatype":"float32","value":100.0,"value_repr":"number","raw":"42c80000","quality":"good","addr":{},"meta":{"deadband":0.5}}'
DB2='{"ts":"2026-05-30T10:00:01.000Z","device":"plc1","protocol":"modbus","point":"d1","mode":"typed","datatype":"float32","value":100.4,"value_repr":"number","raw":"42c8cccd","quality":"good","addr":{},"meta":{"deadband":0.5}}'
DB3='{"ts":"2026-05-30T10:00:02.000Z","device":"plc1","protocol":"modbus","point":"d1","mode":"typed","datatype":"float32","value":100.6,"value_repr":"number","raw":"42c93333","quality":"good","addr":{},"meta":{"deadband":0.5}}'
check_absent "measurement: meta.deadband suppresses sub-threshold change" ot-measurement \
  "$(printf '[te/device/plc1/ot/modbus/sample/d1] %s\n[te/device/plc1/ot/modbus/sample/d1] %s\n[te/device/plc1/ot/modbus/sample/d1] %s' "$DB1" "$DB2" "$DB3")" \
  '"time":"2026-05-30T10:00:02.000Z"' '"time":"2026-05-30T10:00:01.000Z"'

# meta.min_interval: reading 5s after the last emit dropped, reading 15s after emitted.
RL1='{"ts":"2026-05-30T10:00:00.000Z","device":"plc1","protocol":"modbus","point":"r1","mode":"typed","datatype":"uint16","value":1,"value_repr":"number","raw":"0001","quality":"good","addr":{},"meta":{"min_interval":"10s"}}'
RL2='{"ts":"2026-05-30T10:00:05.000Z","device":"plc1","protocol":"modbus","point":"r1","mode":"typed","datatype":"uint16","value":2,"value_repr":"number","raw":"0002","quality":"good","addr":{},"meta":{"min_interval":"10s"}}'
RL3='{"ts":"2026-05-30T10:00:15.000Z","device":"plc1","protocol":"modbus","point":"r1","mode":"typed","datatype":"uint16","value":3,"value_repr":"number","raw":"0003","quality":"good","addr":{},"meta":{"min_interval":"10s"}}'
check_absent "measurement: meta.min_interval rate-limits" ot-measurement \
  "$(printf '[te/device/plc1/ot/modbus/sample/r1] %s\n[te/device/plc1/ot/modbus/sample/r1] %s\n[te/device/plc1/ot/modbus/sample/r1] %s' "$RL1" "$RL2" "$RL3")" \
  '"time":"2026-05-30T10:00:15.000Z"' '"time":"2026-05-30T10:00:05.000Z"'

# meta.debounce: a new value only passes once it has stayed stable for the period; the first
# observation is the candidate (no emit), the confirmation 3s later is emitted.
DE1='{"ts":"2026-05-30T10:00:00.000Z","device":"plc1","protocol":"modbus","point":"b1","mode":"typed","datatype":"uint16","value":7,"value_repr":"number","raw":"0007","quality":"good","addr":{},"meta":{"debounce":"2s"}}'
DE2='{"ts":"2026-05-30T10:00:03.000Z","device":"plc1","protocol":"modbus","point":"b1","mode":"typed","datatype":"uint16","value":7,"value_repr":"number","raw":"0007","quality":"good","addr":{},"meta":{"debounce":"2s"}}'
DE3='{"ts":"2026-05-30T10:00:04.000Z","device":"plc1","protocol":"modbus","point":"b1","mode":"typed","datatype":"uint16","value":9,"value_repr":"number","raw":"0009","quality":"good","addr":{},"meta":{"debounce":"2s"}}'
check_absent "measurement: meta.debounce waits for stability" ot-measurement \
  "$(printf '[te/device/plc1/ot/modbus/sample/b1] %s\n[te/device/plc1/ot/modbus/sample/b1] %s\n[te/device/plc1/ot/modbus/sample/b1] %s' "$DE1" "$DE2" "$DE3")" \
  '"time":"2026-05-30T10:00:03.000Z"' '"time":"2026-05-30T10:00:04.000Z"'

# meta.measurement: per-signal group/series naming echoed from the connector point config
# (e.g. written by the Cloud Fieldbus import shim from a device type's measurementMapping).
MMEAS='{"ts":"2026-05-30T10:00:00.000Z","device":"plc1","protocol":"modbus","point":"temperature","mode":"typed","datatype":"uint16","value":17.001,"value_repr":"number","raw":"4269","quality":"good","addr":{},"meta":{"measurement":{"group":"Environment","series":"Temperature"}}}'
check "measurement: meta.measurement names group/series" ot-measurement \
  "[te/device/plc1/ot/modbus/sample/temperature] $MMEAS" \
  '[te/device/plc1///m/Environment] {"Environment":{"Temperature":17.001},"time":"2026-05-30T10:00:00.000Z"}'

# meta.measurement wins over the point_separator convention (per-signal beats flow-wide).
MMDOT='{"ts":"2026-05-30T10:00:00.000Z","device":"plc1","protocol":"modbus","point":"Foo.Bar","mode":"typed","datatype":"uint16","value":1,"value_repr":"number","raw":"0001","quality":"good","addr":{},"meta":{"measurement":{"group":"Environment","series":"Temperature"}}}'
mmtmp="$(flow_with_params ot-measurement 'point_separator = "."')"
mmout="$(printf '%s\n' "[te/device/plc1/ot/modbus/sample/Foo.Bar] $MMDOT" | tedge flows test --flows-dir "$mmtmp" 2>/dev/null)"
rm -rf "$mmtmp"
if [[ "$mmout" == *'{"Environment":{"Temperature":1}'* && "$mmout" != *'"Foo"'* ]]; then
  echo "ok   - measurement: meta.measurement wins over point_separator"
  pass=$((pass + 1))
else
  echo "FAIL - measurement: meta.measurement wins over point_separator"
  echo "       got: $mmout"
  fail=$((fail + 1))
fi

# combine: two series of one device merged into a single measurement, flushed on interval.
SLVL='{"ts":"2026-05-30T10:00:00.000Z","device":"plc1","protocol":"modbus","point":"level_f32","mode":"typed","datatype":"float32","value":404.17,"value_repr":"number","raw":"43ca15c3","quality":"good","addr":{}}'
check_params "measurement: combine merges series on interval" ot-measurement \
  "$(printf 'combine = "true"\ncombine_interval = "1s"')" \
  "$(printf '[te/device/plc1/ot/modbus/sample/temp_u16] %s\n[te/device/plc1/ot/modbus/sample/level_f32] %s' "$SINT" "$SLVL")" \
  '[te/device/plc1///m/modbus] {"modbus":{"level_f32":404.17,"temp_u16":17001}' \
  --final-on-interval

# --- ot-event (measurement -> event on value change) ---
check "event: emits on first value" ot-event \
  '[te/device/plc1///m/modbus] {"modbus":{"value":5},"time":"2026-05-30T10:00:00.000Z"}' \
  '[te/device/plc1///e/ot_event] {"text":"OT value changed","time":"2026-05-30T10:00:00.000Z"}'
check "event: opcua measurement raises (generic)" ot-event \
  '[te/device/opc1///m/opcua] {"opcua":{"value":9},"time":"2026-05-30T10:00:00.000Z"}' \
  '[te/device/opc1///e/ot_event]'
# Same value twice -> a single event (the second is suppressed as unchanged).
check "event: change-detection fires once for repeats" ot-event \
  "$(printf '[te/device/plc1///m/modbus] {"modbus":{"value":5},"time":"t1"}\n[te/device/plc1///m/modbus] {"modbus":{"value":5},"time":"t2"}')" \
  '"time":"t1"'

# --- ot-alarm (measurement -> alarm, hysteresis; group taken from topic) ---
check "alarm: modbus measurement raises" ot-alarm \
  '[te/device/plc1///m/modbus] {"modbus":{"temp_u16":80},"time":"2026-05-30T10:00:00.000Z"}' \
  '[te/device/plc1///a/ot_overrange] {"severity":"major"'
check "alarm: opcua measurement raises (generic)" ot-alarm \
  '[te/device/opc1///m/opcua] {"opcua":{"temp_u16":80},"time":"2026-05-30T10:00:00.000Z"}' \
  '[te/device/opc1///a/ot_overrange] {"severity":"major"'
check_empty "alarm: below threshold, never raised" ot-alarm \
  '[te/device/plc1///m/modbus] {"modbus":{"temp_u16":60},"time":"2026-05-30T10:00:00.000Z"}'

# --- ot-registration (link -> child-device registration; type from the connector, else protocol) ---
check "registration: declared device type becomes the entity type" ot-registration \
  '[te/device/plc1/ot/modbus/status/link] {"status":"connected","type":"acme-boiler-v2"}' \
  '[te/device/plc1//] {"@type":"child-device","name":"plc1","type":"acme-boiler-v2","ot-protocol":"modbus"}'
check "registration: modbus link -> modbus-device" ot-registration \
  '[te/device/plc1/ot/modbus/status/link] {"status":"connected"}' \
  '[te/device/plc1//] {"@type":"child-device","name":"plc1","type":"modbus-device","ot-protocol":"modbus"}'
check "registration: opcua link -> opcua-device (generic)" ot-registration \
  '[te/device/opc1/ot/opcua/status/link] {"status":"connected"}' \
  '[te/device/opc1//] {"@type":"child-device","name":"opc1","type":"opcua-device","ot-protocol":"opcua"}'
check_empty "registration: disconnected ignored" ot-registration \
  '[te/device/plc1/ot/modbus/status/link] {"status":"disconnected"}'
check_params "registration: publishes twin fragment from info" ot-registration \
  'twin_fragment = "c8y_ModbusDevice"' \
  '[te/device/plc1/ot/modbus/status/link] {"status":"connected","info":{"protocol":"modbus","transport":"tcp","host":"127.0.0.1","port":502,"unit_id":1}}' \
  '[te/device/plc1///twin/c8y_ModbusDevice] {"protocol":"modbus","transport":"tcp","host":"127.0.0.1","port":502,"unit_id":1}'

# --- ot-parameter-state (samples + write results -> twin parameter sets) ---
# Samples echo the point's access; writable points (or meta.parameter opt-ins) are parameters.
ST='{"ts":"2026-05-30T10:00:00.000Z","device":"plc1","protocol":"modbus","point":"temp_u16","mode":"typed","datatype":"uint16","value":17001,"value_repr":"number","raw":"4269","quality":"good","addr":{},"access":"read_write"}'
SL='{"ts":"2026-05-30T10:00:00.000Z","device":"plc1","protocol":"modbus","point":"level_f32","mode":"typed","datatype":"float32","value":1.5,"value_repr":"number","raw":"3fc0 0000","quality":"good","addr":{},"access":"read"}'
SW='{"ts":"2026-05-30T10:00:00.000Z","device":"plc1","protocol":"modbus","point":"status_word","mode":"typed","datatype":"uint16","value":7,"value_repr":"number","raw":"0007","quality":"good","addr":{},"access":"read","meta":{"parameter":true}}'
SP='{"ts":"2026-05-30T10:00:00.000Z","device":"plc1","protocol":"modbus","point":"pump_speed","mode":"typed","datatype":"float32","value":10.5,"value_repr":"number","raw":"4128 0000","quality":"good","addr":{},"access":"read_write","meta":{"parameter":"pump"}}'
SH='{"ts":"2026-05-30T10:00:00.000Z","device":"plc1","protocol":"modbus","point":"hidden_rw","mode":"typed","datatype":"uint16","value":1,"value_repr":"number","raw":"0001","quality":"good","addr":{},"access":"read_write","meta":{"parameter":false}}'
STBAD='{"ts":"2026-05-30T10:00:00.000Z","device":"plc1","protocol":"modbus","point":"temp_u16","mode":"typed","datatype":"uint16","quality":"bad","error":"timeout","addr":{},"access":"read_write"}'
check "parameter-state: writable point sample -> twin set keyed by point id" ot-parameter-state \
  "[te/device/plc1/ot/modbus/sample/temp_u16] $ST" \
  '[te/device/plc1///twin/modbus_control_parameters] {"temp_u16":17001}'
check_empty "parameter-state: read-only point ignored" ot-parameter-state \
  "[te/device/plc1/ot/modbus/sample/level_f32] $SL"
check_empty "parameter-state: bad-quality sample ignored" ot-parameter-state \
  "[te/device/plc1/ot/modbus/sample/temp_u16] $STBAD"
check_absent "parameter-state: unchanged value republishes nothing (single twin)" ot-parameter-state \
  "[te/device/plc1/ot/modbus/sample/temp_u16] $ST"$'\n'"[te/device/plc1/ot/modbus/sample/temp_u16] $ST" \
  '{"temp_u16":17001}' \
  '{"temp_u16":17001}
[te/device/plc1///twin/modbus_control_parameters] {"temp_u16":17001}'
check "parameter-state: meta.parameter names another set" ot-parameter-state \
  "[te/device/plc1/ot/modbus/sample/pump_speed] $SP" \
  '[te/device/plc1///twin/pump] {"pump_speed":10.5}'
check "parameter-state: opted-in read-only point is displayed" ot-parameter-state \
  "[te/device/plc1/ot/modbus/sample/status_word] $SW" \
  '[te/device/plc1///twin/modbus_control_parameters] {"status_word":7}'
check_empty "parameter-state: meta.parameter=false opts a writable point out" ot-parameter-state \
  "[te/device/plc1/ot/modbus/sample/hidden_rw] $SH"
check "parameter-state: opted-out point stays out after a write" ot-parameter-state \
  "[te/device/plc1/ot/modbus/sample/hidden_rw] $SH"$'\n'"[te/device/plc1/ot/modbus/sample/temp_u16] $ST"$'\n'"[te/device/plc1/ot/modbus/cmd/write/w1] {\"status\":\"successful\",\"point\":\"hidden_rw\",\"value\":2}" \
  '[te/device/plc1///twin/modbus_control_parameters] {"temp_u16":17001}'
check "parameter-state: write-only point takes the last acknowledged batch write" ot-parameter-state \
  '[te/device/plc1/ot/modbus/cmd/write-batch/ot--1] {"status":"successful","results":[{"point":"valve_cmd","status":"successful","value":true}]}' \
  '[te/device/plc1///twin/modbus_control_parameters] {"valve_cmd":true}'
check "parameter-state: single write result updates a read/write point optimistically" ot-parameter-state \
  "[te/device/plc1/ot/modbus/sample/temp_u16] $ST"$'\n'"[te/device/plc1/ot/modbus/cmd/write/abc] {\"status\":\"successful\",\"point\":\"temp_u16\",\"value\":4242}" \
  '[te/device/plc1///twin/modbus_control_parameters] {"temp_u16":4242}'
check "parameter-state: written point keeps the set learned from its samples" ot-parameter-state \
  "[te/device/plc1/ot/modbus/sample/pump_speed] $SP"$'\n'"[te/device/plc1/ot/modbus/cmd/write/abc] {\"status\":\"successful\",\"point\":\"pump_speed\",\"value\":12}" \
  '[te/device/plc1///twin/pump] {"pump_speed":12}'
check_empty "parameter-state: failed write leaves the twin alone" ot-parameter-state \
  '[te/device/plc1/ot/modbus/cmd/write/abc] {"status":"failed","point":"temp_u16","reason":"boom"}'
check_params "parameter-state: default_set param renames the default set" ot-parameter-state \
  'default_set = "plc_settings"' \
  "[te/device/plc1/ot/modbus/sample/temp_u16] $ST" \
  '[te/device/plc1///twin/plc_settings] {"temp_u16":17001}'
check "parameter-state: opcua samples -> opcua_control_parameters (generic)" ot-parameter-state \
  '[te/device/opc1/ot/opcua/sample/setpoint] {"device":"opc1","protocol":"opcua","point":"setpoint","mode":"typed","datatype":"int32","value":42,"value_repr":"number","raw":"0000 002a","quality":"good","addr":{},"access":"read_write"}' \
  '[te/device/opc1///twin/opcua_control_parameters] {"setpoint":42}'
# A DTM identifier is tenant-wide, so the set is named after the *device type* when the
# connector reports one — two modbus device types must not share "modbus_control_parameters".
# The name must match what `tedge-dot describe` renders from the same configuration.
STYPED='{"ts":"2026-05-30T10:00:00.000Z","device":"plc1","type":"acme-boiler-v2","protocol":"modbus","point":"temp_u16","mode":"typed","datatype":"uint16","value":17001,"value_repr":"number","raw":"4269","quality":"good","addr":{},"access":"read_write"}'
check "parameter-state: device type qualifies the set name" ot-parameter-state \
  "[te/device/plc1/ot/modbus/sample/temp_u16] $STYPED" \
  '[te/device/plc1///twin/acme_boiler_v2_control_parameters] {"temp_u16":17001}'
SGROUP='{"ts":"2026-05-30T10:00:00.000Z","device":"plc1","type":"acme-boiler-v2","protocol":"modbus","point":"commission_code","mode":"typed","datatype":"uint16","value":3,"value_repr":"number","raw":"0003","quality":"good","addr":{},"access":"read_write","meta":{"parameter":{"group":"commissioning"}}}'
check "parameter-state: meta.parameter.group names a second set of the same type" ot-parameter-state \
  "[te/device/plc1/ot/modbus/sample/commission_code] $SGROUP" \
  '[te/device/plc1///twin/acme_boiler_v2_commissioning_parameters] {"commission_code":3}'
# A write-only point never samples, so the retained link status is the only place its device
# type can come from — otherwise its set would fall back to the protocol name.
check "parameter-state: link status supplies the type for write-only points" ot-parameter-state \
  '[te/device/plc1/ot/modbus/status/link] {"status":"connected","type":"acme-boiler-v2"}'$'\n''[te/device/plc1/ot/modbus/cmd/write-batch/ot--1] {"status":"successful","results":[{"point":"valve_cmd","status":"successful","value":true}]}' \
  '[te/device/plc1///twin/acme_boiler_v2_control_parameters] {"valve_cmd":true}'
check_empty "parameter-state: link status alone publishes nothing" ot-parameter-state \
  '[te/device/plc1/ot/modbus/status/link] {"status":"connected","type":"acme-boiler-v2"}'

# --- ot-command-forward: parameter_update -> write-batch ---
C8YOP='{"status":"init","operation":{"deviceId":"123","c8y_ParameterUpdate":{},"c8y_ParameterUpdate_acme_boiler_v2_control_parameters":{},"acme_boiler_v2_control_parameters":{"temp_u16":4242,"coil_rw":true}},"c8y-mapper":{"on_fragment":"c8y_ParameterUpdate","output":null}}'
check "command-forward: c8y parameter update -> one write-batch with origin + mapper metadata" ot-command-forward \
  "[te/device/plc1///cmd/parameter_update/c8y-mapper-1] $C8YOP" \
  '[te/device/plc1/ot/modbus/cmd/write-batch/ot--c8y-mapper-1] {"status":"init","writes":[{"point":"temp_u16","value":4242},{"point":"coil_rw","value":true}],"origin":{"command":"parameter_update","set":"acme_boiler_v2_control_parameters","parameters":{"temp_u16":4242,"coil_rw":true}},"c8y-mapper":{"on_fragment":"c8y_ParameterUpdate","output":null}}'
check "command-forward: direct parameter update shape" ot-command-forward \
  '[te/device/plc1///cmd/parameter_update/x1] {"status":"init","set":"pump","parameters":{"pump_speed":12}}' \
  '[te/device/plc1/ot/modbus/cmd/write-batch/ot--x1] {"status":"init","writes":[{"point":"pump_speed","value":12}],"origin":{"command":"parameter_update","set":"pump","parameters":{"pump_speed":12}}}'
check_params "command-forward: protocol recorded by ot-parameter-state wins over params" ot-command-forward '' \
  '[te/device/opc1///cmd/parameter_update/x1] {"status":"init","set":"opcua_control_parameters","parameters":{"setpoint":7}}' \
  '[te/device/opc1/ot/opcua/cmd/write-batch/ot--x1]' \
  --context '{"ot-protocol:opc1":"opcua"}'
check "command-forward: unintelligible parameter update forwarded as an empty batch with the error noted" ot-command-forward \
  '[te/device/plc1///cmd/parameter_update/x2] {"status":"init","operation":{"c8y_ParameterUpdate":{}}}' \
  '"writes":[],"origin":{"command":"parameter_update","set":null,"parameters":null,"error":"c8y_ParameterUpdate operation names no parameter set"}'

# --- ot-command-result: origin.command routes reshaped commands back ---
BINIT='{"status":"init","writes":[{"point":"temp_u16","value":4242}],"origin":{"command":"parameter_update","set":"acme_boiler_v2_control_parameters","parameters":{"temp_u16":4242}},"c8y-mapper":{"on_fragment":"c8y_ParameterUpdate","output":null}}'
BRESULT="[te/device/plc1/ot/modbus/cmd/write-batch/ot--c8y-mapper-1] $BINIT"$'\n'"[te/device/plc1/ot/modbus/cmd/write-batch/ot--c8y-mapper-1] {\"status\":\"successful\",\"results\":[{\"point\":\"temp_u16\",\"status\":\"successful\",\"value\":4242}]}"
check "command-result: batch result completes the originating command type" ot-command-result "$BRESULT" \
  '[te/device/plc1///cmd/parameter_update/c8y-mapper-1] {'
check "command-result: batch result keeps the c8y-mapper metadata" ot-command-result "$BRESULT" \
  '"c8y-mapper":{"on_fragment":"c8y_ParameterUpdate","output":null}'
check "command-result: batch result carries status + per-point results" ot-command-result "$BRESULT" \
  '"status":"successful","results":[{"point":"temp_u16","status":"successful","value":4242}]}'
check_absent "command-result: batch request body (writes) is not echoed" ot-command-result "$BRESULT" \
  '"status":"successful"' '"writes"'
check "command-result: failed batch combines the origin note and the connector reason" ot-command-result \
  '[te/device/plc1/ot/modbus/cmd/write-batch/ot--x2] {"status":"init","writes":[],"origin":{"command":"parameter_update","error":"no parameter set"}}'$'\n''[te/device/plc1/ot/modbus/cmd/write-batch/ot--x2] {"status":"failed","reason":"write-batch request has no writes","results":[]}' \
  '[te/device/plc1///cmd/parameter_update/x2] {"origin":{"command":"parameter_update","error":"no parameter set"},"status":"failed","reason":"no parameter set; write-batch request has no writes","results":[]}'
check "command-result: batch without origin mirrors as ot_write_batch" ot-command-result \
  '[te/device/plc1/ot/modbus/cmd/write-batch/ot--b1] {"status":"successful","results":[]}' \
  '[te/device/plc1///cmd/ot_write_batch/b1] {"status":"successful","results":[]}'
check "registration: advertises the parameter_update capability" ot-registration \
  '[te/device/plc1/ot/modbus/status/link] {"status":"connected"}' \
  '[te/device/plc1///cmd/parameter_update] {}'

# --- the parameter bridge in one mapper: state records the protocol, forward uses it, result completes, state updates the twin ---
CHAIN="[te/device/opc1/ot/opcua/sample/setpoint] {\"device\":\"opc1\",\"protocol\":\"opcua\",\"point\":\"setpoint\",\"mode\":\"typed\",\"datatype\":\"int32\",\"value\":0,\"value_repr\":\"number\",\"raw\":\"0000 0000\",\"quality\":\"good\",\"addr\":{},\"access\":\"read_write\"}
[te/device/opc1///cmd/parameter_update/c8y-mapper-9] {\"status\":\"init\",\"operation\":{\"c8y_ParameterUpdate\":{},\"c8y_ParameterUpdate_opcua_parameters\":{},\"opcua_parameters\":{\"setpoint\":42}},\"c8y-mapper\":{\"on_fragment\":\"c8y_ParameterUpdate\",\"output\":null}}
[te/device/opc1/ot/opcua/cmd/write-batch/ot--c8y-mapper-9] {\"status\":\"init\",\"writes\":[{\"point\":\"setpoint\",\"value\":42}],\"origin\":{\"command\":\"parameter_update\",\"set\":\"opcua_parameters\",\"parameters\":{\"setpoint\":42}},\"c8y-mapper\":{\"on_fragment\":\"c8y_ParameterUpdate\",\"output\":null}}
[te/device/opc1/ot/opcua/cmd/write-batch/ot--c8y-mapper-9] {\"status\":\"successful\",\"results\":[{\"point\":\"setpoint\",\"status\":\"successful\",\"value\":42}]}"
check_multi "parameter bridge: forward targets the protocol the state flow recorded (no params needed)" \
  "ot-parameter-state ot-command-forward ot-command-result" "$CHAIN" \
  '[te/device/opc1/ot/opcua/cmd/write-batch/ot--c8y-mapper-9] {"status":"init","writes":[{"point":"setpoint","value":42}]'
check_multi "parameter bridge: result completes the cloud-bound command" \
  "ot-parameter-state ot-command-forward ot-command-result" "$CHAIN" \
  '[te/device/opc1///cmd/parameter_update/c8y-mapper-9] {'
check_multi "parameter bridge: completed command keeps the mapper metadata and result" \
  "ot-parameter-state ot-command-forward ot-command-result" "$CHAIN" \
  '"status":"successful","results":[{"point":"setpoint","status":"successful","value":42}]}'
check_multi "parameter bridge: acknowledged write updates the twin, no ot_write_batch echo" \
  "ot-parameter-state ot-command-forward ot-command-result" "$CHAIN" \
  '[te/device/opc1///twin/opcua_control_parameters] {"setpoint":42}' --absent 'ot_write_batch'

# --- ot-command-forward (thin-edge cmd -> connector write) ---
check "command-forward: init forwarded" ot-command-forward \
  '[te/device/plc1///cmd/ot_write/abc] {"status":"init","point":"coil_rw","value":true}' \
  '[te/device/plc1/ot/modbus/cmd/write/ot--abc] {"status":"init","point":"coil_rw","value":true}'
check_empty "command-forward: non-init ignored" ot-command-forward \
  '[te/device/plc1///cmd/ot_write/abc] {"status":"successful","point":"coil_rw"}'
check "command-forward: set-config init forwarded" ot-command-forward \
  '[te/device/main///cmd/ot_set_config/cfg1] {"status":"init","target":"connector","config":{"poll_interval":"5s"}}' \
  '[te/device/main/ot/modbus/cmd/set-config/ot--cfg1] {"status":"init","target":"connector","config":{"poll_interval":"5s"}}'
check "command-forward: define-device init forwarded" ot-command-forward \
  '[te/device/main///cmd/ot_define_device/d1] {"status":"init","device":{"name":"plc-9"}}' \
  '[te/device/main/ot/modbus/cmd/define-device/ot--d1] {"status":"init","device":{"name":"plc-9"}}'
check_empty "command-forward: non-ot command ignored" ot-command-forward \
  '[te/device/plc1///cmd/restart/abc] {"status":"init"}'

# --- ot-command-result (connector result -> thin-edge cmd) ---
check "command-result: successful mirrored" ot-command-result \
  '[te/device/plc1/ot/modbus/cmd/write/abc] {"status":"successful","point":"coil_rw","value":true}' \
  '[te/device/plc1///cmd/ot_write/abc]'
check "command-result: opcua result mirrored (generic)" ot-command-result \
  '[te/device/opc1/ot/opcua/cmd/write/xyz] {"status":"successful","point":"setpoint","value":42}' \
  '[te/device/opc1///cmd/ot_write/xyz]'
check "command-result: set-config result mirrored" ot-command-result \
  '[te/device/main/ot/modbus/cmd/set-config/cfg1] {"status":"successful"}' \
  '[te/device/main///cmd/ot_set_config/cfg1]'
check_empty "command-result: init not mirrored (no loop)" ot-command-result \
  '[te/device/plc1/ot/modbus/cmd/write/ot--abc] {"status":"init","point":"coil_rw","value":true}'
check "command-result: c8y-mapper metadata preserved in result" ot-command-result \
  $'[te/device/plc1/ot/modbus/cmd/write/ot--abc] {"status":"init","point":"coil_rw","value":true,"c8y-mapper":{"on_fragment":"c8y_SetCoil","output":null}}\n[te/device/plc1/ot/modbus/cmd/write/ot--abc] {"status":"successful","point":"coil_rw","value":true}' \
  '"c8y-mapper":{"on_fragment":"c8y_SetCoil","output":null}'

echo
echo "flows: $pass passed, $fail failed"
[[ "$fail" -eq 0 ]]
