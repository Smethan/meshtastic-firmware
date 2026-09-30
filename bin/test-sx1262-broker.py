#!/usr/bin/env python3
"""Black-box conformance test for watchdogs-sx1262d's fake backend."""

from __future__ import annotations

import json
import os
import pwd
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def send(sock: socket.socket, message: dict) -> None:
    raw = json.dumps(message, separators=(",", ":")).encode()
    assert len(raw) <= 4096
    sock.sendall(raw)


def receive(sock: socket.socket, *, event: str | None = None) -> dict:
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        message = json.loads(sock.recv(4097))
        if event is None or message.get("event") == event:
            return message
    raise AssertionError(f"timed out waiting for {event}")


def hello(path: Path, role: str) -> tuple[socket.socket, int]:
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    sock.settimeout(5)
    sock.connect(str(path))
    send(sock, {
        "type": "hello", "request_id": 1,
        "api": {"major": 1, "minor": 0}, "role": role,
    })
    reply = receive(sock)
    assert reply["ok"], reply
    assert reply["result"]["api"]["major"] == 1
    return sock, reply["result"]["generation"]


def request(sock: socket.socket, generation: int, request_id: int,
            operation: str, **arguments) -> dict:
    send(sock, {
        "type": "request", "request_id": request_id,
        "generation": generation, "op": operation, **arguments,
    })
    while True:
        reply = receive(sock)
        if reply.get("type") == "response" and reply.get("request_id") == request_id:
            return reply


def test_role_authentication(binary: Path) -> None:
    """Exercise distinct NSS lookups; getpwnam storage may be reused."""
    with tempfile.TemporaryDirectory(prefix="sx1262-broker-auth-") as temporary:
        root = Path(temporary)
        config = root / "sx1262.yaml"
        config.write_text("Lora:\n  Module: sim\n", encoding="utf-8")
        sock_path = root / "sx1262d.sock"
        current = pwd.getpwuid(os.getuid())
        manager = next(
            pwd.getpwnam(name) for name in ("root", "nobody")
            if pwd.getpwnam(name).pw_uid != current.pw_uid)
        environment = os.environ.copy()
        environment["WATCHDOGS_SX1262_SOCKET"] = str(sock_path)
        environment["WATCHDOGS_SX1262_FORCED_OFF"] = str(root / "forced-off")
        environment["WATCHDOGS_SX1262_TEST_MESHTASTIC_USER"] = current.pw_name
        environment["WATCHDOGS_SX1262_TEST_MANAGER_USER"] = manager.pw_name
        process = subprocess.Popen(
            [str(binary), "--sx1262-manager-fake", f"--config={config}",
             f"--fsdir={root / 'fs'}"],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            deadline = time.monotonic() + 10
            while not sock_path.exists() and time.monotonic() < deadline:
                if process.poll() is not None:
                    raise AssertionError(process.stdout.read())
                time.sleep(0.02)
            assert sock_path.exists(), "credential-test broker socket was not created"

            meshtastic, _generation = hello(sock_path, "meshtastic")
            meshtastic.close()

            disallowed = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            disallowed.settimeout(5)
            disallowed.connect(str(sock_path))
            send(disallowed, {
                "type": "hello", "request_id": 2,
                "api": {"major": 1, "minor": 0}, "role": "controller",
            })
            rejected = receive(disallowed)
            assert not rejected["ok"], rejected
            assert rejected["error"]["code"] == "unauthorized_role", rejected
            disallowed.close()
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        output = process.stdout.read()
        assert process.returncode == 0, (
            f"credential-test broker exited with status {process.returncode}:\n{output}"
        )


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test-sx1262-broker.py MESHTASTICD_BINARY")
    binary = Path(sys.argv[1]).resolve()
    test_role_authentication(binary)
    with tempfile.TemporaryDirectory(prefix="sx1262-broker-") as temporary:
        root = Path(temporary)
        config = root / "sx1262.yaml"
        config.write_text("Lora:\n  Module: sim\n", encoding="utf-8")
        sock_path = root / "sx1262d.sock"
        forced_off = root / "forced-off"
        environment = os.environ.copy()
        environment["WATCHDOGS_SX1262_SOCKET"] = str(sock_path)
        environment["WATCHDOGS_SX1262_FORCED_OFF"] = str(forced_off)
        process = subprocess.Popen(
            [str(binary), "--sx1262-manager-fake", f"--config={config}",
             f"--fsdir={root / 'fs'}"],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            deadline = time.monotonic() + 10
            while not sock_path.exists() and time.monotonic() < deadline:
                if process.poll() is not None:
                    raise AssertionError(process.stdout.read())
                time.sleep(0.02)
            assert sock_path.exists(), "broker socket was not created"

            meshtastic, generation = hello(sock_path, "meshtastic")
            assert receive(meshtastic, event="lease_granted")["generation"] == generation
            meshcore, mesh_generation = hello(sock_path, "meshcore")
            controller, controller_generation = hello(sock_path, "controller")
            assert generation == mesh_generation == controller_generation

            status = request(controller, generation, 2, "get_status")
            assert status["result"]["state"] == "MESHTASTIC"

            switch = request(controller, generation, 3, "activate_mode", mode="meshcore")
            assert switch["ok"] and switch["result"]["pending"]
            revoke = receive(meshtastic, event="prepare_revoke")
            quiesced = request(meshtastic, revoke["generation"], 2, "quiesced")
            assert quiesced["ok"]
            granted = receive(meshcore, event="lease_granted")
            generation = granted["generation"]
            assert generation > controller_generation

            stale = request(controller, controller_generation, 4, "heartbeat")
            assert not stale["ok"] and stale["error"]["code"] == "stale_generation"
            heartbeat = request(controller, generation, 5, "heartbeat")
            assert heartbeat["ok"]

            # A missed controller lease must use the same quiescence barrier
            # as an explicit mode switch so the old GATT frontend cannot
            # overlap Meshtastic's frontend.
            time.sleep(5.2)
            timed_out = receive(meshcore, event="prepare_revoke")
            assert request(
                meshcore, timed_out["generation"], 2, "quiesced")["ok"]
            recovered = receive(meshtastic, event="lease_granted")
            generation = recovered["generation"]
            status = request(controller, generation, 6, "get_status")
            assert status["result"]["state"] == "MESHTASTIC"

            powered_off = request(controller, generation, 7, "admin_power_off")
            assert powered_off["ok"]
            generation = powered_off["generation"]
            assert powered_off["result"]["state"] == "OFF"
            assert forced_off.is_file()

            powered_on = request(controller, generation, 8, "admin_power_on", mode="meshtastic")
            assert powered_on["ok"]
            assert powered_on["result"]["state"] == "MESHTASTIC"
            assert not forced_off.exists()
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        output = process.stdout.read()
        assert process.returncode == 0, (
            f"broker exited with status {process.returncode}:\n{output}"
        )
    print("SX1262 broker conformance checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
