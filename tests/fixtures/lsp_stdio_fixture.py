import json
import sys


def read_message():
    headers = {}
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        if line == b"\r\n":
            break
        name, value = line.decode("ascii").split(":", 1)
        headers[name.lower()] = value.strip()
    length = int(headers["content-length"])
    return json.loads(sys.stdin.buffer.read(length))


def write_message(message):
    body = json.dumps(message, separators=(",", ":")).encode("utf-8")
    sys.stdout.buffer.write(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii"))
    sys.stdout.buffer.write(body)
    sys.stdout.buffer.flush()


def main():
    initialized = False
    opened = False
    shutdown = False
    while True:
        message = read_message()
        if message is None:
            return 1
        method = message.get("method")
        if method == "initialize":
            initialized = True
            write_message({"jsonrpc": "2.0", "id": message["id"],
                           "result": {"capabilities": {"hoverProvider": True}}})
        elif method == "initialized":
            continue
        elif method == "textDocument/didOpen":
            opened = True
        elif method == "textDocument/hover":
            write_message({"jsonrpc": "2.0", "id": message["id"],
                           "result": {"contents": {"kind": "plaintext",
                                                   "value": "fixture-hover"},
                                      "initialized": initialized,
                                      "document_opened": opened}})
        elif method == "shutdown":
            shutdown = True
            write_message({"jsonrpc": "2.0", "id": message["id"], "result": None})
        elif method == "exit":
            return 0 if shutdown else 2


if __name__ == "__main__":
    raise SystemExit(main())
