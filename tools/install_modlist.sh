#!/usr/bin/env bash
# Install a cut MAO release into a local MO2 modlist for in-game testing.
#
# The test routine (marth, 2026-07-21): after tools/release.sh cuts a version,
# deploy it into the modlist's mods/MAO/ so marth can just launch and test —
# no manual install step. The tiers/patch mod ("MAO - Requiem Tiers") is
# Synthesis OUTPUT and is left untouched; re-run Synthesis if the ladder/economy
# data format changed (it did NOT for a DLL-only or MCM-only bump).
#
# Preserves, never clobbers:
#   - meta.ini                     (MO2-managed)
#   - MCM/Settings/MAO.ini values  (live tuned settings — new keys are BACKFILLED
#                                    via merge_mcm_settings.py, existing kept)
#
# Usage: tools/install_modlist.sh <version> [modlist-root]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VER="${1:?usage: install_modlist.sh <version> [modlist-root]}"
ROOT="${2:-/mnt/gaming/modlists/custom-modlist}"
ZIP="$HERE/releases/$VER/MAO-$VER.zip"
DEST="$ROOT/mods/MAO"

[[ -f "$ZIP" ]] || { echo "ERROR: no release zip at $ZIP — cut it first (tools/release.sh $VER ...)" >&2; exit 1; }
[[ -d "$DEST" ]] || { echo "ERROR: modlist MAO mod not found at $DEST" >&2; exit 1; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
unzip -q "$ZIP" -d "$TMP"

# The live MCM Settings must survive: extract the seed aside, then drop it from
# the payload so the copy can't overwrite the player's tuned values.
SEED="$TMP/MCM/Settings/MAO.ini"
LIVE="$DEST/MCM/Settings/MAO.ini"
[[ -f "$SEED" ]] && rm -f "$SEED"

# Everything else overwrites in place; meta.ini and any *.bak are not in the zip,
# so cp leaves them alone.
cp -r "$TMP/." "$DEST/"

# Backfill any new MCM keys into the live settings without touching existing ones.
if [[ -f "$LIVE" ]]; then
    python3 "$HERE/tools/merge_mcm_settings.py" "$HERE/data/MCM/Settings/MAO.ini" "$LIVE"
else
    cp "$HERE/data/MCM/Settings/MAO.ini" "$LIVE"
    echo "seeded fresh MCM settings"
fi

DLLVER="$(strings "$DEST/SKSE/Plugins/MAO.dll" | grep -E '^1\.[0-9]+\.[0-9]+$' | head -1 || true)"
echo "installed MAO $VER into $DEST  (DLL reports ${DLLVER:-unknown})"
echo "NOTE: tiers/patch mod left as-is; re-run Synthesis only if ladder/economy data changed."
