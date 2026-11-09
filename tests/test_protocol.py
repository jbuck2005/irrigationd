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
import signal
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


def slow_client(source):
    """A client without the token that sends one byte now and then, to hold a slot."""
    sock = socket.create_connection((HOST, PORT), timeout=10, source_address=(source, 0))
    sock.sendall(b"\x16")
    return sock


def closed_by_daemon(sock):
    """True once the daemon has closed the connection, reading anything it sent first."""
    sock.settimeout(1)
    try:
        while True:
            if sock.recv(100) == b"":
                return True
    except OSError:
        return False


def test_slow_clients_cant_lock_out_the_trusted_host():
    """Connections without the token can't hold every slot, or hold one for long."""
    daemon = Daemon(IRRIGATIOND_TRUSTED_ADDR=HOST)
    socks = []
    try:
        # One address gets four connections, and a fifth is turned away at once
        for _ in range(4):
            socks.append(slow_client("127.0.0.3"))
        with socket.create_connection((HOST, PORT), timeout=5, source_address=("127.0.0.3", 0)) as extra:
            reply = extra.recv(100).decode().strip()
        check("a fifth connection from one address is refused", reply == "ERR server busy", reply)

        # More addresses try for every slot; one is always kept for the trusted host
        for i in range(12):
            try:
                socks.append(slow_client(f"127.0.0.{4 + i % 3}"))
            except OSError:
                pass
        time.sleep(0.5)
        started = time.monotonic()
        rc, out, err = ctl("STATUS", token=TOKEN)
        check("the trusted host gets through while others hold slots",
              rc == 0 and "OK STATUS" in out and time.monotonic() - started < 3, out + err)

        # Trickling bytes doesn't keep a connection open past its deadline
        for _ in range(4):
            time.sleep(2)
            for sock in socks:
                try:
                    sock.sendall(b"\x03")
                except OSError:
                    pass
        cut_off = sum(closed_by_daemon(sock) for sock in socks)
        check("slow connections are cut off at their deadline", cut_off == len(socks),
              f"{cut_off} of {len(socks)} closed")

        rc, out, err = ctl("STATUS", token=TOKEN)
        check("commands work again once the slow clients are cut off", rc == 0 and "OK STATUS" in out,
              out + err)
        log = daemon.output()
        check("refused connections are logged",
              "too many connections from this address" in log and "all client slots in use" in log, log[-500:])
    finally:
        for sock in socks:
            sock.close()
        daemon.stop()


def test_known_client_gets_through_without_trusted_addr():
    """With no IRRIGATIOND_TRUSTED_ADDR, a client that used the token keeps a way in."""
    daemon = Daemon()
    socks = []
    try:
        rc, out, err = ctl("STATUS", token=TOKEN)
        check("a first command works", rc == 0 and "OK STATUS" in out, out + err)
        # Unknown addresses try for every slot
        for i in range(16):
            try:
                socks.append(slow_client(f"127.0.0.{10 + i // 4}"))
            except OSError:
                pass
        time.sleep(0.5)
        with socket.create_connection((HOST, PORT), timeout=5, source_address=("127.0.0.20", 0)) as extra:
            reply = extra.recv(100).decode().strip()
        check("an unknown address is refused once the open slots are full", reply == "ERR server busy", reply)
        rc, out, err = ctl("STATUS", token=TOKEN)
        check("a client that used the token still gets through", rc == 0 and "OK STATUS" in out, out + err)
    finally:
        for sock in socks:
            sock.close()
        daemon.stop()


def test_irrigationctl_reports_busy():
    """A busy reply isn't reported as a wrong token."""
    daemon = Daemon()
    socks = []
    try:
        time.sleep(0.5)  # until the start-up check's connection has let go of its slot
        for _ in range(4):
            socks.append(slow_client(HOST))
        time.sleep(0.5)
        rc, out, err = ctl("STATUS", token=TOKEN)
        check("irrigationctl says irrigationd is busy", rc != 0 and "busy" in err and "token is wrong" not in err,
              out + err)
    finally:
        for sock in socks:
            sock.close()
        daemon.stop()


def test_refusals_are_not_all_logged():
    """A flood of refused connections writes a few lines and a count, not one line each."""
    daemon = Daemon()
    socks = []
    try:
        for _ in range(4):
            socks.append(slow_client("127.0.0.30"))
        for _ in range(200):
            try:
                with socket.create_connection((HOST, PORT), timeout=2, source_address=("127.0.0.30", 0)) as s:
                    s.recv(100)
            except OSError:
                pass
        for sock in socks:
            sock.close()
        time.sleep(11.5)  # the window ends, and its sum is written
        log = daemon.output()
    finally:
        daemon.stop()
    lines = log.count("Connection rejected")
    check("refused connections are logged at most 10 at a time", 1 <= lines <= 10, f"{lines} lines")
    check("the rest are counted", "more connections were refused and not logged" in log, log[-500:])


