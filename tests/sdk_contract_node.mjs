import assert from "node:assert/strict";
import { pathToFileURL } from "node:url";
import path from "node:path";

const root = process.argv[2];
const cbor = await import(pathToFileURL(path.join(root, "cbor.mjs")).href);
const sdk = await import(pathToFileURL(path.join(root, "index.mjs")).href);
const value = { z: 1, a: "透镜", nested: [true, null, 4.5] };
assert.deepEqual(cbor.decode(cbor.encode(value)), value);
const surface = new sdk.SurfaceBuilder("org.tokmon.lens.test");
surface.add("diagnostic", "node", { healthy: true }, 1);
assert.equal(surface.contributions.length, 1);
const beam = new sdk.RefractionBeam(
  { id: "act-1", ray: "ray-1", epoch: 1 }, new AbortController().signal);
await beam.emitter.emit("node.result", "tokmon.node.v1", { ok: true });
assert.equal(beam.drafts[0].kind, "node.result");
const opticalRequests = [];
const optical = new sdk.OpticalContext(async (request) => {
  opticalRequests.push(request);
  return sdk.ok(request.operation === "get_all" ? [1, 2] : { answer: 42 });
});
assert.deepEqual((await optical.get("model.context", "active")).value, { answer: 42 });
assert.deepEqual((await optical.getAll("model.tools")).value, [1, 2]);
assert.deepEqual((await optical.query("math.evaluate", { expression: "6 * 7" }, {
  requestSchema: "tokmon.math.calculate.v1",
  responseSchema: "tokmon.math.result.v1",
  timeoutMs: 10,
})).value, { answer: 42 });
assert.equal(opticalRequests[2].capability, "math.evaluate");
console.log("Node.js Lens SDK contract passed");
