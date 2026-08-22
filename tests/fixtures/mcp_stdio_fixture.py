import json
import sys


def main() -> None:
    request = json.loads(sys.stdin.readline())
    if request.get("method") == "tools/list":
        result = {
            "tools": [
                {
                    "name": "fixture.echo",
                    "description": "Echo fixture",
                    "inputSchema": {
                        "type": "object",
                        "properties": {"text": {"type": "string"}},
                        "required": ["text"],
                        "additionalProperties": False,
                    },
                }
            ]
        }
    elif request.get("method") == "tools/call":
        result = {"content": [{"type": "text", "text": "fixture-result"}]}
    else:
        result = {"capabilities": {}}
    print(json.dumps({"jsonrpc": "2.0", "id": request.get("id"), "result": result},
                     separators=(",", ":")), flush=True)


if __name__ == "__main__":
    main()
