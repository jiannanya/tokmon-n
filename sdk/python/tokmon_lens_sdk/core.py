from __future__ import annotations

import asyncio
from dataclasses import dataclass, field
from typing import Any, Generic, Mapping, TypeVar

T = TypeVar("T")


@dataclass(frozen=True)
class LensError:
    code: str
    message: str
    retryable: bool = False


@dataclass(frozen=True)
class Result(Generic[T]):
    value: T | None = None
    error: LensError | None = None

    @property
    def ok(self) -> bool:
        return self.error is None


def ok(value: T) -> Result[T]:
    return Result(value=value)


def err(code: str, message: str, retryable: bool = False) -> Result[Any]:
    return Result(error=LensError(code, message, retryable))


def passed() -> dict[str, str]:
    return {"status": "passed"}


def completed(detail: str = "completed") -> dict[str, str]:
    return {"status": "completed", "detail": detail}


def rejected(detail: str) -> dict[str, str]:
    return {"status": "rejected", "detail": detail}


class ActPattern(Generic[T]):
    def __init__(self, kind: str, schema: str = "*") -> None:
        self.kind = kind
        self.schema = schema

    def match(self, act: Mapping[str, Any]) -> Result[T]:
        if act.get("kind") != self.kind or (
            self.schema != "*" and act.get("schema") != self.schema
        ):
            return err("pattern_mismatch", "Act does not match")
        return ok(act.get("parameters", {}))  # type: ignore[arg-type]


class OpticalInput:
    def __init__(self, frame: Mapping[str, Any] | None) -> None:
        value = frame or {}
        self.photon_window = value.get("photon_window", {"photons": []})
        self.incident = value.get("incident", {})
        self.beat = value.get("beat", {})

    @property
    def photons(self) -> list[dict[str, Any]]:
        return list(self.photon_window.get("photons", []))

    def latest(self, kind: str | None = None) -> dict[str, Any] | None:
        values = [item for item in self.photons if kind is None or item.get("kind") == kind]
        return values[-1] if values else None

    def cells(self, port: str) -> list[dict[str, Any]]:
        return list(self.incident.get(port, {}).get("cells", []))

    def one(self, port: str) -> dict[str, Any] | None:
        values = self.cells(port)
        return values[0] if len(values) == 1 else None

    def sealed(self, port: str) -> bool:
        return bool(self.incident.get(port, {}).get("sealed", False))


class WavefrontBuilder:
    def __init__(self, lens: str, input_value: OpticalInput) -> None:
        self.lens = lens
        self.input = input_value
        self.cells: list[dict[str, Any]] = []

    def emit(
        self, output: str, key: str, value: Any, *, caused_by: list[str] | None = None,
        priority: int = 0, band: str | None = None,
        schema: str = "tokmon.surface.contribution.v1", surface: bool = True,
        sensitivity: str = "normal",
    ) -> Result[None]:
        causes = list(caused_by or [])
        visible = {cell.get("id") for port in self.input.incident.values()
                   for cell in port.get("cells", [])}
        if any(item not in visible for item in causes):
            return err("permission_denied", "provenance references an invisible input cell")
        beat_key = self.input.beat.get("key", {})
        self.cells.append({
            "id": f"worker-field-{len(self.cells) + 1}",
            "band": band or output, "schema": schema, "key": key, "value": value,
            "priority": priority, "surface": surface, "sensitivity": sensitivity,
            "provenance": {
                "producer": self.lens, "generation": 0,
                "epoch": beat_key.get("epoch", 0), "path_index": 0,
                "output_port": output, "input_cells": causes, "input_photons": [],
                "assembly_hash": beat_key.get("assembly_hash", "worker-boundary"),
            },
        })
        return ok(None)

    def add(self, channel: str, key: str, value: Any, priority: int = 0) -> Result[None]:
        return self.emit(channel, key, value, priority=priority)

    def add_tool(self, *, name: str, description: str, arguments_schema: str) -> Result[None]:
        return self.add("model.tools", name, {
            "name": name, "description": description,
            "argumentsSchema": arguments_schema,
        })

    def propose(self, act: dict[str, Any], caused_by: list[str] | None = None) -> Result[None]:
        return self.emit("act.proposal", act.get("id", f"worker-act-{len(self.cells) + 1}"),
                         act, caused_by=caused_by, band="act.proposal",
                         schema="tokmon.act.proposal.v1")


@dataclass
class Emitter:
    beam: "RefractionBeam"

    async def emit(self, kind: str, schema: str, payload: Any) -> Result[dict[str, str]]:
        if self.beam.cancelled.is_set():
            return err("cancelled", "beam cancelled")
        self.beam.drafts.append({
            "ray": self.beam.act["ray"], "kind": kind, "schema": schema,
            "payload": payload, "epoch": self.beam.act.get("epoch", 0),
            "caused_by_act": self.beam.act["id"],
        })
        return ok({"id": f"draft-{len(self.beam.drafts)}"})

    async def tool_result(
        self, *, act: Mapping[str, Any], tool_name: str, payload: Mapping[str, Any]
    ) -> Result[dict[str, str]]:
        return await self.emit("tool.result", "tokmon.tool.result.v1", {
            "tool": tool_name, "source_act": act["id"], **payload,
        })


@dataclass
class RefractionBeam:
    act: dict[str, Any]
    cancelled: asyncio.Event = field(default_factory=asyncio.Event)
    drafts: list[dict[str, Any]] = field(default_factory=list)
    logs: list[dict[str, Any]] = field(default_factory=list)

    def __post_init__(self) -> None:
        self.emitter = Emitter(self)

    def log(self, level: str, message: str, **fields: Any) -> None:
        self.logs.append({"level": level, "message": message, "fields": fields})


class Lens:
    id: str
    version: str = "0.1.0"

    def view(self, input_value: OpticalInput,
             outgoing: WavefrontBuilder) -> Result[None]:
        raise NotImplementedError

    async def refract(
        self, photons: Mapping[str, Any], act: dict[str, Any], beam: RefractionBeam
    ) -> Result[dict[str, Any]]:
        raise NotImplementedError
