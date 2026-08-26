import { pathToFileURL } from "node:url";
import { encode, decode } from "./cbor.mjs";
import { SurfaceBuilder, RefractionBeam, OpticalContext, err, ok } from "./index.mjs";

const entryIndex = process.argv.indexOf("--entry");
if (entryIndex < 0 || !process.argv[entryIndex + 1]) throw new Error("--entry is required");
const module = await import(pathToFileURL(process.argv[entryIndex + 1]).href);
const lens = module.default;
if (!lens?.id || typeof lens.view !== "function" || typeof lens.refract !== "function")
  throw new Error("entry must default-export a Lens with id, view and refract");

const controllers = new Map();
const write = (frame) => {
  const payload = encode(frame); const header = Buffer.alloc(4); header.writeUInt32BE(payload.length);
  process.stdout.write(Buffer.concat([header, payload]));
};
const failure = (error) => ({ ok: false, error: {
  code: error?.code ?? "lens_crashed", message: String(error?.message ?? error), retryable: false,
}});

let pending = Buffer.alloc(0);
const chunks = process.stdin[Symbol.asyncIterator]();
const readFrame = async () => {
  while (true) {
    if (pending.length >= 4) {
      const size = pending.readUInt32BE(0);
      if (size <= 0 || size > 16 * 1024 * 1024) throw new Error("invalid frame size");
      if (pending.length >= 4 + size) {
        const frame = decode(pending.subarray(4, 4 + size));
        pending = pending.subarray(4 + size);
        return frame;
      }
    }
    const next = await chunks.next();
    if (next.done) return null;
    pending = Buffer.concat([pending, next.value]);
  }
};

while (true) {
    const frame = await readFrame();
    if (!frame) break;
    const response = { request_id: frame.request_id, payload: {} };
    try {
      if (frame.type === "worker.hello") {
        response.type = "worker.ready";
        response.payload = { protocol_major: 1, protocol_minor: 1, lens_id: lens.id,
          runtime: "node", runtime_version: process.versions.node,
          version: lens.version ?? "0.1.0",
          features: { derive: typeof lens.derive === "function",
            coordinate: typeof lens.coordinate === "function",
            query: typeof lens.query === "function" } };
      } else if (frame.type === "lens.view.request") {
        const surface = new SurfaceBuilder(lens.id);
        const result = await lens.view(frame.payload.window, surface);
        response.type = "lens.view.result";
        response.payload = result?.ok === false ? result : { ok: true,
          surface: { epoch: frame.payload.epoch ?? 0,
            contributions: surface.contributions, proposals: surface.proposals } };
      } else if (frame.type === "lens.derive.request") {
        const result = typeof lens.derive === "function" ?
          await lens.derive(frame.payload.window) : ok(null);
        response.type = "lens.derive.result";
        response.payload = result?.ok === false ? result :
          { ok: true, state: result && typeof result === "object" && "ok" in result ?
              result.value : result ?? null };
      } else if (frame.type === "lens.coordinate.request") {
        const exchange = async (payload) => {
          write({ type: "host.optical.request", request_id: frame.request_id, payload });
          const hostResponse = await readFrame();
          if (!hostResponse || hostResponse.type !== "host.optical.result")
            return err("protocol_error", "missing optical host response");
          if (!hostResponse.payload?.ok) {
            const failure = hostResponse.payload?.error ?? {};
            return err(failure.code ?? "provider_failed",
              failure.message ?? "optical host failed", failure.retryable ?? false);
          }
          return ok(hostResponse.payload.value);
        };
        const surface = new SurfaceBuilder(lens.id);
        const result = typeof lens.coordinate === "function" ?
          await lens.coordinate(frame.payload.window, new OpticalContext(exchange), surface) :
          ok(undefined);
        response.type = "lens.coordinate.result";
        response.payload = result?.ok === false ? result : { ok: true,
          surface: { epoch: 0, contributions: surface.contributions,
            proposals: surface.proposals } };
      } else if (frame.type === "lens.query.request") {
        const result = typeof lens.query === "function" ? await lens.query(
          frame.payload.state?.value, frame.payload.capability,
          frame.payload.parameters, frame.payload.budget) :
          err("unsupported", `unsupported optical capability: ${frame.payload.capability}`);
        response.type = "lens.query.result";
        response.payload = result?.ok === false ? result :
          { ok: true, value: result && typeof result === "object" && "ok" in result ?
              result.value : result };
      } else if (frame.type === "lens.refract.request") {
        const controller = new AbortController(); controllers.set(frame.request_id, controller);
        const beam = new RefractionBeam(frame.payload.act, controller.signal);
        const result = await lens.refract(frame.payload.window, frame.payload.act, beam);
        controllers.delete(frame.request_id); response.type = "lens.refract.result";
        response.payload = result?.ok === false ? result : { ok: true,
          ...(result?.value ?? result), drafts: beam.drafts, logs: beam.logs };
      } else if (frame.type === "beam.cancel") {
        controllers.get(frame.payload.request_id)?.abort(); response.type = "beam.cancelled";
        response.payload = { ok: true };
      } else if (frame.type === "worker.shutdown") {
        for (const controller of controllers.values()) controller.abort();
        response.type = "worker.stopped"; response.payload = { ok: true }; write(response); process.exit(0);
      } else throw new Error(`unknown frame type: ${frame.type}`);
    } catch (error) {
      response.type = ({
        "lens.view.request": "lens.view.result",
        "lens.derive.request": "lens.derive.result",
        "lens.coordinate.request": "lens.coordinate.result",
        "lens.query.request": "lens.query.result",
      })[frame.type] ?? "lens.refract.result";
      response.payload = failure(error);
    }
    write(response);
}
