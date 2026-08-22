const concat = (parts) => Buffer.concat(parts.map((part) => Buffer.from(part)));

function head(major, value) {
  if (value < 24) return Buffer.from([(major << 5) | value]);
  if (value <= 0xff) return Buffer.from([(major << 5) | 24, value]);
  if (value <= 0xffff) {
    const result = Buffer.alloc(3); result[0] = (major << 5) | 25; result.writeUInt16BE(value, 1); return result;
  }
  if (value <= 0xffffffff) {
    const result = Buffer.alloc(5); result[0] = (major << 5) | 26; result.writeUInt32BE(value, 1); return result;
  }
  const result = Buffer.alloc(9); result[0] = (major << 5) | 27;
  result.writeBigUInt64BE(BigInt(value), 1); return result;
}

export function encode(value) {
  if (value === null || value === undefined) return Buffer.from([0xf6]);
  if (value === false) return Buffer.from([0xf4]);
  if (value === true) return Buffer.from([0xf5]);
  if (typeof value === "number" && Number.isSafeInteger(value))
    return value >= 0 ? head(0, value) : head(1, -1 - value);
  if (typeof value === "number") {
    const result = Buffer.alloc(9); result[0] = 0xfb; result.writeDoubleBE(value, 1); return result;
  }
  if (typeof value === "string") {
    const bytes = Buffer.from(value, "utf8"); return concat([head(3, bytes.length), bytes]);
  }
  if (Buffer.isBuffer(value) || value instanceof Uint8Array) {
    const bytes = Buffer.from(value); return concat([head(2, bytes.length), bytes]);
  }
  if (Array.isArray(value)) return concat([head(4, value.length), ...value.map(encode)]);
  if (typeof value === "object") {
    const entries = Object.entries(value).sort(([a], [b]) =>
      Buffer.byteLength(a) - Buffer.byteLength(b) || Buffer.from(a).compare(Buffer.from(b)));
    return concat([head(5, entries.length), ...entries.flatMap(([key, item]) => [encode(key), encode(item)])]);
  }
  throw new TypeError(`unsupported CBOR value: ${typeof value}`);
}

export function decode(buffer) {
  const bytes = Buffer.from(buffer); let offset = 0;
  const argument = (additional) => {
    if (additional < 24) return additional;
    if (additional === 24) return bytes[offset++];
    if (additional === 25) { const value = bytes.readUInt16BE(offset); offset += 2; return value; }
    if (additional === 26) { const value = bytes.readUInt32BE(offset); offset += 4; return value; }
    if (additional === 27) { const value = Number(bytes.readBigUInt64BE(offset)); offset += 8; return value; }
    throw new Error("indefinite CBOR is not supported");
  };
  const read = (depth = 0) => {
    if (depth > 64 || offset >= bytes.length) throw new Error("invalid CBOR frame");
    const initial = bytes[offset++]; const major = initial >> 5; const additional = initial & 31;
    if (major === 7) {
      if (additional === 20) return false; if (additional === 21) return true;
      if (additional === 22) return null;
      if (additional === 27) { const value = bytes.readDoubleBE(offset); offset += 8; return value; }
      throw new Error("unsupported CBOR simple value");
    }
    const length = argument(additional);
    if (major === 0) return length; if (major === 1) return -1 - length;
    if (major === 2) { const value = bytes.subarray(offset, offset + length); offset += length; return value; }
    if (major === 3) { const value = bytes.toString("utf8", offset, offset + length); offset += length; return value; }
    if (major === 4) return Array.from({ length }, () => read(depth + 1));
    if (major === 5) {
      const value = {}; for (let index = 0; index < length; index += 1) value[read(depth + 1)] = read(depth + 1);
      return value;
    }
    throw new Error("unsupported CBOR major type");
  };
  const value = read(); if (offset !== bytes.length) throw new Error("trailing CBOR bytes"); return value;
}

