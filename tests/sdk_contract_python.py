from __future__ import annotations

import pathlib
import asyncio
import sys

root = pathlib.Path(sys.argv[1])
sys.path.insert(0, str(root))

from tokmon_lens_sdk.cbor import decode, encode
from tokmon_lens_sdk.core import RefractionBeam, SurfaceBuilder

value = {"z": 1, "a": "透镜", "nested": [True, None, 4.5]}
assert decode(encode(value)) == value
surface = SurfaceBuilder("org.tokmon.lens.test")
surface.add("diagnostic", "python", {"healthy": True}, 1)
assert len(surface.contributions) == 1
beam = RefractionBeam({"id": "act-1", "ray": "ray-1", "epoch": 1})
asyncio.run(beam.emitter.emit("python.result", "tokmon.python.v1", {"ok": True}))
assert beam.drafts[0]["kind"] == "python.result"
print("CPython Lens SDK contract passed")
