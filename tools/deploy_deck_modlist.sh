#!/usr/bin/env bash
# Deploy a cut MAO release into an MO2 modlist ON THE STEAM DECK over ssh.
#
# The active test modlist is Tuxborn on the Deck (marth, 2026-07-28). This is the
# MO2 analog of tools/install_modlist.sh; the older tools/deploy_deck.sh does a
# FLAT non-MO2 install into a bare Skyrim and is for a different target — do not
# confuse them.
#
# Preserves, never clobbers:
#   - meta.ini                     (MO2-managed)
#   - MCM/Settings/MAO.ini values  (live tuned settings — new keys BACKFILLED via
#                                    merge_mcm_settings.py, existing kept; the
#                                    merge runs LOCALLY so it needs no deck python)
# Leaves any Synthesis-output mod (MAO - Patch.esp / mao_tiers.json) untouched.
#
# Usage: tools/deploy_deck_modlist.sh <version> [modlist=Tuxbornrc1] [host=deck@marthdeck]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VER="${1:?usage: deploy_deck_modlist.sh <version> [modlist] [host]}"
LIST="${2:-Tuxbornrc1}"
HOST="${3:-deck@marthdeck}"
ZIP="$HERE/releases/$VER/MAO-$VER.zip"
DEST="Games/$LIST/mods/MAO"   # relative to the deck user's home

[[ -f "$ZIP" ]] || { echo "ERROR: no release zip at $ZIP — cut it first (tools/release.sh $VER ...)" >&2; exit 1; }
ssh "$HOST" "[ -d \"\$HOME/$DEST\" ]" || { echo "ERROR: ~/$DEST not found on $HOST" >&2; exit 1; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
unzip -q "$ZIP" -d "$TMP"
rm -f "$TMP/MCM/Settings/MAO.ini"   # never overwrite live settings

# Push the payload (all but the live Settings). No --delete: meta.ini, the live
# Settings, fonts, and anything MO2 manages stay in place.
rsync -a "$TMP/" "$HOST:$DEST/"

# Backfill new MCM keys into the deck's live Settings without touching tuned ones.
LIVE="$TMP/live-settings.ini"
if scp -q "$HOST:$DEST/MCM/Settings/MAO.ini" "$LIVE" 2>/dev/null; then
    python3 "$HERE/tools/merge_mcm_settings.py" "$HERE/data/MCM/Settings/MAO.ini" "$LIVE"
    scp -q "$LIVE" "$HOST:$DEST/MCM/Settings/MAO.ini"
else
    scp -q "$HERE/data/MCM/Settings/MAO.ini" "$HOST:$DEST/MCM/Settings/MAO.ini"
    echo "seeded fresh MCM settings on deck"
fi

DLLVER="$(ssh "$HOST" "strings '$DEST/SKSE/Plugins/MAO.dll' | grep -E '^1\.[0-9]+\.[0-9]+\$' | head -1" || true)"
echo "deployed MAO $VER to $HOST:~/$DEST  (DLL reports ${DLLVER:-unknown})"
echo "NOTE: economy needs current Synthesis output (mao_tiers.json) in the modlist;"
echo "      re-run Synthesis only if the ladder/economy data format changed."
