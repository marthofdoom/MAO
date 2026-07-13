#!/usr/bin/env bash
# Deploy the CI-built MAO.dll (+ MAO.ini) FLAT into the Steam Deck's Skyrim SE
# install over SSH. The Deck runs a plain (non-MO2) install, so files drop
# straight into Data/SKSE/Plugins — no archive, no mod manager.
#
# Usage: tools/deploy_deck.sh [--run <id>] [--host <user@host>]
# Defaults: latest successful `native` run; host deck@marthdeck.
#
# The DLL only builds on Windows CI (MSVC ABI), so this pulls the artifact with
# gh, then scp's it to the Deck. MAO.ini is only copied if absent, so your
# on-Deck edits (hotkey/gamepad button) survive a redeploy — delete it there to
# reset. Prints the SKSE log path to tail after launching.
set -euo pipefail
cd "$(dirname "$0")/.."

RUN_ID=""
HOST="deck@marthdeck"
GAME="/home/deck/.local/share/Steam/steamapps/common/Skyrim Special Edition"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --run)  RUN_ID="${2:?--run needs an id}"; shift 2 ;;
        --host) HOST="${2:?--host needs user@host}"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

PLUGINS="$GAME/Data/SKSE/Plugins"

if [[ -z "$RUN_ID" ]]; then
    RUN_ID=$(gh run list --workflow=native.yml --status success --limit 1 \
             --json databaseId -q '.[0].databaseId')
    [[ -n "$RUN_ID" ]] || { echo "ERROR: no successful 'native' run. Let CI go green first." >&2; exit 1; }
fi
echo "== MAO.dll from native run $RUN_ID -> $HOST =="

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
gh run download "$RUN_ID" -n MAO-dll -D "$STAGE"
[[ -f "$STAGE/MAO.dll" ]] || { echo "ERROR: artifact had no MAO.dll" >&2; exit 1; }
echo "DLL: $(stat -c '%s bytes' "$STAGE/MAO.dll")"

# Confirm the target exists before writing anything.
ssh -o BatchMode=yes "$HOST" "test -d '$PLUGINS'" \
    || { echo "ERROR: $PLUGINS not found on $HOST" >&2; exit 1; }

scp -q "$STAGE/MAO.dll" "$HOST:$PLUGINS/MAO.dll"
[[ -f "$STAGE/MAO.pdb" ]] && scp -q "$STAGE/MAO.pdb" "$HOST:$PLUGINS/MAO.pdb" || true

# Seed the INI only if the Deck doesn't already have one (preserve edits).
if ssh -o BatchMode=yes "$HOST" "test ! -f '$PLUGINS/MAO.ini'"; then
    scp -q data/SKSE/Plugins/MAO.ini "$HOST:$PLUGINS/MAO.ini"
    echo "seeded MAO.ini"
else
    echo "MAO.ini already on Deck — left as-is (delete it there to reset)"
fi

echo "== deployed =="
ssh -o BatchMode=yes "$HOST" "ls -la '$PLUGINS/MAO.dll' '$PLUGINS/MAO.ini'"
echo
echo "Launch Skyrim (via skse64_loader / Steam), then tail the log:"
echo "  ssh $HOST \"tail -f '/home/deck/.local/share/Steam/steamapps/compatdata/489830/pfx/drive_c/users/steamuser/Documents/My Games/Skyrim Special Edition/SKSE/MAO.log'\""
