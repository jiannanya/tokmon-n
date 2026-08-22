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


class SurfaceBuilder:
    def __init__(self, lens: str) -> None:
        self.lens = lens
        self.contributions: list[dict[str, Any]] = []
        self.proposals: list[dict[str, Any]] = []

    def add(self, channel: str, key: str, value: Any, priority: int = 0) -> Result[None]:
        self.contributions.append(
            {"lens": self.lens, "channel": channel, "key": key,
             "value": value, "priority": priority}
        )
        return ok(None)

    def add_tool(self, *, name: str, description: str, arguments_schema: str) -> Result[None]:
        return self.add("model.tools", name, {
            "name": name, "description": description,
            "argumentsSchema": arguments_schema,
        })

    def propose(self, act: dict[str, Any]) -> Result[None]:
        self.proposals.append(act)
        return ok(None)


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

    def view(self, photons: Mapping[str, Any], surface: SurfaceBuilder) -> Result[None]:
        raise NotImplementedError

    async def refract(
        self, photons: Mapping[str, Any], act: dict[str, Any], beam: RefractionBeam
    ) -> Result[dict[str, Any]]:
        raise NotImplementedError

