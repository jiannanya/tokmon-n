import { pathToFileURL } from "node:url";
import { encode, decode } from "./cbor.mjs";
import { SurfaceBuilder, RefractionBeam } from "./index.mjs";

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
for await (const chunk of process.stdin) {
  pending = Buffer.concat([pending, chunk]);
  while (pending.length >= 4) {
    const size = pending.readUInt32BE(0);
    if (size <= 0 || size > 16 * 1024 * 1024) throw new Error("invalid frame size");
    if (pending.length < 4 + size) break;
    const frame = decode(pending.subarray(4, 4 + size)); pending = pending.subarray(4 + size);
    const response = { request_id: frame.request_id, payload: {} };
    try {
      if (frame.type === "worker.hello") {
        response.type = "worker.ready";
        response.payload = { protocol_major: 1, protocol_minor: 0, lens_id: lens.id,
          runtime: "node", runtime_version: process.versions.node,
          version: lens.version ?? "0.1.0" };
      } else if (frame.type === "lens.view.request") {
        const surface = new SurfaceBuilder(lens.id);
        const result = await lens.view(frame.payload.window, surface);
        response.type = "lens.view.result";
        response.payload = result?.ok === false ? result : { ok: true,
          surface: { epoch: frame.payload.epoch ?? 0,
            contributions: surface.contributions, proposals: surface.proposals } };
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
      response.type = frame.type?.startsWith("lens.view") ? "lens.view.result" : "lens.refract.result";
      response.payload = failure(error);
    }
    write(response);
  }
}
