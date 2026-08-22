import http.server
import pathlib
import sys


class Handler(http.server.BaseHTTPRequestHandler):
    output = pathlib.Path(sys.argv[2])
    protocol_version = "HTTP/1.1"
    received = False

    def do_POST(self):
        length = int(self.headers.get("content-length", "0"))
        body = self.rfile.read(length)
        Handler.received = True
        Handler.output.write_bytes(body)
        Handler.output.with_suffix(".type").write_text(
            self.headers.get("content-type", ""), encoding="utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", "2")
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(b"{}")
        self.wfile.flush()
        self.close_connection = True

    def log_message(self, *_):
        pass


def main():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), Handler)
    port_file = Handler.output.with_suffix(".port")
    port_file.write_text(str(server.server_address[1]), encoding="ascii")
    server.timeout = 0.5
    while not Handler.received:
        server.handle_request()
    server.server_close()


if __name__ == "__main__":
    main()
