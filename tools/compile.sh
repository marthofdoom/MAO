#!/usr/bin/env bash
# Compile MAO Papyrus scripts to out/Scripts/*.pex.
# Usage: tools/compile.sh MEO_StartupQuest MEO_PouchScript   (no .psc suffix)
#        tools/compile.sh all
# Proton Hotfix wine bundles wine-mono, which system wine lacks. Import order
# matters: our Source first (stubs shadow), then SKSE64 full sources (the
# bundled Actor.psc is stripped), then PO3 and the Nemesis compiler scripts.
set -euo pipefail
cd "$(dirname "$0")/.."

PROTON="/mnt/gaming/Steam/steamapps/common/Proton Hotfix/files/bin/wine"
MONO_DATA="/mnt/gaming/Steam/steamapps/common/Proton Hotfix/files/share/wine"
NEMESIS="/mnt/gaming/modlists/LoreRim/mods/Project New Reign - Nemesis Unlimited Behavior Engine/Nemesis_Engine/Papyrus Compiler"
PAPYRUS="$NEMESIS/PapyrusCompiler.exe"
FLAGS="$NEMESIS/scripts/TESV_Papyrus_Flags.flg"
IMPORTS="$PWD/Source/Scripts"
IMPORTS+=";$PWD/Source/Stubs"   # compile-only stubs for base types absent from SKSE sources
IMPORTS+=";/mnt/gaming/modlists/LoreRim/mods/Skyrim Script Extender (SKSE64)/Scripts/Source"
IMPORTS+=";/mnt/gaming/modlists/LoreRim/mods/powerofthree's Papyrus Extender/Source/scripts"
IMPORTS+=";/mnt/gaming/modlists/LoreRim/mods/PapyrusUtil SE - Modders Scripting Utility Functions/Source/Scripts"
IMPORTS+=";$NEMESIS/scripts"

mkdir -p out/Scripts

scripts=("$@")
if [[ "${1:-}" == "all" ]]; then
    scripts=(MAO_MCM)
fi
[[ ${#scripts[@]} -gt 0 ]] || { echo "usage: tools/compile.sh <ScriptName>... | all" >&2; exit 1; }

fail=0
for s in "${scripts[@]}"; do
    out=$(WINEDATADIR="$MONO_DATA" "$PROTON" "$PAPYRUS" "Source/Scripts/$s.psc" \
        -f="$FLAGS" -i="$IMPORTS" -o="out/Scripts" 2>&1) || true
    if grep -q "1 succeeded, 0 failed" <<<"$out"; then
        echo "OK   $s"
    else
        echo "FAIL $s"
        grep -E "\.psc\(|error|Error" <<<"$out" | head -15
        fail=1
    fi
done
exit $fail
