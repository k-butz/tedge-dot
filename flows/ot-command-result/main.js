// ot-command-result: mirror a connector command result back to the thin-edge command.
//
// Direction: OT protocol format -> thin-edge.io data model.
//   in:  te/device/<device>/ot/<protocol>/cmd/<verb>/<id>  {"status":"executing|successful|failed",...}
//   out: te/device/<device>///cmd/<command type>/<id>      (same payload, retained)
//
// Protocol-neutral and verb-neutral: mirrors any connector's command result onto the matching
// generic `ot_<verb>` command (the connector verb's `-` becomes `_` and gains the `ot_` prefix:
// write -> ot_write, set-config -> ot_set_config, define-device -> ot_define_device, ...).
// A request whose init carried `origin.command` (ot-command-forward sets it when it reshapes a
// command, e.g. parameter_update -> write-batch) is mirrored onto that command type instead.
// Only connector-driven transitions are mirrored (status != init), so the original request that
// ot-command-forward sends to the connector is not echoed back (no loop).

const decoder = new TextDecoder();

export function onMessage(message, context) {
  const prefix = context.config?.command_prefix || "ot_";

  const parts = message.topic.split("/");
  const device = parts[2];
  const verb = parts[parts.length - 2];
  const id = `${parts[parts.length - 1]}`.replace(/^ot--/, "");

  let payload;
  try {
    payload = JSON.parse(decoder.decode(message.payload));
  } catch (_e) {
    return []; // ignore clearing/empty/non-JSON messages
  }
  const status = payload?.status ?? "";

  // On init: cache the full payload so metadata (e.g. "c8y-mapper", "origin") can be
  // re-attached to connector result messages that won't carry those keys.
  if (status === "init") {
    context.script.set(id, payload);
    return [];
  }
  if (status === "") return []; // ignore clearing/non-JSON messages

  const initPayload = context.script.get(id) ?? {};
  // `origin` from the cached request, else from the result itself: the connector echoes it into
  // every transition it publishes (§6.4), which is what makes a REPLAYED terminal result
  // routable. The cache is in-memory, so a mapper that restarts between the request and the
  // result has lost it — and mirroring onto `ot_write_batch` instead of `parameter_update`
  // would leave the cloud operation waiting forever on a command that never completes.
  const originOf = (p) => (p?.origin && typeof p.origin === "object" ? p.origin : null);
  const origin = originOf(initPayload) ?? originOf(payload);
  const commandType = origin?.command || prefix + verb.split("-").join("_");

  // Merge stored init metadata with the connector result; connector fields win. The request
  // body of a reshaped command (`writes`) is not echoed: the thin-edge command keeps its own
  // shape (`origin.set` / `origin.parameters` say what was asked).
  const { writes: _writes, ...initMeta } = initPayload;
  const merged = { ...initMeta, ...payload };
  if (status === "failed" && origin?.error) {
    merged.reason = [origin.error, payload.reason].filter((r) => r).join("; ");
  }

  // Clean up cached state once the command reaches a terminal state.
  if (status === "successful" || status === "failed") {
    context.script.remove(id, null);
  }

  return [{
    topic: `te/device/${device}///cmd/${commandType}/${id}`,
    payload: JSON.stringify(merged),
    mqtt: { retain: true, qos: 1 },
  }];
}
