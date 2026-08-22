from __future__ import annotations

import argparse
import asyncio
import importlib.util
import struct
import sys
from pathlib import Path
from typing import Any

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tokmon_lens_sdk.cbor import decode, encode
from tokmon_lens_sdk.core import RefractionBeam, Result, SurfaceBuilder


def load_lens(path: Path) -> Any:
    spec = importlib.util.spec_from_file_location("tokmon_user_lens", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load Lens entry: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    lens = getattr(module, "lens", None)
    if lens is None:
        lens_type = getattr(module, "LensEntry", None)
        lens = lens_type() if lens_type else None
    if lens is None or not getattr(lens, "id", None):
        raise RuntimeError("entry must export 'lens' or LensEntry")
    return lens


def result_payload(result: Any) -> dict[str, Any]:
    if isinstance(result, Result):
        if not result.ok:
            assert result.error is not None
            return {"ok": False, "error": result.error.__dict__}
        value = result.value
    else:
        value = result
    if isinstance(value, dict):
        return {"ok": True, **value}
    return {"ok": True, "value": value}


async def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--entry", type=Path, required=True)
    args = parser.parse_args()
    lens = load_lens(args.entry.resolve())
    tasks: dict[int, tuple[asyncio.Task[Any], RefractionBeam]] = {}
    reader = sys.stdin.buffer
    writer = sys.stdout.buffer

    while True:
        header = await asyncio.to_thread(reader.read, 4)
        if not header:
            return 0
        if len(header) != 4:
            raise RuntimeError("truncated worker header")
        size = struct.unpack(">I", header)[0]
        if size <= 0 or size > 16 * 1024 * 1024:
            raise RuntimeError("invalid worker frame size")
        payload = await asyncio.to_thread(reader.read, size)
        if len(payload) != size:
            raise RuntimeError("truncated worker frame")
        frame = decode(payload)
        request_id = int(frame.get("request_id", 0))
        response: dict[str, Any] = {"request_id": request_id, "payload": {}}
        try:
            if frame["type"] == "worker.hello":
                response.update(type="worker.ready", payload={
                    "protocol_major": 1, "protocol_minor": 0, "lens_id": lens.id,
                    "runtime": "cpython",
                    "runtime_version": ".".join(str(value) for value in sys.version_info[:3]),
                    "version": getattr(lens, "version", "0.1.0"),
                })
            elif frame["type"] == "lens.view.request":
                surface = SurfaceBuilder(lens.id)
                result = lens.view(frame["payload"]["window"], surface)
                response.update(type="lens.view.result", payload=result_payload(result))
                if response["payload"].get("ok"):
                    response["payload"]["surface"] = {
                        "epoch": frame["payload"].get("epoch", 0),
                        "contributions": surface.contributions,
                        "proposals": surface.proposals,
                    }
            elif frame["type"] == "lens.refract.request":
                beam = RefractionBeam(frame["payload"]["act"])
                task = asyncio.create_task(lens.refract(
                    frame["payload"]["window"], frame["payload"]["act"], beam
                ))
                tasks[request_id] = (task, beam)
                result = await task
                tasks.pop(request_id, None)
                response.update(type="lens.refract.result", payload=result_payload(result))
                if response["payload"].get("ok"):
                    response["payload"].update(drafts=beam.drafts, logs=beam.logs)
            elif frame["type"] == "beam.cancel":
                target = int(frame["payload"].get("request_id", 0))
                if target in tasks:
                    tasks[target][1].cancelled.set()
                    tasks[target][0].cancel()
                response.update(type="beam.cancelled", payload={"ok": True})
            elif frame["type"] == "worker.shutdown":
                for task, beam in tasks.values():
                    beam.cancelled.set()
                    task.cancel()
                response.update(type="worker.stopped", payload={"ok": True})
                packed = encode(response)
                writer.write(struct.pack(">I", len(packed)) + packed)
                writer.flush()
                return 0
            else:
                raise RuntimeError(f"unknown frame type: {frame['type']}")
        except asyncio.CancelledError:
            response.update(type="lens.refract.result", payload={
                "ok": False, "error": {"code": "cancelled", "message": "beam cancelled"},
            })
        except Exception as exception:  # boundary conversion is intentional
            response.update(type="lens.refract.result", payload={
                "ok": False,
                "error": {"code": "lens_crashed", "message": str(exception), "retryable": False},
            })
        packed = encode(response)
        writer.write(struct.pack(">I", len(packed)) + packed)
        writer.flush()


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
