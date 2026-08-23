#!/usr/bin/env python3
import sys
import socket
import json
import signal
import argparse

def sigterm_handler(signum, frame):
    print("\n[Python] SIGTERM received! Performing clean shutdown...", flush=True)
    sys.exit(0)

signal.signal(signal.SIGTERM, sigterm_handler)

parser = argparse.ArgumentParser(description="Abstract Namespace Worker")
parser.add_argument("--ipc-name", type=str, required=True, help="Abstract IPC name (without null byte)")
args = parser.parse_args()

# 1. Initialize SOCK_SEQPACKET
sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)

# 2. Connect via Abstract Namespace by prepending \0
abstract_address = "\0" + args.ipc_name
try:
    sock.connect(abstract_address)
except Exception as e:
    print(f"[Python] Fatal: Failed to connect to C++ IPC '{args.ipc_name}': {e}", file=sys.stderr)
    sys.exit(1)

print(f"[Python] Connected to Abstract IPC: {args.ipc_name}", flush=True)

try:
    while True:
        # SEQPACKET guarantees we receive exactly one discrete JSON message per recv() call
        data = sock.recv(65535)
        if not data:
            print("[Python] C++ closed the connection. Exiting.", flush=True)
            break

        payload = json.loads(data.decode("utf-8"))
        step = payload.get("step", 0)
        command = payload.get("command", "unknown")
        
        print(f"[Python] Processed command '{command}' step {step}", flush=True)

        # 3. Formulate and send response on the same connected socket
        response = {
            "status": "ACK",
            "in_response_to": step
        }
        sock.sendall(json.dumps(response).encode("utf-8"))

except KeyboardInterrupt:
    pass
except Exception as e:
    print(f"[Python] Runtime error: {e}", file=sys.stderr)
finally:
    sock.close()
    print("[Python] IPC socket closed.", flush=True)