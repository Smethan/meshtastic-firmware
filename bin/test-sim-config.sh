#!/usr/bin/env bash
set -euo pipefail

binary=${1:-.pio/build/native-wdg/meshtasticd}
if [[ ! -x $binary ]]; then
    echo "native binary is missing: $binary" >&2
    exit 2
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cat >"$work/config.yaml" <<'YAML'
Lora:
  Module: sx1262
General:
  MaxNodes: 137
  ConfigDirectory: ""
YAML

output=$(
    MESHTASTIC_WDG_DISABLE_BLUETOOTH=1 \
        "$binary" -s --config="$work/config.yaml" --output-yaml
)
grep -q 'Module: sim' <<<"$output"
grep -q 'MaxNodes: 137' <<<"$output"
