#!/usr/bin/env python3
"""Black-box conformance test for watchdogs-sx1262d's fake backend."""

from __future__ import annotations

import json
import os
import pwd
import signal
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


def stop_process(process: subprocess.Popen) -> str:
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)
    return process.stdout.read()


def test_meshtastic_broker_startup(binary: Path) -> None:
    """Boot the real daemon through the fake broker at a 30 dBm region."""
    with tempfile.TemporaryDirectory(prefix="sx1262-broker-daemon-") as temporary:
        root = Path(temporary)
        manager_config = root / "sx1262.yaml"
        manager_config.write_text("Lora:\n  Module: sim\n", encoding="utf-8")
        broker_socket = root / "sx1262d.sock"
        daemon_socket = root / "wdg.sock"
        daemon_config = root / "config.yaml"
        daemon_config.write_text(
            "Lora:\n"
            "  Module: broker\n"
            f"  BrokerSocket: {broker_socket}\n"
            "General:\n"
            "  MACAddress: '02:00:00:00:00:01'\n",
            encoding="utf-8",
        )
        current = pwd.getpwuid(os.getuid())
        manager = next(
            account for account in map(pwd.getpwnam, ("root", "nobody"))
            if account.pw_uid != current.pw_uid)
        manager_environment = os.environ.copy()
        manager_environment["WATCHDOGS_SX1262_SOCKET"] = str(broker_socket)
        manager_environment["WATCHDOGS_SX1262_FORCED_OFF"] = str(root / "forced-off")
        manager_environment["WATCHDOGS_SX1262_TEST_MESHTASTIC_USER"] = current.pw_name
        manager_environment["WATCHDOGS_SX1262_TEST_MANAGER_USER"] = manager.pw_name
        broker = subprocess.Popen(
            [str(binary), "--sx1262-manager-fake", f"--config={manager_config}",
             f"--fsdir={root / 'manager-fs'}"],
            env=manager_environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        daemon = None
        broker_output = ""
        daemon_output = ""
        try:
            deadline = time.monotonic() + 10
            while not broker_socket.exists() and time.monotonic() < deadline:
                if broker.poll() is not None:
                    raise AssertionError(broker.stdout.read())
                time.sleep(0.02)
            assert broker_socket.exists(), "startup-test broker socket was not created"

            daemon_environment = os.environ.copy()
            daemon_environment["MESHTASTIC_WDG_SOCKET"] = str(daemon_socket)
            daemon_environment["MESHTASTIC_WDG_ALLOWED_UID"] = str(os.getuid())
            daemon_environment["MESHTASTIC_WDG_DISABLE_BLUETOOTH"] = "1"
            daemon = subprocess.Popen(
                [str(binary), "--port=0", f"--config={daemon_config}",
                 f"--fsdir={root / 'daemon-fs'}"],
                env=daemon_environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            deadline = time.monotonic() + 10
            while not daemon_socket.exists() and time.monotonic() < deadline:
                if daemon.poll() is not None:
                    daemon_output = daemon.stdout.read()
                    raise AssertionError(
                        "broker-backed daemon exited before health readiness:\n"
                        + daemon_output)
                time.sleep(0.02)
            assert daemon_socket.exists(), "broker-backed daemon health socket was not created"
            assert daemon.poll() is None, "broker-backed daemon exited after readiness"
        finally:
            if daemon is not None and daemon.poll() is None:
                daemon_output = stop_process(daemon)
            broker_output = stop_process(broker) if broker.poll() is None else broker.stdout.read()

        assert daemon.returncode in (0, -signal.SIGTERM), (
            f"broker-backed daemon exited with status {daemon.returncode}:\n{daemon_output}"
        )
        assert "Final Tx power: 22 dBm" in daemon_output, daemon_output
        assert "broker init success" in daemon_output, daemon_output
        assert "rejected configure_phy" not in daemon_output, daemon_output
        assert broker.returncode == 0, (
            f"startup-test broker exited with status {broker.returncode}:\n{broker_output}"
        )


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test-sx1262-broker.py MESHTASTICD_BINARY")
    binary = Path(sys.argv[1]).resolve()
    test_role_authentication(binary)
    test_meshtastic_broker_startup(binary)
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
