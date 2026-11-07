# Irrigation Controller

This project provides a simple, secure irrigation control system based on a
Raspberry Pi and an MCP23017 I²C GPIO expander.

It consists of three main components:

1. **irrigationd** – the daemon (background service) that listens for commands
   over TCP and controls irrigation zones via the MCP23017.
2. **irrigationctl** – a lightweight CLI tool to send commands to the daemon.
3. **Home Assistant integration** – Python code that allows zones to be managed
   from Home Assistant as switches.

---

## 🔧 Build and Install

```bash
make
sudo make install
```

This installs:

- `/usr/local/bin/irrigationd` – the daemon
- `/usr/local/bin/irrigationctl` – the CLI tool
- `/etc/systemd/system/irrigationd.service` – systemd unit
- `/etc/default/irrigationd` – configuration file (token and bind address)

---

## ⚙️ Configuration

Edit `/etc/default/irrigationd`:

```bash
IRRIGATIOND_TOKEN=your-long-random-token
IRRIGATIOND_BIND_ADDR=127.0.0.1
```

- **IRRIGATIOND_TOKEN** – required token for all commands. Generate your own with `openssl rand -hex 16`; never use the example value.
- **IRRIGATIOND_BIND_ADDR** – set to `127.0.0.1` (default) to only accept local
  connections, or `0.0.0.0` to allow remote access.

Reload systemd and enable the service:

```bash
sudo systemctl daemon-reload
sudo systemctl enable irrigationd
sudo systemctl start irrigationd
```

Check logs:

```bash
sudo journalctl -u irrigationd -f
```

---

## 🖥️ Usage

### CLI Tool

Turn on zone 1 for 30 seconds:

```bash
irrigationctl "ZONE=1 TIME=30 TOKEN=your-long-random-token"
```

Turn off zone 1:

```bash
irrigationctl "ZONE=1 TIME=0 TOKEN=your-long-random-token"
```

### Status

Report every zone without changing any valve:

```bash
irrigationctl "STATUS TOKEN=your-long-random-token"
```

The reply is one line, at most 182 bytes including the newline:

```text
OK STATUS V=1 ZONES=14 Z1=OFF:0 Z2=ON:28 Z3=OFF:0 ... Z14=OFF:0
```

Fields are separated by single spaces:

- `OK STATUS` – a status reply. A rejected request gets the usual `ERR ...` line.
- `V=1` – format version. It changes only if an existing field changes meaning.
  New fields may be added to the end of the line, so ignore any you don't know.
- `ZONES=14` – number of zones. Every zone from 1 to this number is listed, in order.
- `Zn=STATE:SECONDS` – zone `n`. `STATE` is `ON` or `OFF`. `SECONDS` is the time
  left in the current run, rounded up, and is always `0` when `OFF`.

`STATE` is what the daemon last set, not a reading from the valve. If a valve
fails to close, its zone stays `ON` with `SECONDS` at `1` or `0` while the daemon
keeps retrying.

### Netcat (debugging)

```bash
echo "ZONE=1 TIME=10 TOKEN=your-long-random-token" | nc 127.0.0.1 4242
```

---

## 🏠 Home Assistant Integration

A custom integration is provided under `custom_components/irrigation/`. Copy
this directory into your Home Assistant `custom_components` folder.

Update your `configuration.yaml`:

```yaml
switch:
  - platform: irrigation
    host: 127.0.0.1
    port: 4242
    token: your-long-random-token
    zones:
      - 1
      - 2
      - 3
```

Restart Home Assistant. Each zone will appear as a switch.

---

## 🔒 Security Notes

- Always configure a strong `IRRIGATIOND_TOKEN`.
- Keep `IRRIGATIOND_BIND_ADDR=127.0.0.1` unless you explicitly need remote access.
- Use a firewall if exposing the service beyond localhost.
- Home Assistant integration must be configured with the same token.

---
