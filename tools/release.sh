#!/usr/bin/env bash
# Cut an immutable, exe-free (Synthesis-only) MAO release.
# (Ported from MEO's release_synthesis.sh — the proven release shape.)
#
# The download ships NO binary tool — the install-time patcher is a Synthesis
# patcher added from GitHub (see assets/MAO.synth), so the release is: the mod
# zip (DLL + ESP + MCM + fonts) plus the static MAO.synth onboarding file. The
# DLL is pulled from a green `native` CI run (CommonLibSSE-NG only builds on
# Windows/MSVC). Rules inherited from MEO:
#   - releases/ is IMMUTABLE: bump the version instead of re-cutting.
#   - NEVER cut before the version's last generator/DLL commit — generated
#     text and code math are one contract (MEO shipped lying tooltips twice).
#   - the version in the zip comes from kPluginVersion via the generator's MCM
#     stamp; pass the SAME version here or the gate below refuses.
#
# Usage: tools/release.sh <version> "desc" [--run <native-run-id>]
set -euo pipefail
cd "$(dirname "$0")/.."

VER="${1:?usage: tools/release.sh <version> [description] [--run <id>]}"; shift
DESC=""; RUN_ID=""
if [[ $# -gt 0 && "$1" != --* ]]; then DESC="$1"; shift; fi
while [[ $# -gt 0 ]]; do
    case "$1" in
        --run) RUN_ID="${2:?--run needs an id}"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

DEST="releases/$VER"; ZIP="$DEST/MAO-$VER.zip"
[[ -e "$DEST" ]] && { echo "ERROR: $DEST already exists. Releases are immutable; bump or clear it." >&2; exit 1; }

# The version being cut must BE the version in the code (one source of truth).
CODEVER=$(grep -oP 'kPluginVersion\s*=\s*"\K[0-9]+\.[0-9]+\.[0-9]+' native/plugin.cpp)
[[ "$VER" == "$CODEVER" || "$VER" == "v$CODEVER" ]] || {
    echo "ERROR: cutting '$VER' but native/plugin.cpp says kPluginVersion=$CODEVER" >&2; exit 1; }

# Resolve the CI run that built the DLL.
if [[ -z "$RUN_ID" ]]; then
    RUN_ID=$(gh run list --workflow=native.yml --status success --limit 1 --json databaseId -q '.[0].databaseId')
    [[ -n "$RUN_ID" ]] || { echo "ERROR: no successful 'native' run found." >&2; exit 1; }
fi
# The DLL must be built from THIS commit — the ESP/MCM below are regenerated from
# the working tree, so a mismatched DLL reships the "generated text vs code math"
# drift (MEO's lying-tooltip incident class). Gate on the run's head SHA == HEAD.
RUN_SHA=$(gh run view "$RUN_ID" --json headSha -q '.headSha' 2>/dev/null)
HEAD_SHA=$(git rev-parse HEAD)
if [[ -n "$RUN_SHA" && "$RUN_SHA" != "$HEAD_SHA" ]]; then
    echo "ERROR: native run $RUN_ID built ${RUN_SHA:0:7}, but HEAD is ${HEAD_SHA:0:7}." >&2
    echo "       Push HEAD, wait for its green build, and pass --run <that id>." >&2
    exit 1
fi
echo "== native DLL from run $RUN_ID (${RUN_SHA:0:7}) =="

STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/SKSE/Plugins/MAO/fonts"
gh run download "$RUN_ID" -n MAO-dll -D "$STAGE/SKSE/Plugins"
[[ -f "$STAGE/SKSE/Plugins/MAO.dll" ]] || { echo "ERROR: artifact had no MAO.dll" >&2; exit 1; }
echo "DLL: $(stat -c '%s bytes' "$STAGE/SKSE/Plugins/MAO.dll")"

# Regenerate all DLL-adjacent content fresh (complete standalone mod every time;
# the generator also re-stamps the MCM version from kPluginVersion).
echo "== regenerate ESP + stamped MCM config =="
python3 MAO_GenerateESP.py "$STAGE/esp"
[[ -f data/Scripts/MAO_MCM.pex ]] || { echo "ERROR: no MAO_MCM.pex (tools/compile.sh MAO_MCM once)" >&2; exit 1; }

cp "$STAGE/esp/MAO.esp" "$STAGE/MAO.esp"; rm -rf "$STAGE/esp"
cp data/SKSE/Plugins/MAO.ini "$STAGE/SKSE/Plugins/MAO.ini"
cp data/SKSE/Plugins/MAO/fonts/head.ttf data/SKSE/Plugins/MAO/fonts/body.ttf \
   data/SKSE/Plugins/MAO/fonts/sans.ttf "$STAGE/SKSE/Plugins/MAO/fonts/"
mkdir -p "$STAGE/MCM/Config/MAO" "$STAGE/MCM/Settings" "$STAGE/Scripts"
cp data/MCM/Config/MAO/config.json "$STAGE/MCM/Config/MAO/"
cp data/MCM/Settings/MAO.ini "$STAGE/MCM/Settings/"
cp data/Scripts/MAO_MCM.pex "$STAGE/Scripts/"

cp assets/MAO-README.txt "$STAGE/MAO-README.txt"           # in-zip readme (Nexus users)
cp assets/MAO.synth      "$STAGE/MAO.synth"                # the Synthesis onboarding file

mkdir -p "$STAGE/fomod"
cat > "$STAGE/fomod/info.xml" <<EOF
<fomod>
  <Name>marth's Alchemy Overhaul</Name>
  <Author>marth</Author>
  <Version>$VER</Version>
  <Website>https://github.com/marthofdoom/MAO</Website>
  <Description>Flask-based alchemy: gather essence, configure flasks, coat weapons. Adapt the perk tree + ingredient tiers to your load order with the MAO Synthesis patcher (see MAO.synth).</Description>
</fomod>
EOF

# Completeness gate: refuse an incomplete release.
for req in "SKSE/Plugins/MAO.dll" "SKSE/Plugins/MAO.ini" \
           "SKSE/Plugins/MAO/fonts/head.ttf" "SKSE/Plugins/MAO/fonts/body.ttf" \
           "SKSE/Plugins/MAO/fonts/sans.ttf" "MAO.esp" \
           "MCM/Config/MAO/config.json" "MCM/Settings/MAO.ini" \
           "Scripts/MAO_MCM.pex" "fomod/info.xml" "MAO-README.txt" "MAO.synth"; do
    [[ -f "$STAGE/$req" ]] || { echo "ERROR: release incomplete — missing $req" >&2; exit 1; }
done

mkdir -p "$DEST"
( cd "$STAGE" && zip -qr - . ) > "$ZIP"
cp assets/MAO.synth "$DEST/MAO.synth"          # static Synthesis onboarding file
printf '%s\n' "$VER" > "$DEST/VERSION"
{ [[ -n "$DESC" ]] && printf '%s\n' "$DESC"; printf 'built from native run %s\n' "$RUN_ID"; } > "$DEST/NOTES.txt"

echo "== manifest (MO2 installs this as Data/) =="; unzip -l "$ZIP"
echo; echo "Wrote $ZIP  +  $DEST/MAO.synth"
echo "Tag with:  git tag -a $VER -m \"${DESC:-$VER}\" && git push origin $VER"
