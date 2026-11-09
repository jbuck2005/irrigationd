#!/bin/sh
# Installs /etc/default/irrigationd and makes sure it holds a token.
# Called by "make install". A token that is already set is never changed,
# because Home Assistant is using it.
set -eu

CONF="${DESTDIR:-}/etc/default/irrigationd"

if [ ! -f "$CONF" ]; then
	install -m 600 irrigationd.default "$CONF"
	echo "Installed default /etc/default/irrigationd"
fi
# Also for a file that's already there, which may hold a token
chmod 600 "$CONF"

current=$(sed -n 's/^IRRIGATIOND_TOKEN=//p' "$CONF" | tail -n 1)
case "$current" in
	""|CHANGE_ME_to_a_long_random_token|your-long-random-token) ;;
	*)
		echo "Keeping the token already set in /etc/default/irrigationd"
		exit 0
		;;
esac

# 16 random bytes as 32 hex characters, the same as: openssl rand -hex 16
token=$(od -An -tx1 -N16 /dev/urandom | tr -d ' \n')
if [ ${#token} -ne 32 ]; then
	echo "Could not generate a token. Set IRRIGATIOND_TOKEN in /etc/default/irrigationd yourself." >&2
	exit 1
fi

if grep -Eq '^#?[[:space:]]*IRRIGATIOND_TOKEN=' "$CONF"; then
	sed -i -E "s|^#?[[:space:]]*IRRIGATIOND_TOKEN=.*|IRRIGATIOND_TOKEN=$token|" "$CONF"
else
	echo "IRRIGATIOND_TOKEN=$token" >> "$CONF"
fi
chmod 600 "$CONF"

cat <<EOF

================================================================================
  A new irrigationd token was generated and saved in /etc/default/irrigationd:

      $token

  Copy it now and paste it into the Token field of the irrigation-ha
  integration in Home Assistant. Commands without it are refused.

  To show it again later:
      sudo sed -n 's/^IRRIGATIOND_TOKEN=//p' /etc/default/irrigationd
================================================================================

EOF
