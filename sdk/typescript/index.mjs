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

export class SurfaceBuilder {
  #lens;
  contributions = [];
  proposals = [];
  constructor(lens) { this.#lens = lens; }
  add(channel, key, value, priority = 0) {
    this.contributions.push({ lens: this.#lens, channel, key, value, priority });
    return ok(undefined);
  }
  propose(act) {
    this.proposals.push(act);
    return ok(undefined);
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

