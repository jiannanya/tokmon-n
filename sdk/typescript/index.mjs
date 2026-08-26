export const ok = (value) => ({ ok: true, value });
export const err = (code, message, retryable = false) => ({
  ok: false,
  error: { code, message, retryable },
});

export const defineLens = (lens) => Object.freeze(lens);

export function actPattern(kind, schema = "*") {
  return Object.freeze({
    kind,
    schema,
    match(act) {
      if (act.kind !== kind || (schema !== "*" && act.schema !== schema)) {
        return err("pattern_mismatch", "Act does not match");
      }
      return ok(act.parameters ?? {});
    },
  });
}

export class OpticalInput {
  constructor(frame) {
    this.photonWindow = frame?.photon_window ?? { photons: [] };
    this.incident = frame?.incident ?? {};
    this.beat = frame?.beat ?? {};
  }
  get photons() { return this.photonWindow.photons ?? []; }
  latest(kind) {
    const values = kind ? this.photons.filter((photon) => photon.kind === kind) : this.photons;
    return values.length ? values[values.length - 1] : undefined;
  }
  cells(port) { return this.incident?.[port]?.cells ?? []; }
  one(port) { const values = this.cells(port); return values.length === 1 ? values[0] : undefined; }
  sealed(port) { return this.incident?.[port]?.sealed === true; }
}

export class WavefrontBuilder {
  #lens;
  #input;
  cells = [];
  constructor(lens, input) { this.#lens = lens; this.#input = input; }
  emit(output, key, value, options = {}) {
    const causedBy = [...(options.causedBy ?? [])];
    const visible = new Set(Object.values(this.#input.incident ?? {})
      .flatMap((port) => port?.cells ?? []).map((cell) => cell.id));
    if (causedBy.some((id) => !visible.has(id)))
      return err("permission_denied", "provenance references an invisible input cell");
    this.cells.push({
      id: `worker-field-${this.cells.length + 1}`,
      band: options.band ?? output,
      schema: options.schema ?? "tokmon.surface.contribution.v1",
      key,
      value,
      priority: options.priority ?? 0,
      surface: options.surface ?? true,
      sensitivity: options.sensitivity ?? "normal",
      provenance: {
        producer: this.#lens,
        generation: 0,
        epoch: this.#input.beat?.key?.epoch ?? 0,
        path_index: 0,
        output_port: output,
        input_cells: causedBy,
        input_photons: [],
        assembly_hash: this.#input.beat?.key?.assembly_hash ?? "worker-boundary",
      },
    });
    return ok(undefined);
  }
  add(channel, key, value, priority = 0) {
    return this.emit(channel, key, value, { priority, surface: true });
  }
  propose(act, causedBy = []) {
    return this.emit("act.proposal", act.id ?? `worker-act-${this.cells.length + 1}`,
      act, { causedBy, band: "act.proposal", schema: "tokmon.act.proposal.v1",
        surface: true });
  }
  get model() {
    return {
      addTool: (tool) => this.add("model.tools", tool.name, tool, 0),
      addContext: (key, value) => this.add("model.context", key, value, 0),
    };
  }
  get ui() {
    return { add: (key, value, priority = 0) => this.add("ui", key, value, priority) };
  }
}

export class RefractionBeam {
  constructor(act, signal) {
    this.act = act;
    this.signal = signal;
    this.drafts = [];
    this.logs = [];
    this.emitter = {
      emit: async (kind, schema, payload) => {
        if (signal.aborted) return err("cancelled", "beam cancelled");
        const draft = {
          ray: act.ray,
          kind,
          schema,
          payload,
          epoch: act.epoch,
          caused_by_act: act.id,
        };
        this.drafts.push(draft);
        return ok({ id: `draft-${this.drafts.length}` });
      },
      toolResult: async (sourceAct, toolName, payload) =>
        this.emitter.emit("tool.result", "tokmon.tool.result.v1", {
          tool: toolName,
          source_act: sourceAct.id,
          ...payload,
        }),
    };
  }
  log(level, message, fields = {}) {
    this.logs.push({ level, message, fields });
  }
}

export const passed = () => ({ status: "passed" });
export const completed = (detail = "completed") => ({ status: "completed", detail });
export const rejected = (detail) => ({ status: "rejected", detail });
