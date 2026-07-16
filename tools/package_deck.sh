#!/usr/bin/env bash
# Package MAO as a mod-manager-ready zip and drop it in the Deck's Projects
# folder (~/Games/Projects) for marth to install into the modlist. Replaces the
# flat-install deploy_deck.sh workflow now that the Deck runs a managed modlist.
#
# Usage: tools/package_deck.sh [--run <native-run-id>] [--host <user@host>] [--no-upload]
# Defaults: latest successful `native` run; host deck@marthdeck.
#
# MEO release-cutter shape: pull the CI-built DLL, regenerate ESP + version-
# stamped MCM config fresh from HEAD, stage the complete mod, gate on a
# required-file list, zip as MAO-v<version>.zip (version read from
# kPluginVersion in native/plugin.cpp — the single source of truth).
#
# NOT included: mao_tiers.json (per-load-order; generate with the installer's
# write-tiers against the modlist once it settles) and MAO.ini user edits
# (the zip ships the seed; the mod manager owns changes from there).
set -euo pipefail
cd "$(dirname "$0")/.."

RUN_ID=""
HOST="deck@marthdeck"
UPLOAD=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --run)  RUN_ID="${2:?--run needs an id}"; shift 2 ;;
        --host) HOST="${2:?--host needs user@host}"; shift 2 ;;
        --no-upload) UPLOAD=0; shift ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

VER=$(grep -oP 'kPluginVersion\s*=\s*"\K[0-9]+\.[0-9]+\.[0-9]+' native/plugin.cpp)
[[ -n "$VER" ]] || { echo "ERROR: could not read kPluginVersion from native/plugin.cpp" >&2; exit 1; }
ZIP="MAO-v$VER.zip"
echo "== packaging MAO v$VER =="

if [[ -z "$RUN_ID" ]]; then
    RUN_ID=$(gh run list --workflow=native.yml --status success --limit 1 \
             --json databaseId -q '.[0].databaseId')
    [[ -n "$RUN_ID" ]] || { echo "ERROR: no successful 'native' run. Let CI go green first." >&2; exit 1; }
fi

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
MOD="$STAGE/mod"
mkdir -p "$MOD/SKSE/Plugins/MAO/fonts" "$MOD/MCM/Config/MAO" "$MOD/MCM/Settings" "$MOD/Scripts"

# DLL from CI (MSVC-only build).
gh run download "$RUN_ID" -n MAO-dll -D "$STAGE/dl"
[[ -f "$STAGE/dl/MAO.dll" ]] || { echo "ERROR: artifact had no MAO.dll" >&2; exit 1; }
cp "$STAGE/dl/MAO.dll" "$MOD/SKSE/Plugins/MAO.dll"
[[ -f "$STAGE/dl/MAO.pdb" ]] && cp "$STAGE/dl/MAO.pdb" "$MOD/SKSE/Plugins/MAO.pdb" || true

# ESP + version-stamped MCM config, fresh from HEAD (the generator stamps
# data/MCM/Config/MAO/config.json as a side effect).
python3 MAO_GenerateESP.py "$STAGE/esp" >/dev/null
cp "$STAGE/esp/MAO.esp" "$MOD/MAO.esp"

cp data/MCM/Config/MAO/config.json      "$MOD/MCM/Config/MAO/config.json"
cp data/MCM/Settings/MAO.ini            "$MOD/MCM/Settings/MAO.ini"
cp data/Scripts/MAO_MCM.pex             "$MOD/Scripts/MAO_MCM.pex"
cp data/SKSE/Plugins/MAO.ini            "$MOD/SKSE/Plugins/MAO.ini"
cp data/SKSE/Plugins/MAO/fonts/body.ttf data/SKSE/Plugins/MAO/fonts/head.ttf \
   data/SKSE/Plugins/MAO/fonts/sans.ttf "$MOD/SKSE/Plugins/MAO/fonts/"

# Completeness gate (MEO practice): refuse an incomplete zip.
for req in MAO.esp SKSE/Plugins/MAO.dll SKSE/Plugins/MAO.ini \
           SKSE/Plugins/MAO/fonts/body.ttf SKSE/Plugins/MAO/fonts/head.ttf \
           SKSE/Plugins/MAO/fonts/sans.ttf MCM/Config/MAO/config.json \
           MCM/Settings/MAO.ini Scripts/MAO_MCM.pex; do
    [[ -f "$MOD/$req" ]] || { echo "ERROR: staged mod is missing $req — refusing to zip" >&2; exit 1; }
done

( cd "$MOD" && zip -qr "$STAGE/$ZIP" . )
echo "staged: $(unzip -l "$STAGE/$ZIP" | tail -1 | awk '{print $1" bytes, "$2" files"}') (CI run $RUN_ID)"

if [[ "$UPLOAD" == 1 ]]; then
    ssh -o BatchMode=yes "$HOST" "mkdir -p /home/deck/Games/Projects"
    scp -q "$STAGE/$ZIP" "$HOST:/home/deck/Games/Projects/$ZIP"
    echo "uploaded: $HOST:/home/deck/Games/Projects/$ZIP"
else
    cp "$STAGE/$ZIP" "releases-local-$ZIP" 2>/dev/null || cp "$STAGE/$ZIP" "./$ZIP"
    echo "kept locally: ./$ZIP"
fi
