import json
import pathlib
import socket
import sys


def read_request(connection: socket.socket) -> None:
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = connection.recv(4096)
        if not chunk:
            return
        data += chunk
    header, body = data.split(b"\r\n\r\n", 1)
    length = 0
    for line in header.decode("latin1").split("\r\n")[1:]:
        if line.lower().startswith("content-length:"):
            length = int(line.split(":", 1)[1].strip())
    while len(body) < length:
        chunk = connection.recv(4096)
        if not chunk:
            break
        body += chunk
    if body:
        json.loads(body[:length].decode("utf-8"))


def send(connection: socket.socket, status: int, body: bytes,
         content_type: str = "application/json") -> None:
    reason = "OK" if status == 200 else "Service Unavailable"
    header = (
        f"HTTP/1.1 {status} {reason}\r\n"
        f"Content-Type: {content_type}\r\n"
        f"Content-Length: {len(body)}\r\n"
        "Connection: close\r\n\r\n"
    ).encode("ascii")
    connection.sendall(header + body)


def main() -> None:
    requested_port = int(sys.argv[1])
    ready_path = pathlib.Path(sys.argv[2])
    request_count = int(sys.argv[3])
    failures = int(sys.argv[4])
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", requested_port))
    server.listen(4)
    ready_path.write_text(str(server.getsockname()[1]), encoding="utf-8")
    for index in range(request_count):
        connection, _ = server.accept()
        with connection:
            read_request(connection)
            if index < failures:
                send(connection, 503, b'{"error":{"message":"retry"}}')
                continue
            events = [
                {"choices": [{"delta": {"reasoning_content": "fixture reasoning"}}]},
                {"choices": [{"delta": {"content": "hello "}}]},
                {"choices": [{"delta": {"content": "world"}}],
                 "usage": {"prompt_tokens": 3, "completion_tokens": 2}},
            ]
            payload = "".join(
                f"data: {json.dumps(event, separators=(',', ':'))}\n\n"
                for event in events
            ) + "data: [DONE]\n\n"
            send(connection, 200, payload.encode("utf-8"), "text/event-stream")
    server.close()


if __name__ == "__main__":
    main()
