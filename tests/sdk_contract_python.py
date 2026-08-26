from __future__ import annotations

import pathlib
import asyncio
import sys

root = pathlib.Path(sys.argv[1])
sys.path.insert(0, str(root))

from tokmon_lens_sdk.cbor import decode, encode
from tokmon_lens_sdk.core import OpticalContext, RefractionBeam, SurfaceBuilder, ok

value = {"z": 1, "a": "透镜", "nested": [True, None, 4.5]}
assert decode(encode(value)) == value
surface = SurfaceBuilder("org.tokmon.lens.test")
surface.add("diagnostic", "python", {"healthy": True}, 1)
assert len(surface.contributions) == 1
beam = RefractionBeam({"id": "act-1", "ray": "ray-1", "epoch": 1})
asyncio.run(beam.emitter.emit("python.result", "tokmon.python.v1", {"ok": True}))
assert beam.drafts[0]["kind"] == "python.result"

async def verify_optical() -> None:
    requests = []

    async def exchange(request):
        requests.append(request)
        return ok([1, 2] if request["operation"] == "get_all" else {"answer": 42})

    optical = OpticalContext(exchange)
    assert (await optical.get("model.context", "active")).value == {"answer": 42}
    assert (await optical.get_all("model.tools")).value == [1, 2]
    response = await optical.query(
        "math.evaluate", {"expression": "6 * 7"},
        request_schema="tokmon.math.calculate.v1",
        response_schema="tokmon.math.result.v1", timeout_ms=10,
    )
    assert response.value == {"answer": 42}
    assert requests[2]["capability"] == "math.evaluate"

asyncio.run(verify_optical())
print("CPython Lens SDK contract passed")
