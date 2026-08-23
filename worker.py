#!/usr/bin/env python3
import sys
import os
import socket
import json
import signal
import argparse

def sigterm_handler(signum, frame):
    print("\n[Python] SIGTERM received! Performing clean shutdown...", flush=True)
    sys.exit(0)

signal.signal(signal.SIGTERM, sigterm_handler)

parser = argparse.ArgumentParser(description="UDS JSON Worker")
parser.add_argument("--listen-sock", type=str, default="/tmp/federate_py.sock", help="Path where Python listens")
parser.add_argument("--send-sock", type=str, default="/tmp/federate_cpp.sock", help="Path where C++ listens")
args = parser.parse_args()

# Clean up stale socket file if it exists
if os.path.exists(args.listen_sock):
    os.remove(args.listen_sock)

# Initialize AF_UNIX datagram socket
sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
sock.bind(args.listen_sock)
sock.settimeout(1.0)

print(f"[Python] UDS Worker initialized. Listening on {args.listen_sock}, sending to {args.send_sock}", flush=True)

try:
    while True:
        try:
            data, sender_addr = sock.recvfrom(65535)
            payload = json.loads(data.decode("utf-8"))

            step = payload.get("step", 0)
            command = payload.get("command", "unknown")
            print(f"[Python] Received '{command}' (step={step})", flush=True)

            response = {
                "status": "ACK",
                "in_response_to": step,
                "command": command,
                "worker_state": "ready"
            }

            response_bytes = json.dumps(response).encode("utf-8")
            sock.sendto(response_bytes, args.send_sock)
            print(f"[Python] Sent response to {args.send_sock}", flush=True)

        except socket.timeout:
            continue
        except json.JSONDecodeError as e:
            print(f"[Python] Malformed JSON received: {e}", flush=True)
        except Exception as e:
            print(f"[Python] Unexpected runtime error: {e}", flush=True)

except KeyboardInterrupt:
    pass
finally:
    sock.close()
    if os.path.exists(args.listen_sock):
        os.remove(args.listen_sock)
    print("[Python] UDS Socket closed and unlinked. Exiting.", flush=True)