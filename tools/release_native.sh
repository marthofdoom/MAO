#!/usr/bin/env bash
# Cut an immutable, MO2-installable MAO release around the CI-built DLL.
#
# MAO.dll builds only on Windows CI (CommonLibSSE-NG needs the MSVC ABI), so
# this pulls it from a successful `native` run and wraps it in the MO2 archive
# layout: the zip root IS the virtual Data folder, so MO2 installs it with no
# manual file placement. Releases are immutable — refuses to overwrite an
# existing releases/<version>/.
#
# Usage: tools/release_native.sh <version> "description" [--run <id>]
# Example: tools/release_native.sh v0.1.0-m1 "M1 gathering loop"
#
# P0 releases ship DLL + MAO.ini. Once M2 adds MAO_GenerateESP.py, extend this
# to regenerate + package MAO.esp alongside (see the sibling MEO script).
set -euo pipefail
cd "$(dirname "$0")/.."

VER="${1:?usage: tools/release_native.sh <version> [description] [--run <id>]}"
shift
DESC=""
RUN_ID=""
if [[ $# -gt 0 && "$1" != --* ]]; then DESC="$1"; shift; fi
while [[ $# -gt 0 ]]; do
    case "$1" in
        --run) RUN_ID="${2:?--run needs an id}"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

DEST="releases/$VER"
ZIP="$DEST/MAO-$VER.zip"
if [[ -e "$DEST" ]]; then
    echo "ERROR: $DEST already exists. Releases are immutable; bump the version." >&2
    exit 1
fi

if [[ -z "$RUN_ID" ]]; then
    RUN_ID=$(gh run list --workflow=native.yml --status success --limit 1 \
             --json databaseId -q '.[0].databaseId')
    [[ -n "$RUN_ID" ]] || { echo "ERROR: no successful 'native' run. Push native/ and let CI go green first." >&2; exit 1; }
fi
echo "== native DLL from run $RUN_ID =="

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/SKSE/Plugins"

gh run download "$RUN_ID" -n MAO-dll -D "$STAGE/SKSE/Plugins"
[[ -f "$STAGE/SKSE/Plugins/MAO.dll" ]] || { echo "ERROR: artifact had no MAO.dll" >&2; exit 1; }
echo "DLL: $(stat -c '%s bytes' "$STAGE/SKSE/Plugins/MAO.dll")"

# Ship the seed INI next to the DLL (harmless if the user already has one; MO2
# lets them keep their edited copy on reinstall).
cp data/SKSE/Plugins/MAO.ini "$STAGE/SKSE/Plugins/MAO.ini"

# Menu fonts (the DLL bakes them at init; skins expect them).
mkdir -p "$STAGE/SKSE/Plugins/MAO/fonts"
cp data/SKSE/Plugins/MAO/fonts/body.ttf data/SKSE/Plugins/MAO/fonts/head.ttf \
   data/SKSE/Plugins/MAO/fonts/sans.ttf data/SKSE/Plugins/MAO/fonts/OFL-*.txt \
   "$STAGE/SKSE/Plugins/MAO/fonts/"

# Regenerate MAO.esp fresh (deterministic, never hand-edited) at the zip root
# (= virtual Data/). Every release is a complete standalone mod.
python3 MAO_GenerateESP.py "$STAGE/espgen" >/dev/null
[[ -f "$STAGE/espgen/MAO.esp" ]] || { echo "ERROR: MAO_GenerateESP.py produced no MAO.esp" >&2; exit 1; }
mv "$STAGE/espgen/MAO.esp" "$STAGE/MAO.esp"; rmdir "$STAGE/espgen"

mkdir -p "$STAGE/fomod"
cat > "$STAGE/fomod/info.xml" <<EOF
<fomod>
  <Name>Marth's Alchemy Overhaul</Name>
  <Author>Marth</Author>
  <Version>$VER</Version>
  <Website>https://github.com/marthofdoom/MAO</Website>
  <Description>Flask-based alchemy: potions become permanent essence-charged Flasks.</Description>
</fomod>
EOF

for req in "SKSE/Plugins/MAO.dll" "SKSE/Plugins/MAO.ini" "MAO.esp" "fomod/info.xml" \
           "SKSE/Plugins/MAO/fonts/body.ttf" "SKSE/Plugins/MAO/fonts/head.ttf"; do
    [[ -f "$STAGE/$req" ]] || { echo "ERROR: release incomplete — missing $req" >&2; exit 1; }
done

mkdir -p "$DEST"
( cd "$STAGE" && zip -qr - . ) > "$ZIP"
printf '%s\n' "$VER" > "$DEST/VERSION"
[[ -n "$DESC" ]] && printf '%s\n' "$DESC" > "$DEST/NOTES.txt"
printf 'built from native run %s\n' "$RUN_ID" >> "$DEST/NOTES.txt"

echo "== manifest (MO2 installs this as Data/) =="
unzip -l "$ZIP"
echo
echo "Wrote $ZIP"
echo "Tag with:  git tag -a $VER -m \"${DESC:-$VER}\" && git push origin $VER"