def test_impostor_with_a_certificate_is_refused():
    """irrigationctl doesn't accept a server that offers a certificate instead of the key."""
    with tempfile.TemporaryDirectory() as tmp:
        cert, key = os.path.join(tmp, "cert.pem"), os.path.join(tmp, "key.pem")
        subprocess.run(
            ["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
             "-nodes", "-keyout", key, "-out", cert, "-days", "1", "-subj", "/CN=impostor"],
            check=True, capture_output=True,
        )
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(cert, key)
        listener = socket.socket()
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((HOST, PORT))
        listener.listen(1)
        received = bytearray()

        def serve():
            conn, _ = listener.accept()
            conn.settimeout(5)
            try:
                with ctx.wrap_socket(conn, server_side=True) as tls:
                    received.extend(tls.recv(100))
                    tls.sendall(b"OK\n")
            except (OSError, ssl.SSLError):
                pass

        thread = threading.Thread(target=serve)
        thread.start()
        rc, out, err = ctl("ZONE=1 TIME=0", token="not-the-token")
        thread.join(timeout=10)
        listener.close()
    check("irrigationctl refuses a server with a certificate", rc != 0 and "OK" not in out, out + err)
    check("the impostor receives no command", bytes(received) == b"", bytes(received))


def test_token_on_command_line_warns():
    daemon = Daemon()
    try:
        rc, out, err = ctl(f"STATUS TOKEN={TOKEN}")
        check("a token on the command line still works", rc == 0 and "OK STATUS" in out, out + err)
        check("a token on the command line is warned about", "Set IRRIGATION_TOKEN instead" in err, err)
        rc, out, err = ctl("STATUS", token=TOKEN)
        check("IRRIGATION_TOKEN gives no warning", "Warning" not in err, err)
    finally:
        daemon.stop()

def stop_with(daemon, sig):
    daemon.proc.send_signal(sig)
    try:
        return daemon.proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        daemon.proc.kill()
        return None


def test_every_stop_signal_closes_the_valves():
    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP, signal.SIGQUIT):
        daemon = Daemon()
        try:
            rc, out, err = ctl("ZONE=1 TIME=600", token=TOKEN)
            code = stop_with(daemon, sig)
            log = daemon.output()
        finally:
            daemon.stop()
        # SIGHUP and SIGQUIT exit with a failure, so systemd starts the daemon again
        want = 0 if sig in (signal.SIGTERM, signal.SIGINT) else 1
        check(f"{sig.name} closes an open valve before exiting with {want}",
              rc == 0 and code == want and "fake valve: zone 1 OFF" in log.split("Received signal")[-1],
              f"rc={rc} exit={code}\n{log}")


def test_failed_close_at_shutdown_is_retried():
    daemon = Daemon(FAKE_FAILED_CLOSES="3")
    try:
        ctl("ZONE=1 TIME=600", token=TOKEN)
        code = stop_with(daemon, signal.SIGTERM)
        log = daemon.output()
    finally:
        daemon.stop()
    after = log.split("Received signal")[-1]
    check("a close that fails at shutdown is retried until it works",
          code == 0 and after.count("close failed") == 3 and "zone 1 OFF" in after
          and "closed on retry" in after, f"exit={code}\n{log}")


def startup_log(**env):
    """Run the daemon for a moment on an address nothing connects to, and return its log."""
    full_env = {"PATH": os.environ.get("PATH", ""), "IRRIGATIOND_TOKEN": TOKEN, **env}
    proc = subprocess.Popen([DAEMON], env=full_env, stderr=subprocess.PIPE, text=True)
    time.sleep(0.5)
    proc.terminate()
    return proc.communicate(timeout=10)[1]


def test_trusted_addr_reminder():
    reminder = "IRRIGATIOND_TRUSTED_ADDR is not set"
    check("a reminder is logged when listening beyond this machine without a trusted address",
          reminder in startup_log(IRRIGATIOND_BIND_ADDR="127.0.0.2"))
    check("no reminder with a trusted address set",
          reminder not in startup_log(IRRIGATIOND_BIND_ADDR="127.0.0.2", IRRIGATIOND_TRUSTED_ADDR="192.0.2.10"))
    check("no reminder when listening only on 127.0.0.1",
          reminder not in startup_log(IRRIGATIOND_BIND_ADDR=HOST))


test_tls_and_plain_allowed()
test_plain_disabled()
test_token_never_sent()
test_slow_clients_cant_lock_out_the_trusted_host()
test_known_client_gets_through_without_trusted_addr()
test_refusals_are_not_all_logged()
test_irrigationctl_reports_busy()
test_impostor_with_a_certificate_is_refused()
test_token_on_command_line_warns()
test_every_stop_signal_closes_the_valves()
test_failed_close_at_shutdown_is_retried()
test_trusted_addr_reminder()

print()
if failures:
    print(f"{len(failures)} failed: " + ", ".join(failures))
    sys.exit(1)
print("All protocol tests passed")
