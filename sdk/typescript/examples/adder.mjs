import { actPattern, completed, defineLens, ok } from "../index.mjs";

const add = actPattern("tool.add", "tokmon.math.add.v1");

export default defineLens({
  id: "org.tokmon.lens.adder-node",
  version: "0.1.0",
  view(_window, surface) {
    surface.model.addTool({
      name: "add", description: "计算两个数字之和", argumentsSchema: "tokmon.math.add.v1",
    });
    return ok(undefined);
  },
  async refract(_window, act, beam) {
    const matched = add.match(act);
    if (!matched.ok) return ok({ status: "passed" });
    const emitted = await beam.emitter.toolResult(act, "add", {
      result: Number(matched.value.left) + Number(matched.value.right),
    });
    return emitted.ok ? ok(completed()) : emitted;
  },
});

