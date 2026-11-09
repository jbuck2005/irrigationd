#!/bin/sh
# Creates the "irrigation" system user that irrigationd.service runs as, and
# lets it use the I2C bus. Called by "make install". Without it the service
# won't start, and the tempting fix of deleting User= would run it as root.
set -eu

# Skipped when installing into a staging directory, such as for a package
if [ -n "${DESTDIR:-}" ]; then
	exit 0
fi

if ! id irrigation >/dev/null 2>&1; then
	useradd --system --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin irrigation
	echo "Created the irrigation system user"
fi

if getent group i2c >/dev/null 2>&1; then
	usermod -a -G i2c irrigation
else
	echo "No i2c group: enable I2C (sudo raspi-config) and run make install again" >&2
fi
