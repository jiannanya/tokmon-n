from __future__ import annotations

from typing import Any

from tokmon_lens_sdk import ActPattern, Lens, RefractionBeam, Result, SurfaceBuilder, completed, ok, passed


class LensEntry(Lens):
    id = "org.tokmon.lens.adder-python"
    add = ActPattern[dict[str, float]]("tool.add", "tokmon.math.add.v1")

    def view(self, _photons: dict[str, Any], surface: SurfaceBuilder) -> Result[None]:
        surface.add_tool(
            name="add", description="计算两个数字之和", arguments_schema="tokmon.math.add.v1"
        )
        return ok(None)

    async def refract(
        self, _photons: dict[str, Any], act: dict[str, Any], beam: RefractionBeam
    ) -> Result[dict[str, Any]]:
        matched = self.add.match(act)
        if not matched.ok:
            return ok(passed())
        assert matched.value is not None
        emitted = await beam.emitter.tool_result(
            act=act, tool_name="add",
            payload={"result": matched.value["left"] + matched.value["right"]},
        )
        return ok(completed()) if emitted.ok else Result(error=emitted.error)


lens = LensEntry()

