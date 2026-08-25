#!/usr/bin/env python3
import sys
import socket
import json
import signal
import argparse

def sigterm_handler(signum, frame):
    print("[Python] SIGTERM received! Performing clean shutdown...", flush=True)
    sys.exit(0)

signal.signal(signal.SIGTERM, sigterm_handler)

parser = argparse.ArgumentParser(description="Abstract Namespace Slave Worker")
parser.add_argument("--ipc-name", type=str, required=True, help="Abstract IPC name (without null byte)")
args = parser.parse_args()

# 1. Initialize SEQPACKET socket (inherits default blocking mode with no timeout)
sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)

# 2. Connect via Abstract Namespace (\0 prefix)
abstract_address = "\0" + args.ipc_name
try:
    sock.connect(abstract_address)
    print(f"[Python] Connected to Abstract IPC: {args.ipc_name}", flush=True)
except Exception as e:
    print(f"[Python] Fatal: Failed to connect to IPC endpoint '{args.ipc_name}': {e}", file=sys.stderr)
    sys.exit(1)

try:
    while True:
        # Blocks indefinitely until C++ sends a message or terminates the link
        data = sock.recv(65536)
        if not data:
            print("[Python] C++ closed the IPC connection. Exiting loop.", flush=True)
            break

        try:
            payload = json.loads(data.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError) as e:
            print(f"[Python] Malformed payload received: {e}", file=sys.stderr)
            err_response = {"status": "ERROR", "error": "malformed_json"}
            sock.sendall(json.dumps(err_response).encode("utf-8"))
            continue

        step = payload.get("step", 0)
        command = payload.get("command", "unknown")

        # --- Subprogram logic runs here ---
        response = {
            "status": "ACK",
            "in_response_to": step,
            "command": command
        }

        # Send reply back to C++
        sock.sendall(json.dumps(response).encode("utf-8"))

except BrokenPipeError:
    print("[Python] Parent disconnected abruptly.", file=sys.stderr)
except KeyboardInterrupt:
    pass
except Exception as e:
    print(f"[Python] Unexpected runtime error: {e}", file=sys.stderr)
finally:
    # Because SystemExit inherits from BaseException (not Exception)
    # it skips the except Exception: clause, unwinds the call stack
    # and triggers the finally: block before the interpreter terminates
    sock.close()
    print("[Python] IPC socket closed.", flush=True)