#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD="$ROOT/packaging/wdg/build-deb.sh"
SERVICE="$ROOT/packaging/wdg/meshtasticd-wdg.service"
POSTINST="$ROOT/packaging/wdg/postinst"
TMPFILES="$ROOT/packaging/wdg/meshtasticd-wdg.tmpfiles"
VERIFY_ASSETS="$ROOT/packaging/wdg/verify-tested-assets.sh"

bash -n "$BUILD" "$POSTINST" "$VERIFY_ASSETS"
grep -q '^umask 022$' "$BUILD"
grep -q '^Conflicts=meshtasticd.service$' "$SERVICE"
grep -q 'flock -n -E 75 /run/lock/watchdogs/aio-sx1262.lock' "$SERVICE"
grep -q '^RestartPreventExitStatus=75$' "$SERVICE"
grep -q 'Smethan/meshtastic-firmware' "$BUILD"
grep -q 'Package: meshtasticd-wdg' "$BUILD"
grep -q 'Architecture: arm64' "$BUILD"
grep -q 'SOURCE.txt' "$BUILD"
grep -q 'GPL-3.0-only' "$BUILD"
grep -q '"source_ref"' "$BUILD"
grep -q 'copyright >SHA256SUMS' "$BUILD"
grep -q 'unreviewed runtime dependency' "$BUILD"
grep -qx 'd /run/lock/watchdogs 2750 root watchdogs -' "$TMPFILES"
grep -qx 'f /run/lock/watchdogs/aio-sx1262.lock 0660 root watchdogs -' "$TMPFILES"

TEST_ROOT=$(mktemp -d)
trap 'rm -rf "$TEST_ROOT"' EXIT
DEB=meshtasticd-wdg_2.8.0+wdg1_arm64.deb
for directory in draft tested; do
	mkdir "$TEST_ROOT/$directory"
	printf 'package\n' >"$TEST_ROOT/$directory/$DEB"
	printf 'source\n' >"$TEST_ROOT/$directory/SOURCE.txt"
	printf '{}\n' >"$TEST_ROOT/$directory/compatibility.json"
	printf 'GPL-3.0-only\n' >"$TEST_ROOT/$directory/copyright"
	(
		cd "$TEST_ROOT/$directory"
		sha256sum "$DEB" compatibility.json SOURCE.txt copyright >SHA256SUMS
	)
done
"$VERIFY_ASSETS" "$TEST_ROOT/draft" "$TEST_ROOT/tested" "$DEB"

# A mutable draft checksum must be rejected before it is interpreted.
printf 'tampered\n' >"$TEST_ROOT/draft/SHA256SUMS"
if "$VERIFY_ASSETS" "$TEST_ROOT/draft" "$TEST_ROOT/tested" "$DEB" \
	>/dev/null 2>&1; then
	echo "tested asset verifier accepted a mutable draft checksum" >&2
	exit 1
fi
cp "$TEST_ROOT/tested/SHA256SUMS" "$TEST_ROOT/draft/SHA256SUMS"

# Non-regular and nested entries are never part of the five-file artifact.
mkfifo "$TEST_ROOT/draft/unexpected-fifo"
if "$VERIFY_ASSETS" "$TEST_ROOT/draft" "$TEST_ROOT/tested" "$DEB" \
	>/dev/null 2>&1; then
	echo "tested asset verifier accepted an extra FIFO" >&2
	exit 1
fi

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
