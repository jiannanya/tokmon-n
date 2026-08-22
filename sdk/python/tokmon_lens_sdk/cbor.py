from __future__ import annotations

import struct
from typing import Any


def _head(major: int, value: int) -> bytes:
    prefix = major << 5
    if value < 24:
        return bytes([prefix | value])
    if value <= 0xFF:
        return bytes([prefix | 24, value])
    if value <= 0xFFFF:
        return bytes([prefix | 25]) + struct.pack(">H", value)
    if value <= 0xFFFFFFFF:
        return bytes([prefix | 26]) + struct.pack(">I", value)
    return bytes([prefix | 27]) + struct.pack(">Q", value)


def encode(value: Any) -> bytes:
    if value is None:
        return b"\xf6"
    if value is False:
        return b"\xf4"
    if value is True:
        return b"\xf5"
    if isinstance(value, int):
        return _head(0, value) if value >= 0 else _head(1, -1 - value)
    if isinstance(value, float):
        return b"\xfb" + struct.pack(">d", value)
    if isinstance(value, str):
        data = value.encode("utf-8")
        return _head(3, len(data)) + data
    if isinstance(value, (bytes, bytearray, memoryview)):
        data = bytes(value)
        return _head(2, len(data)) + data
    if isinstance(value, (list, tuple)):
        return _head(4, len(value)) + b"".join(encode(item) for item in value)
    if isinstance(value, dict):
        items = sorted(value.items(), key=lambda item: (len(item[0].encode()), item[0].encode()))
        return _head(5, len(items)) + b"".join(
            encode(str(key)) + encode(item) for key, item in items
        )
    raise TypeError(f"unsupported CBOR value: {type(value)!r}")


def decode(data: bytes) -> Any:
    offset = 0

    def argument(additional: int) -> int:
        nonlocal offset
        if additional < 24:
            return additional
        sizes = {24: 1, 25: 2, 26: 4, 27: 8}
        size = sizes.get(additional)
        if size is None or offset + size > len(data):
            raise ValueError("invalid CBOR argument")
        value = int.from_bytes(data[offset:offset + size], "big")
        offset += size
        return value

    def read(depth: int = 0) -> Any:
        nonlocal offset
        if depth > 64 or offset >= len(data):
            raise ValueError("invalid CBOR frame")
        initial = data[offset]
        offset += 1
        major, additional = initial >> 5, initial & 31
        if major == 7:
            if additional == 20:
                return False
            if additional == 21:
                return True
            if additional == 22:
                return None
            if additional == 27:
                value = struct.unpack_from(">d", data, offset)[0]
                offset += 8
                return value
            raise ValueError("unsupported CBOR simple value")
        length = argument(additional)
        if major == 0:
            return length
        if major == 1:
            return -1 - length
        if major == 2:
            value = data[offset:offset + length]
            offset += length
            return value
        if major == 3:
            value = data[offset:offset + length].decode("utf-8")
            offset += length
            return value
        if major == 4:
            return [read(depth + 1) for _ in range(length)]
        if major == 5:
            return {read(depth + 1): read(depth + 1) for _ in range(length)}
        raise ValueError("unsupported CBOR major type")

    value = read()
    if offset != len(data):
        raise ValueError("trailing CBOR bytes")
    return value

