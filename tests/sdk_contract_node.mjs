import assert from "node:assert/strict";
import { pathToFileURL } from "node:url";
import path from "node:path";

const root = process.argv[2];
const cbor = await import(pathToFileURL(path.join(root, "cbor.mjs")).href);
const sdk = await import(pathToFileURL(path.join(root, "index.mjs")).href);
const value = { z: 1, a: "透镜", nested: [true, null, 4.5] };
assert.deepEqual(cbor.decode(cbor.encode(value)), value);
const input = new sdk.OpticalInput({ photon_window: { photons: [] }, incident: {},
  beat: { key: { epoch: 1, assembly_hash: "assembly-test" } } });
const outgoing = new sdk.WavefrontBuilder("org.tokmon.lens.test", input);
outgoing.add("diagnostic", "node", { healthy: true }, 1);
assert.equal(outgoing.cells.length, 1);
const beam = new sdk.RefractionBeam(
  { id: "act-1", ray: "ray-1", epoch: 1 }, new AbortController().signal);
await beam.emitter.emit("node.result", "tokmon.node.v1", { ok: true });
assert.equal(beam.drafts[0].kind, "node.result");
console.log("Node.js Lens SDK contract passed");
