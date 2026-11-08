# Irrigation Controller

This project provides a simple, secure irrigation control system based on a
Raspberry Pi and an MCP23017 I²C GPIO expander.

It consists of three main components:

1. **irrigationd** – the daemon (background service) that listens for commands
   over TCP and controls irrigation zones via the MCP23017.
2. **irrigationctl** – a lightweight CLI tool to send commands to the daemon.
3. **Home Assistant integration** – a separate project,
   [irrigation-ha](https://github.com/jbuck2005/irrigation-ha), that lets you
   manage zones from Home Assistant.

---

## 🔧 Build and Install

Building needs OpenSSL 1.1.1 or later (Raspberry Pi OS Bookworm ships 3.0):

```bash
sudo apt install build-essential libssl-dev
make
sudo make install
```

`make test` runs the protocol tests on any Linux machine, with a fake valve
driver and no hardware.

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

- **IRRIGATIOND_TOKEN** – required token for all commands. `make install`
  generates a random one, saves it here and prints it in a banner: copy it into
  the Home Assistant integration. It also does this for an existing file whose
  token is unset or an example value, but never changes a token you have set.
  The daemon refuses to start without a token, or with an example value.
- **IRRIGATIOND_BIND_ADDR** – set to `127.0.0.1` (default) to only accept local
  connections, or `0.0.0.0` to allow remote access.
- **IRRIGATIOND_PLAIN_TOKEN** – `no` (fresh installs) accepts only TLS, so the
  token never crosses the network. `yes` also accepts the older plain text
  `TOKEN=` lines, for clients that don't speak TLS yet, and logs a warning at
  most once an hour while they are used. When missing, as in a config file from
  before TLS, it means `yes`. See [Encryption](#-encryption).
- **IRRIGATIOND_TRUSTED_ADDR** – optional. One IPv4 address that the rate limit
  does not apply to. See [Security Notes](#-security-notes).

To show the token again:

```bash
sudo sed -n 's/^IRRIGATIOND_TOKEN=//p' /etc/default/irrigationd
```

To replace it with a new random one:

```bash
sudo sed -i -E "s|^#?[[:space:]]*IRRIGATIOND_TOKEN=.*|IRRIGATIOND_TOKEN=$(openssl rand -hex 16)|" /etc/default/irrigationd
```

Then restart the daemon and enter the new token in Home Assistant.

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

`irrigationctl` connects with TLS. It takes the token from `IRRIGATION_TOKEN`,
or from a `TOKEN=` field in the command, which it then leaves out of what it
sends. `IRRIGATION_HOST` and `IRRIGATION_PORT` pick the daemon (default
`127.0.0.1:4242`).

Turn on zone 1 for 30 seconds:

```bash
export IRRIGATION_TOKEN=your-long-random-token
irrigationctl "ZONE=1 TIME=30"
```

Turn off zone 1:

```bash
irrigationctl "ZONE=1 TIME=0"
```

`IRRIGATION_PLAIN=1` sends the command in plain text instead, for an
irrigationd from before TLS.

### Run limits

`TIME` can be at most 7200 seconds (2 hours). Sending a new run to a zone that is
already running replaces the old run, but a valve stays open for at most 2 hours
in one go: a replacement that would go past that is shortened to fit and still
gets `OK`, so use `STATUS` to see the real time left.

When a valve reaches 2 hours it closes, and new runs on that zone get
`ERR zone at maximum runtime` for the next 10 minutes. This includes a single
`TIME=7200` run that finishes normally. Stops always work, and a stop on that zone
(`TIME=0`) ends the wait, so someone can override it deliberately. The wait is not
kept across a daemon restart.

A run that ends by itself before 2 hours, or is stopped, starts no wait, so this
limits one continuous open, not total watering in a day.

### Status

Report every zone without changing any valve:

```bash
irrigationctl "STATUS"
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

### Debugging by hand

With `IRRIGATIOND_PLAIN_TOKEN=yes`, plain text still works with `nc`:

```bash
echo "STATUS TOKEN=your-long-random-token" | nc 127.0.0.1 4242
```

Over TLS, use `openssl s_client` with the key derived from the token:

```bash
KEY=$(printf 'irrigationd-psk-v1:%s' "your-long-random-token" | sha256sum | cut -d' ' -f1)
echo STATUS | openssl s_client -quiet -connect 127.0.0.1:4242 -tls1_3 \
  -ciphersuites TLS_AES_128_GCM_SHA256 -psk "$KEY" -psk_identity irrigationd
```

---

## 🏠 Home Assistant Integration

The integration lives in its own repository,
[irrigation-ha](https://github.com/jbuck2005/irrigation-ha). Follow the
installation steps there, then set it up from the Home Assistant UI. There is
nothing to add to `configuration.yaml`.

1. Go to **Settings → Devices & Services → Add Integration** and choose
   **Irrigation Controller**.
2. Enter the **Host** and **Port** of the machine running `irrigationd`
   (port 4242 by default).
3. Enter the number of **Zones** and a default run time.
4. Enter the **Token**, which must match `IRRIGATIOND_TOKEN`.

If Home Assistant runs on a different machine, set `IRRIGATIOND_BIND_ADDR` so
that `irrigationd` accepts remote connections. See [Security Notes](#-security-notes).

---

## 🔐 Encryption

Clients connect with TLS 1.3 and use the token as a pre-shared key, so the
token itself never crosses the network, and commands and replies can't be
read, changed or replayed. There are no certificates to manage.

- Both ends derive a 32 byte key: SHA-256 of `irrigationd-psk-v1:` followed by
  the token. The PSK identity is `irrigationd`, and the only cipher suite is
  `TLS_AES_128_GCM_SHA256`.
- Each handshake also does a fresh key exchange, so a token that leaks later
  can't decrypt traffic captured earlier.
- TLS and plain text share port 4242: a TLS connection always starts with the
  byte `0x16`, which no plain text command does.
- Inside TLS the commands and replies are the same lines as before, and
  `TOKEN=` is not needed.
- A handshake with the wrong token fails, and counts as a failed login for the
  rate limit below.

### Upgrading an existing install

Nothing stops working at any step, and the token stays the same.

1. Update and reinstall irrigationd. Your existing `/etc/default/irrigationd`
   has no `IRRIGATIOND_PLAIN_TOKEN`, so plain text keeps working.
2. Update the Home Assistant integration. It switches to TLS by itself.
3. Add `IRRIGATIOND_PLAIN_TOKEN=no` to `/etc/default/irrigationd` and run
   `sudo systemctl restart irrigationd`.

Until step 3, the journal shows a warning while anything still logs in with
the token in plain text.

---

## 🔒 Security Notes

- Always configure a strong `IRRIGATIOND_TOKEN`.
- Keep `IRRIGATIOND_BIND_ADDR=127.0.0.1` unless you explicitly need remote access.
- Use a firewall if exposing the service beyond localhost.
- Set `IRRIGATIOND_PLAIN_TOKEN=no` once every client uses TLS.
- Home Assistant integration must be configured with the same token.
- After 10 connections from one IP fail to authenticate within 60 seconds, each
  further connection from it waits 1 second before its token is checked, and at
  most 2 may be open at once. A valid token is still accepted and clears the count.
  Clients with the right token are not rate limited.
- That limit can refuse a valid Stop in one case: when the same IP is sending bad
  tokens over several connections at once. To rule that out, set
  `IRRIGATIOND_TRUSTED_ADDR` to the address of your Home Assistant host. Only use
  an address that already has the token, because guesses from it are not limited.

---
