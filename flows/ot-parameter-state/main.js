// ot-parameter-state: keep the device twin's parameter sets in sync with the connector.
//
// Direction: OT protocol format -> thin-edge.io data model.
//   in:  te/device/<device>/ot/<protocol>/sample/<point>         (reads of parameter points)
//        te/device/<device>/ot/<protocol>/cmd/write/<id>         (single write results)
//        te/device/<device>/ot/<protocol>/cmd/write-batch/<id>   (batch write results)
//   out: te/device/<device>///twin/<set>                         (retained: { <point>: value })
//
// A *parameter* is a point whose `access` (echoed in every sample) permits writes, or that opts
// in via meta.parameter (meta.parameter = false opts a writable point out). Parameters are
// grouped into *sets* (meta.parameter.set, else the default set); each set is one twin fragment
// keyed by point id — the same sets `tedge-dot describe` declares in the cloud.
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
//   "ot-parameter-values:<device>:<set>"  -> { <point>: value }
//   "ot-parameter-set:<device>:<point>"   -> set name, or false for opted-out points

const decoder = new TextDecoder();

function canWrite(access) {
  const a = String(access || "read").toLowerCase();
  return a === "write" || a === "read_write" || a === "readwrite";
}

function defaultSet(context, protocol) {
  const configured = String(context.config?.default_set || "").trim();
  return configured || `${String(protocol).replace(/[^A-Za-z0-9]/g, "_")}_parameters`;
}

// The set a sampled point belongs to (false when it is not a parameter).
function setFromSample(sample, dflt) {
  const mp = sample.meta?.parameter;
  if (mp === false) return false;
  if (typeof mp === "string" && mp) return mp;
  if (mp && typeof mp === "object" && typeof mp.set === "string" && mp.set) return mp.set;
  if (mp === undefined || mp === null) return canWrite(sample.access) ? dflt : false;
  return dflt; // true or an object without a set
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
  const dflt = defaultSet(context, protocol);

  if (kind === "sample") {
    const point = payload.point || parts[6];
    // Remember the point's set (or opt-out) so write results can be attributed later.
    const set = setFromSample(payload, dflt);
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
      return known === undefined || known === null ? dflt : known;
    };
    return applyValues(context, device, updates, resolveSet);
  }
  return [];
}
