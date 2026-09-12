// ot-parameter-state: keep the device twin's parameter sets in sync with the connector.
//
// Direction: OT protocol format -> thin-edge.io data model.
//   in:  te/device/<device>/ot/<protocol>/sample/<point>         (reads of parameter points)
//        te/device/<device>/ot/<protocol>/cmd/write/<id>         (single write results)
//        te/device/<device>/ot/<protocol>/cmd/write-batch/<id>   (batch write results)
//        te/device/<device>/ot/<protocol>/status/link            (retained: the device type)
//   out: te/device/<device>///twin/<set>                         (retained: { <point>: value })
//
// A *parameter* is a point whose `access` (echoed in every sample) permits writes, or that opts
// in via meta.parameter (meta.parameter = false opts a writable point out). Parameters are
// grouped into *sets*; each set is one twin fragment keyed by point id — the same sets
// `tedge-dot describe` declares in the cloud, which is why the naming rule below has to match
// the SDK's (impl/rust/crates/sdk/src/descriptor.rs, impl/c/sdk/src/descriptor.c):
//
//   <device type, else the protocol>_<meta.parameter.group, default "control">_parameters
//
// The device type is echoed in every sample and on the link status (contract §3.1/§5), so the
// flow never needs the connector's configuration file. meta.parameter.set bypasses the rule and
// is used verbatim.
//
// Where values come from:
//   * readable parameters: every good sample (so the twin follows the device, including
//     changes made locally on the PLC/HMI);
//   * write-only parameters (access = "write"): the last acknowledged write — the device cannot
//     be read back, so this is the *commanded* state, not a measured one. It is unknown (absent
//     from the twin) until the first successful write after the mapper started, and it lands in
//     the default set (no sample ever tells us its meta);
//   * read/write parameters are also updated optimistically from a successful write, then
//     confirmed/corrected by the next sample.
//
// Shared state (context.mapper):
//   "ot-protocol:<device>"                -> protocol segment seen for the device
//                                            (read by ot-command-forward)
//   "ot-device-type:<device>"             -> declared device type, when the connector reports one
//   "ot-parameter-values:<device>:<set>"  -> { <point>: value }
//   "ot-parameter-set:<device>:<point>"   -> set name, or false for opted-out points

const decoder = new TextDecoder();

function canWrite(access) {
  const a = String(access || "read").toLowerCase();
  return a === "write" || a === "read_write" || a === "readwrite";
}

const DEFAULT_GROUP = "control";

// Every RUN of characters outside [A-Za-z0-9] becomes a single "_", so a device type can be
// written the way it reads ("acme-meter-v2") and still be a valid fragment key. A run rather
// than a character because the C SDK folds bytes and this folds characters: collapsing runs is
// what makes them agree on a name with a non-ASCII character in it.
function sanitize(s) {
  return String(s).replace(/[^A-Za-z0-9]+/g, "_");
}

// How this device's sets are named: by its declared type, else by the protocol. `forced` is the
// flow's default_set (and `tedge-dot describe --set`): one name for every point that does not
// give an absolute one.
function naming(context, device, protocol) {
  return {
    forced: String(context.config?.default_set || "").trim(),
    qualifier: context.mapper.get(`ot-device-type:${device}`) || protocol,
  };
}

// The set for a point in `group` (undefined = the default group).
// Sanitized as a whole, so a qualifier that already ends in a separator does not produce a
// doubled "_" — the SDKs assemble the name the same way.
function setFor(names, group) {
  if (names.forced) return names.forced;
  return sanitize(`${names.qualifier}_${group || DEFAULT_GROUP}_parameters`);
}

// The set a sampled point belongs to (false when it is not a parameter).
function setFromSample(sample, names) {
  const mp = sample.meta?.parameter;
  if (mp === false) return false;
  if (typeof mp === "string" && mp) return mp; // a bare string names the set, absolutely
  if (mp && typeof mp === "object") {
    if (typeof mp.set === "string" && mp.set) return mp.set;
    return setFor(names, typeof mp.group === "string" ? mp.group : undefined);
  }
  if (mp === undefined || mp === null) return canWrite(sample.access) ? setFor(names) : false;
  return setFor(names); // `true`, or any other scalar
}

// Apply {point: value} updates for a device; returns the twin messages of the changed sets.
function applyValues(context, device, updates, resolveSet) {
  const changed = new Set();
  for (const [point, value] of Object.entries(updates)) {
    if (value === undefined) continue;
    const set = resolveSet(point);
    if (!set) continue;
    const key = `ot-parameter-values:${device}:${set}`;
    const values = context.mapper.get(key) || {};
    if (JSON.stringify(values[point]) === JSON.stringify(value)) continue;
    values[point] = value;
    context.mapper.set(key, values);
    changed.add(set);
  }
  return [...changed].map((set) => ({
    topic: `te/device/${device}///twin/${set}`,
    payload: JSON.stringify(context.mapper.get(`ot-parameter-values:${device}:${set}`) || {}),
    mqtt: { retain: true, qos: 1 },
  }));
}

export function onMessage(message, context) {
  const parts = message.topic.split("/");
  const device = parts[2];
  const protocol = parts[4];
  const kind = parts[5];
  let payload;
  try {
    payload = JSON.parse(decoder.decode(message.payload));
  } catch (_e) {
    return [];
  }
  if (!payload || typeof payload !== "object") return [];
  context.mapper.set(`ot-protocol:${device}`, protocol);
  // The device type qualifies every set name below. It arrives on the retained link status
  // (before any sample) and on every sample, so a device with only write-only points — which
  // never samples — still gets its sets named after its type.
  if (typeof payload.type === "string" && payload.type) {
    context.mapper.set(`ot-device-type:${device}`, payload.type);
  }
  if (kind === "status") return [];
  const names = naming(context, device, protocol);

  if (kind === "sample") {
    const point = payload.point || parts[6];
    // Remember the point's set (or opt-out) so write results can be attributed later.
    const set = setFromSample(payload, names);
    context.mapper.set(`ot-parameter-set:${device}:${point}`, set);
    if (!set || payload.quality !== "good" || payload.value === undefined) return [];
    return applyValues(context, device, { [point]: payload.value }, () => set);
  }

  if (kind === "cmd" && payload.status === "successful") {
    const verb = parts[6];
    const updates = {};
    if (verb === "write" && typeof payload.point === "string" && payload.value !== undefined) {
      updates[payload.point] = payload.value;
    } else if (verb === "write-batch") {
      for (const r of payload.results ?? []) {
        if (r?.status === "successful" && typeof r.point === "string" && r.value !== undefined) {
          updates[r.point] = r.value;
        }
      }
    }
    // A point that was written is writable by definition: known set, else the default one.
    const resolveSet = (point) => {
      const known = context.mapper.get(`ot-parameter-set:${device}:${point}`);
      return known === undefined || known === null ? setFor(names) : known;
    };
    return applyValues(context, device, updates, resolveSet);
  }
  return [];
}
