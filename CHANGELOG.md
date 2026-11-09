# Changelog

## Unreleased

- A command with a NUL byte in it is refused with `ERR invalid character`. Before, everything after the NUL was ignored, which can change a command's meaning, such as `TIME=3<NUL>600` read as `TIME=3`

## 1.0.1 - 2026-11-09

- A command line too long for the 255-byte buffer is refused with `ERR command too long`. Before, it was cut short and the rest acted on, and a cut can change a command's meaning, such as `TIME=3600` read as `TIME=3`
- irrigationd logs a warning at startup when `IRRIGATIOND_TOKEN` is shorter than 16 characters, the minimum Home Assistant accepts for a new token. A short token could be guessed offline from one recorded TLS handshake. It still starts, so an existing install keeps watering
- `make install` runs `systemctl daemon-reload` itself, so systemd picks up an updated unit file
- The watchdog checks the MCP23017 every 30 seconds and sets it up again if it was reset or its latches changed. Before, a reset was only noticed at the next valve write, which could be hours away, with every pin an input in the meantime
- Correction to 1.0.0: the reserved connection slots keep Home Assistant in only once it has used the token in the last 15 minutes. Right after irrigationd starts, a flood from devices without the token can keep it out until `IRRIGATIOND_TRUSTED_ADDR` is set. irrigationd now logs a reminder at startup, and `make install` prints one, while it isn't set and irrigationd listens beyond this machine
- SIGHUP and SIGQUIT still close every valve, but now exit with a failure, so systemd starts the daemon again. Before, `kill -HUP` (often sent to reload a daemon) left the service stopped until someone noticed

## 1.0.0 - 2026-11-09

### Security

- Slow or idle connections can no longer lock Home Assistant out. Each connection gets 5 seconds to finish, one address can hold at most 4 connections, and one slot is always kept for the trusted address
- The last 4 connection slots are kept for `IRRIGATIOND_TRUSTED_ADDR` and for any address that used the token in the last 15 minutes, so devices without the token can't shut Home Assistant out even when `IRRIGATIOND_TRUSTED_ADDR` isn't set
- Refused connections are logged at most 10 every 10 seconds, with a count of the rest, so a flood can't wear the SD card or push the daemon's own messages out of the journal
- `irrigationctl` refuses a server that offers a certificate instead of the token's key, so it can't be fooled by an impostor. It also warns when the token is given on the command line; set `IRRIGATION_TOKEN` instead
- The systemd service is more tightly sandboxed: a read-only system, no capabilities, only the I2C device, and a limited set of system calls

### Valve safety

- SIGHUP and SIGQUIT now close every valve before the daemon exits, like SIGTERM. Before, they ended it with a valve open
- A valve that fails to close when the daemon is stopped is retried every half second for up to 30 seconds, since systemd doesn't restart a stopped service
- If the MCP23017 resets, such as on a brown-out when a solenoid starts, it's set up again before the next valve write, and each write is read back so a write the chip didn't take counts as failed and a close is retried

### Other

- `make install` creates the `irrigation` system user the service runs as and adds it to the `i2c` group, and sets an existing config file to mode 600
- `irrigationctl` says when irrigationd is busy or rate limiting, instead of reporting a wrong token
