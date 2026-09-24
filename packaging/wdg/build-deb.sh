#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
TAG=${1:-${GITHUB_REF_NAME:-}}
BINARY=${2:-"$ROOT/.pio/build/native-wdg/meshtasticd"}
OUT_DIR=${3:-"$ROOT/dist"}

if [[ ! $TAG =~ ^v([0-9]+)\.([0-9]+)\.([0-9]+)-wdg\.([0-9]+)$ ]]; then
    echo "release tag must match vMAJOR.MINOR.PATCH-wdg.N" >&2
    exit 2
fi
if [[ ! -f $BINARY || ! -x $BINARY ]]; then
    echo "native-wdg binary is missing or not executable: $BINARY" >&2
    exit 2
fi
if [[ $(dpkg --print-architecture) != arm64 ]]; then
    echo "WDG release packages must be built natively for arm64" >&2
    exit 2
fi

PACKAGE_VERSION="${BASH_REMATCH[1]}.${BASH_REMATCH[2]}.${BASH_REMATCH[3]}+wdg${BASH_REMATCH[4]}"
ASSET="meshtasticd-wdg_${PACKAGE_VERSION}_arm64.deb"
UPSTREAM_REPOSITORY=$(sed -n 's/^upstream_repository=//p' "$ROOT/UPSTREAM_BASE")
UPSTREAM_TAG=$(sed -n 's/^upstream_tag=//p' "$ROOT/UPSTREAM_BASE")
UPSTREAM_COMMIT=$(sed -n 's/^upstream_commit=//p' "$ROOT/UPSTREAM_BASE")

[[ $UPSTREAM_REPOSITORY == meshtastic/firmware ]]
[[ $UPSTREAM_TAG == v2.8.* ]]
[[ $UPSTREAM_COMMIT =~ ^[0-9a-f]{40}$ ]]

MAX_GLIBC=$(readelf --version-info "$BINARY" \
    | sed -n 's/.*Name: GLIBC_\([0-9][0-9.]*\).*/\1/p' \
    | sort -V | tail -n 1)
if [[ -z $MAX_GLIBC ]] || ! dpkg --compare-versions "$MAX_GLIBC" le 2.36; then
    echo "binary requires glibc ${MAX_GLIBC:-unknown}; Bookworm limit is 2.36" >&2
    exit 2
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/stage"
mkdir -p \
    "$STAGE/DEBIAN" \
    "$STAGE/usr/lib/meshtasticd-wdg" \
    "$STAGE/usr/lib/systemd/system" \
    "$STAGE/usr/lib/tmpfiles.d" \
    "$STAGE/usr/lib/sysusers.d" \
    "$STAGE/usr/share/dbus-1/system.d" \
    "$STAGE/usr/share/meshtasticd-wdg" \
    "$STAGE/usr/share/doc/meshtasticd-wdg"

install -m 0755 "$BINARY" "$STAGE/usr/lib/meshtasticd-wdg/meshtasticd"
install -m 0644 "$ROOT/packaging/wdg/meshtasticd-wdg.service" \
    "$STAGE/usr/lib/systemd/system/meshtasticd-wdg.service"
install -m 0644 "$ROOT/packaging/wdg/meshtasticd-wdg.tmpfiles" \
    "$STAGE/usr/lib/tmpfiles.d/meshtasticd-wdg.conf"
install -m 0644 "$ROOT/packaging/wdg/meshtasticd-wdg.sysusers" \
    "$STAGE/usr/lib/sysusers.d/meshtasticd-wdg.conf"
install -m 0644 "$ROOT/packaging/wdg/meshtasticd-wdg-dbus.conf" \
    "$STAGE/usr/share/dbus-1/system.d/meshtasticd-wdg.conf"
install -m 0644 "$ROOT/packaging/wdg/wdg-portduino.example.yaml" \
    "$STAGE/usr/share/meshtasticd-wdg/wdg-portduino.example.yaml"
install -m 0644 "$ROOT/packaging/wdg/copyright" \
    "$STAGE/usr/share/doc/meshtasticd-wdg/copyright"
