#!/bin/sh
# Installs ws-pull, or updates it: sudo sh install.sh [display URL]
#   e.g. sudo sh install.sh http://192.168.1.50
# Run it from this folder's copy. An update keeps the settings, the password
# and the archive; a URL given replaces the one in the settings.
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Run it with sudo: sudo sh install.sh [display URL]" >&2
    exit 1
fi
here=$(cd "$(dirname "$0")" && pwd)
url=${1:-}
if [ -n "$url" ] && ! printf '%s' "$url" | grep -Eq '^https?://[A-Za-z0-9.:_-]+(/[A-Za-z0-9._/-]*)?$'; then
    echo "The URL should look like http://192.168.1.50 or http://esp32flight.local" >&2
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is missing" >&2
    exit 1
fi
if ! python3 -c 'import sys; sys.exit(sys.version_info < (3, 10))'; then
    echo "ws-pull needs Python 3.10 or newer, this is $(python3 --version)" >&2
    exit 1
fi

# its own system user, without a login
if ! id ws-pull >/dev/null 2>&1; then
    useradd --system --user-group --home-dir /var/lib/ws-pull --no-create-home \
        --shell /usr/sbin/nologin --comment "weather station puller" ws-pull
fi

# the script belongs to root, so the service can't change its own code
install -d -m 0755 /opt/ws-pull
install -m 0755 "$here/ws_pull.py" /opt/ws-pull/ws_pull.py
install -m 0644 "$here/README.md" /opt/ws-pull/README.md

install -d -m 0755 /etc/ws-pull
if [ ! -e /etc/ws-pull/ws-pull.conf ]; then
    install -m 0644 "$here/ws-pull.conf.example" /etc/ws-pull/ws-pull.conf
fi
if [ -n "$url" ]; then
    sed -i "s|^url *=.*|url = $url|" /etc/ws-pull/ws-pull.conf
fi
if [ ! -e /etc/ws-pull/password ]; then
    install -m 0600 /dev/null /etc/ws-pull/password
fi
chown root:root /etc/ws-pull/password
chmod 0600 /etc/ws-pull/password

install -m 0644 "$here/ws-pull.service" "$here/ws-pull.timer" /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now ws-pull.timer

echo
echo "Installed. The display: $(grep -E '^url *=' /etc/ws-pull/ws-pull.conf)"
if [ ! -s /etc/ws-pull/password ]; then
    echo "If the display's web panel has a password: sudo nano /etc/ws-pull/password (just the password)"
fi
echo "First run now:  sudo systemctl start ws-pull"
echo "Its log:        journalctl -u ws-pull -n 20"
echo "Next runs:      systemctl list-timers ws-pull.timer"
