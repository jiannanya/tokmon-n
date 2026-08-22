import sys

print("PTY-READY 终端", flush=True)
for line in sys.stdin:
    value = line.rstrip("\r\n")
    if value == "quit":
        print("PTY-BYE", flush=True)
        break
    print("PTY-ECHO:" + value, flush=True)