install -m 0644 "$ROOT/UPSTREAM_BASE" \
    "$STAGE/usr/share/doc/meshtasticd-wdg/UPSTREAM_BASE"
install -m 0755 "$ROOT/packaging/wdg/postinst" "$STAGE/DEBIAN/postinst"

SHLIBS=$(dpkg-shlibdeps -O -e"$BINARY" 2>"$WORK/shlib-errors" \
    | sed -n 's/^shlibs:Depends=//p') || {
    cat "$WORK/shlib-errors" >&2
    exit 2
}
if [[ -z $SHLIBS ]]; then
    echo "dpkg-shlibdeps did not report runtime dependencies" >&2
    cat "$WORK/shlib-errors" >&2
    exit 2
fi

cat >"$STAGE/DEBIAN/control" <<EOF
Package: meshtasticd-wdg
Version: $PACKAGE_VERSION
Architecture: arm64
Maintainer: Smethan <noreply@github.com>
Section: net
Priority: optional
Depends: adduser, bluez, dbus, util-linux, $SHLIBS
Homepage: https://github.com/Smethan/meshtastic-firmware
Description: Meshtastic daemon with WatchDogsGo local API and BlueZ phone transport
 Side-by-side Meshtastic Portduino build for the uConsole AIO v2 radio.
EOF

python3 - "$TAG" "$PACKAGE_VERSION" "$ASSET" "$UPSTREAM_TAG" "$UPSTREAM_COMMIT" \
    >"$STAGE/usr/share/doc/meshtasticd-wdg/compatibility.json" <<'PY'
import json, sys
tag, version, asset, upstream_tag, upstream_commit = sys.argv[1:]
print(json.dumps({
    "format": 1,
    "repository": "Smethan/meshtastic-firmware",
    "tag": tag,
    "upstream_repository": "meshtastic/firmware",
    "upstream_tag": upstream_tag,
    "upstream_commit": upstream_commit,
    "wdg_api": {"major": 1, "minor": 0},
    "package": {
        "name": "meshtasticd-wdg",
        "version": version,
        "architecture": "arm64",
        "asset": asset,
    },
    "minimum_glibc": "2.36",
    "artifact_digest_source": "GitHub release compatibility.json",
}, indent=2, sort_keys=True))
PY

mkdir -p "$OUT_DIR"
dpkg-deb --root-owner-group --build "$STAGE" "$OUT_DIR/$ASSET"

SIZE=$(stat -c %s "$OUT_DIR/$ASSET")
SHA256=$(sha256sum "$OUT_DIR/$ASSET" | awk '{print $1}')
python3 - "$TAG" "$PACKAGE_VERSION" "$ASSET" "$SIZE" "$SHA256" \
    "$UPSTREAM_TAG" "$UPSTREAM_COMMIT" "$MAX_GLIBC" \
    >"$OUT_DIR/compatibility.json" <<'PY'
import json, sys
tag, version, asset, size, sha256, upstream_tag, upstream_commit, glibc = sys.argv[1:]
print(json.dumps({
    "format": 1,
    "repository": "Smethan/meshtastic-firmware",
    "tag": tag,
    "upstream_repository": "meshtastic/firmware",
    "upstream_tag": upstream_tag,
    "upstream_commit": upstream_commit,
    "wdg_api": {"major": 1, "minor": 0},
    "package": {
        "name": "meshtasticd-wdg",
        "version": version,
        "architecture": "arm64",
        "asset": asset,
        "size": int(size),
        "sha256": sha256,
    },
    "minimum_glibc": "2.36",
    "built_glibc_requirement": glibc,
}, indent=2, sort_keys=True))
PY

(
    cd "$OUT_DIR"
    sha256sum "$ASSET" compatibility.json >SHA256SUMS
)

dpkg-deb --info "$OUT_DIR/$ASSET"
dpkg-deb --contents "$OUT_DIR/$ASSET"
echo "$OUT_DIR/$ASSET"
