#!/usr/bin/env python3
"""End to end tests for irrigationd's network protocol, run by "make test".

Starts the test build of irrigationd (a fake valve driver, a spare port) and
talks to it with irrigationctl and with plain sockets. Needs no hardware and
no network beyond 127.0.0.1. The Python ssl check, which is how Home Assistant
connects, runs only on Python 3.13 or later.

Usage: test_protocol.py IRRIGATIOND IRRIGATIONCTL PORT
"""

import hashlib
import os
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time

DAEMON, CTL, PORT = sys.argv[1], sys.argv[2], int(sys.argv[3])
TOKEN = "test-token-0123456789abcdef"
HOST = "127.0.0.1"

failures = []


def check(name, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else f": {detail}"))
    if not ok:
        failures.append(name)


class Daemon:
    """One irrigationd process with the given extra environment."""

    def __init__(self, **env):
        self.log = tempfile.TemporaryFile(mode="w+")
        full_env = {
            "PATH": os.environ.get("PATH", ""),
            "IRRIGATIOND_TOKEN": TOKEN,
            "IRRIGATIOND_BIND_ADDR": HOST,
        }
        full_env.update(env)
        self.proc = subprocess.Popen([DAEMON], env=full_env, stderr=self.log)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            try:
                socket.create_connection((HOST, PORT), timeout=1).close()
                return
            except OSError:
                time.sleep(0.05)
        self.stop()
        raise RuntimeError("irrigationd did not start:\n" + self.output())

    def output(self):
        self.log.seek(0)
        return self.log.read()

    def stop(self):
        self.proc.terminate()
        self.proc.wait(timeout=5)


def ctl(command, token=None, plain=False):
    env = {"PATH": os.environ.get("PATH", ""), "IRRIGATION_HOST": HOST, "IRRIGATION_PORT": str(PORT)}
    if token is not None:
        env["IRRIGATION_TOKEN"] = token
    if plain:
        env["IRRIGATION_PLAIN"] = "1"
    result = subprocess.run([CTL, command], env=env, capture_output=True, text=True, timeout=20)
    return result.returncode, result.stdout.strip(), result.stderr.strip()


def plain_line(line):
    with socket.create_connection((HOST, PORT), timeout=10) as sock:
        sock.sendall(line.encode() + b"\n")
        data = b""
        while not data.endswith(b"\n"):
            chunk = sock.recv(1024)
            if not chunk:
                break
            data += chunk
    return data.decode().strip()


def psk_key(token):
    return hashlib.sha256(b"irrigationd-psk-v1:" + token.encode()).digest()


def python_tls(line, token):
    """Connect the way the Home Assistant integration does."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    ctx.set_psk_client_callback(lambda hint: ("irrigationd", psk_key(token)))
    with socket.create_connection((HOST, PORT), timeout=10) as raw:
        with ctx.wrap_socket(raw) as sock:
            sock.sendall(line.encode() + b"\n")
            data = b""
            while not data.endswith(b"\n"):
                chunk = sock.recv(1024)
                if not chunk:
                    break
                data += chunk
            return data.decode().strip(), sock.version(), sock.cipher()[0]


def test_tls_and_plain_allowed():
    daemon = Daemon()
    try:
        rc, out, err = ctl("STATUS", token=TOKEN)
        check("TLS STATUS works", rc == 0 and "OK STATUS V=1 ZONES=14" in out, out + err)

        rc, out, err = ctl("ZONE=2 TIME=30", token=TOKEN)
        check("TLS ZONE command works", rc == 0 and out == "Response: OK", out + err)
        rc, out, _ = ctl("STATUS", token=TOKEN)
        check("TLS STATUS shows the run", " Z2=ON:" in out, out)

        rc, out, err = ctl(f"ZONE=2 TIME=0 TOKEN={TOKEN}")
        check("irrigationctl takes TOKEN= from the command", rc == 0 and out == "Response: OK", out + err)

        rc, out, err = ctl("STATUS", token="wrong-token")
        check("TLS with a wrong token is refused", rc != 0 and "OK" not in out, out + err)

        reply = plain_line(f"STATUS TOKEN={TOKEN}")
        check("plain text still works when allowed", reply.startswith("OK STATUS V=1"), reply)
        reply = plain_line("STATUS TOKEN=wrong-token")
        check("plain text with a wrong token is refused", reply == "ERR auth required", reply)

        rc, out, err = ctl("STATUS", token=TOKEN, plain=True)
        check("IRRIGATION_PLAIN=1 sends plain text", rc == 0 and "OK STATUS" in out, out + err)

        if hasattr(ssl.SSLContext, "set_psk_client_callback"):
            reply, version, cipher = python_tls("STATUS", TOKEN)
            check("Python ssl (Home Assistant's way) connects",
                  reply.startswith("OK STATUS V=1") and version == "TLSv1.3"
                  and cipher == "TLS_AES_128_GCM_SHA256", f"{reply} {version} {cipher}")
            try:
                python_tls("STATUS", "wrong-token")
                check("Python ssl with a wrong token is refused", False, "connected")
            except (ssl.SSLError, OSError):
                check("Python ssl with a wrong token is refused", True)
        else:
            print("SKIP Python ssl checks: needs Python 3.13 or later")

        # Failed handshakes count toward the rate limit, and the right token
        # still gets through afterwards
        for _ in range(11):
            ctl("STATUS", token="wrong-token")
        rc, out, err = ctl("STATUS", token=TOKEN)
        check("right token works after many failed handshakes", rc == 0 and "OK STATUS" in out, out + err)
        log = daemon.output()
        check("failed handshakes are logged", "Rejected TLS connection" in log, log[-500:])
        check("plain text use is warned about once",
              log.count("token received in plain text") == 1, log[-500:])
    finally:
        daemon.stop()


def test_plain_disabled():
    for value in ("no", "maybe"):
        daemon = Daemon(IRRIGATIOND_PLAIN_TOKEN=value)
        try:
            reply = plain_line(f"STATUS TOKEN={TOKEN}")
            check(f"IRRIGATIOND_PLAIN_TOKEN={value} refuses plain text",
                  reply == "ERR plain text disabled, use TLS", reply)
            rc, out, err = ctl("STATUS", token=TOKEN)
            check(f"IRRIGATIOND_PLAIN_TOKEN={value} still accepts TLS", rc == 0 and "OK STATUS" in out, out + err)
        finally:
            daemon.stop()


def test_token_never_sent():
    """irrigationctl over TLS must not put the token on the wire."""
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((HOST, PORT))
    listener.listen(1)
    received = bytearray()

    def capture():
        conn, _ = listener.accept()
        conn.settimeout(3)
        try:
            while True:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                received.extend(chunk)
        except OSError:
            pass
        conn.close()

    thread = threading.Thread(target=capture)
    thread.start()
    ctl(f"ZONE=1 TIME=30 TOKEN={TOKEN}")
    thread.join(timeout=10)
    listener.close()
    check("irrigationctl sends a TLS handshake", received[:1] == b"\x16", bytes(received[:16]))
    check("the token never appears on the wire", TOKEN.encode() not in received, "token found")
    check("the key never appears on the wire", psk_key(TOKEN) not in received, "key found")


test_tls_and_plain_allowed()
test_plain_disabled()
test_token_never_sent()

print()
if failures:
    print(f"{len(failures)} failed: " + ", ".join(failures))
    sys.exit(1)
print("All protocol tests passed")
