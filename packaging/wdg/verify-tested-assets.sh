#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 3 ]; then
    echo "usage: verify-tested-assets.sh DRAFT_DIR TESTED_DIR DEB_BASENAME" >&2
    exit 2
fi

DRAFT_DIR=$1
TESTED_DIR=$2
DEB=$3

[[ $DEB =~ ^meshtasticd-wdg_[0-9]+\.[0-9]+\.[0-9]+\+wdg[0-9]+_arm64\.deb$ ]]

ASSETS=(SHA256SUMS SOURCE.txt compatibility.json copyright "$DEB")
CHECKSUM_ASSETS=(SOURCE.txt compatibility.json copyright "$DEB")

validate_directory() {
    local directory=$1 count asset names expected_names
    [ -d "$directory" ] && [ ! -L "$directory" ]

    # Count every directory entry recursively.  This rejects nested paths,
    # FIFOs, devices, sockets, and extra symlinks instead of silently ignoring
    # them with a top-level `find -type f` query.
    count=$(find "$directory" -mindepth 1 -printf x | wc -c)
    [ "$count" -eq "${#ASSETS[@]}" ]
    for asset in "${ASSETS[@]}"; do
        [ -f "$directory/$asset" ]
        [ ! -L "$directory/$asset" ]
    done

    names=$(sed -n 's/^[0-9a-f]\{64\}  \([A-Za-z0-9._+-]\+\)$/\1/p' \
        "$directory/SHA256SUMS" | sort)
    expected_names=$(printf '%s\n' "${CHECKSUM_ASSETS[@]}" | sort)
    [ "$names" = "$expected_names" ]
    [ "$(wc -l <"$directory/SHA256SUMS")" -eq "${#CHECKSUM_ASSETS[@]}" ]
}

validate_directory "$DRAFT_DIR"
validate_directory "$TESTED_DIR"

# Establish byte identity with the immutable, successful Actions artifact
# before interpreting the draft's checksum file.
for asset in "${ASSETS[@]}"; do
    cmp "$DRAFT_DIR/$asset" "$TESTED_DIR/$asset"
done

for directory in "$DRAFT_DIR" "$TESTED_DIR"; do
    (cd "$directory" && sha256sum --strict --check SHA256SUMS)
done
