#!/usr/bin/env python3
import sys
import socket
import json
import signal
import argparse

def sigterm_handler(signum, frame):
    print("\n[Python] SIGTERM received! Shutting down cleanly...", flush=True)
    sys.exit(0)

# Graceful termination trap
signal.signal(signal.SIGTERM, sigterm_handler)

parser = argparse.ArgumentParser(description="Dual-Port UDP JSON Worker")
parser.add_argument("--listen-port", type=int, default=5005, help="Port to receive commands from C++")
parser.add_argument("--send-port", type=int, default=5006, help="Port to send responses/ACKs to C++")
args = parser.parse_args()

# Initialize socket
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

# Bind strictly to listen-port
sock.bind(("127.0.0.1", args.listen_port))
target_addr = ("127.0.0.1", args.send_port)

# Yield periodically to allow OS signals (SIGTERM/SIGINT) to interrupt cleanly
sock.settimeout(1.0)

print(
    f"[Python] Worker initialized. Listening on 127.0.0.1:{args.listen_port}, "
    f"sending to 127.0.0.1:{args.send_port}...", 
    flush=True
)

try:
    while True:
        try:
            # 1. Receive incoming UDP datagram
            data, sender_addr = sock.recvfrom(65535)
            
            # 2. Parse incoming JSON
            payload = json.loads(data.decode("utf-8"))
            step = payload.get("step", 0)
            command = payload.get("command", "unknown")
            
            print(f"[Python] Received '{command}' (step={step}) from {sender_addr}", flush=True)

            # 3. Construct response / ACK payload
            response = {
                "status": "ACK",
                "in_response_to": step,
                "command": command,
                "worker_state": "ok"
            }

            # 4. Explicitly send response to configured target_addr
            response_bytes = json.dumps(response).encode("utf-8")
            sock.sendto(response_bytes, target_addr)
            print(f"[Python] Sent ACK for step {step} to {target_addr}", flush=True)

        except socket.timeout:
            continue
        except json.JSONDecodeError as e:
            print(f"[Python] Malformed JSON received: {e}", flush=True)
            error_response = {"status": "ERROR", "error": "malformed_json"}
            sock.sendto(json.dumps(error_response).encode("utf-8"), target_addr)
        except Exception as e:
            print(f"[Python] Unexpected runtime error: {e}", flush=True)

except KeyboardInterrupt:
    pass
finally:
    sock.close()
    print("[Python] Socket closed. Exiting worker.", flush=True)