#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD="$ROOT/packaging/wdg/build-deb.sh"
SERVICE="$ROOT/packaging/wdg/meshtasticd-wdg.service"
POSTINST="$ROOT/packaging/wdg/postinst"

bash -n "$BUILD" "$POSTINST"
grep -q '^Conflicts=meshtasticd.service$' "$SERVICE"
grep -q 'flock -n -E 75 /run/lock/watchdogs/aio-sx1262.lock' "$SERVICE"
grep -q '^RestartPreventExitStatus=75$' "$SERVICE"
grep -q 'Smethan/meshtastic-firmware' "$BUILD"
grep -q 'Package: meshtasticd-wdg' "$BUILD"
grep -q 'Architecture: arm64' "$BUILD"

if grep -Eq '(systemctl|service)[[:space:]]+(start|enable|disable|stop)' "$POSTINST"; then
    echo "package maintainer script must not change service state" >&2
    exit 1
fi
if grep -Eq '(^|/)usr/bin/meshtasticd([^A-Za-z0-9_-]|$)' "$BUILD"; then
    echo "package script contains the stock binary path" >&2
    exit 1
fi
if rg -n --glob '!test-package.sh' \
    'Smethan/firmware([^A-Za-z0-9_-]|$)' \
    "$ROOT/packaging" "$ROOT/UPSTREAM_BASE"; then
    echo "generic Smethan/firmware repository reference found" >&2
    exit 1
fi

echo "WDG package policy checks passed"
