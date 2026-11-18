#!/bin/sh
# Removes ws-pull: sudo sh uninstall.sh
# Keeps the settings (/etc/ws-pull) and the archive (/var/lib/ws-pull);
# with --purge those go too, along with the ws-pull user.
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Run it with sudo: sudo sh uninstall.sh [--purge]" >&2
    exit 1
fi

systemctl disable --now ws-pull.timer 2>/dev/null || true
systemctl stop ws-pull.service 2>/dev/null || true
rm -f /etc/systemd/system/ws-pull.service /etc/systemd/system/ws-pull.timer
systemctl daemon-reload
rm -rf /opt/ws-pull

if [ "${1:-}" = "--purge" ]; then
    rm -rf /etc/ws-pull /var/lib/ws-pull
    if id ws-pull >/dev/null 2>&1; then
        userdel ws-pull
    fi
    echo "Removed, with the settings, the archive and the ws-pull user."
else
    echo "Removed. Kept: /etc/ws-pull (settings, password) and /var/lib/ws-pull (the archive)."
    echo "Everything: sudo sh uninstall.sh --purge"
fi
