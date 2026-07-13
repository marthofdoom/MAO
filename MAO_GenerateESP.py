#!/usr/bin/env python3
"""Generate MAO.esp byte-by-byte (never hand-edited).

P1a — minimal ESP: the "Open Field Kit" lesser power (SPEL) + one inert MGEF
it carries. That's all P1a needs — the power exists so the DLL can grant it and
detect its cast (TESSpellCastEvent) to open the Field Kit, replacing the P0
hotkey. MAO is native-first: NO Papyrus, NO startup quest — the DLL grants the
power itself. Casting applies the inert MGEF (does nothing); the cast event is
the whole point.

Byte recipes are ported verbatim from the sibling MEO generator, which is
proven in-game. The one principle: when a record misbehaves, dump it and diff
subrecords against a working vanilla twin — never trust this docstring.

FormIDs are FROZEN. The ESP is ESL-flagged (TES4 flag 0x200), so locals live in
0x800-0xFFF; co-saves and the DLL reference them by (localID, "MAO.esp").
Forms are only ever ADDED here, never renumbered or deleted.

Usage: python3 MAO_GenerateESP.py [out_dir]   (default: out/)
"""
import os
import struct
import sys
from io import BytesIO

# ── FormIDs (single master Skyrim.esm -> own file index 0x01) ──
OWN = 0x01000000
FREF_EQUP_VOICE = 0x00025BEE  # Skyrim.esm EQUP "Voice" (lesser-power slot)

FID_FIELDKIT_MGEF  = OWN | 0x800  # FROZEN — inert effect the power carries
FID_FIELDKIT_SPELL = OWN | 0x801  # FROZEN — the Open Field Kit lesser power
NEXT_OBJECT_ID     = 0x802        # first unused local; grows as forms are added

FORM_VERSION = 44

# ── binary helpers (byte-for-byte valid; forked from MEO/MRO) ──
def subrec(tag, data):
    return tag.encode('ascii') + struct.pack('<H', len(data)) + data

def record(tag, fid, flags, data):
    return (tag.encode('ascii') + struct.pack('<I', len(data)) + struct.pack('<I', flags)
            + struct.pack('<I', fid) + struct.pack('<I', 0) + struct.pack('<H', FORM_VERSION)
            + struct.pack('<H', 0) + data)

def group(label, data):
    return b'GRUP' + struct.pack('<I', 24 + len(data)) + label.encode('ascii') \
        + struct.pack('<iII', 0, 0, 0) + data

def zstr(s):
    return s.encode('ascii') + b'\x00'


def make_tes4(next_id):
    hedr = struct.pack('<f', 1.70) + struct.pack('<I', 100) + struct.pack('<I', next_id)
    body = subrec('HEDR', hedr) + subrec('CNAM', zstr("Marth")) \
        + subrec('SNAM', zstr("Marth's Alchemy Overhaul"))
    body += subrec('MAST', zstr("Skyrim.esm")) + subrec('DATA', struct.pack('<Q', 0))
    return record('TES4', 0, 0x00000200, body)  # 0x200 = ESL-flagged


# ── Inert self MGEF (Script archetype, no VMAD, no visible effect). The power
# needs at least one effect to be a valid spell; this one does nothing — the
# native TESSpellCastEvent sink is what reacts to the cast. Layout copied from
# MEO's proven inert "marker" MGEF (self delivery). ──
def mgef_inert_self_data():
    d = bytearray(152)
    struct.pack_into('<I', d, 0, 0x8000)       # flags: no-hit-effect / inert
    struct.pack_into('<I', d, 12, 0xFFFFFFFF)  # (no) menu display object
    struct.pack_into('<I', d, 16, 0xFFFFFFFF)
    struct.pack_into('<I', d, 64, 1)           # archetype: Script
    struct.pack_into('<i', d, 68, -1)
    struct.pack_into('<I', d, 80, 1)           # casting: Constant (self)
    struct.pack_into('<I', d, 84, 0)           # delivery: Self
    struct.pack_into('<i', d, 88, -1)
    struct.pack_into('<f', d, 112, 1.0)
    return bytes(d)

def make_mgef():
    body = subrec('EDID', zstr("MAO_FieldKitMGEF")) + subrec('FULL', zstr("")) \
        + subrec('MDOB', struct.pack('<I', 0)) \
        + subrec('DATA', mgef_inert_self_data()) + subrec('SNDD', b'') \
        + subrec('DNAM', struct.pack('<I', 0))
    return group('MGEF', record('MGEF', FID_FIELDKIT_MGEF, 0, body))


# ── The Open Field Kit lesser power (SPIT type 3, self, Voice slot). ──
def spit_lesser_power():
    return struct.pack('<fIIfIIffI', 0.0, 0, 3, 0.0, 1, 0, 0.0, 0.0, 0)

def make_spel():
    body = subrec('EDID', zstr("MAO_OpenFieldKit")) + subrec('OBND', b'\x00' * 12) \
        + subrec('FULL', zstr("Open Field Kit"))
    body += subrec('MDOB', struct.pack('<I', 0)) + subrec('ETYP', struct.pack('<I', FREF_EQUP_VOICE))
    body += subrec('DESC', zstr("Open your Field Kit to view essence stores and configure flasks."))
    body += subrec('SPIT', spit_lesser_power()) \
        + subrec('EFID', struct.pack('<I', FID_FIELDKIT_MGEF)) \
        + subrec('EFIT', struct.pack('<fII', 0.0, 0, 0))
    return group('SPEL', record('SPEL', FID_FIELDKIT_SPELL, 0, body))


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "out"
    os.makedirs(out_dir, exist_ok=True)
    esp = BytesIO()
    esp.write(make_tes4(NEXT_OBJECT_ID))
    esp.write(make_mgef())
    esp.write(make_spel())
    data = esp.getvalue()
    path = os.path.join(out_dir, "MAO.esp")
    with open(path, 'wb') as f:
        f.write(data)
    print(f"Written: {path} ({len(data):,} bytes)")
    print("  MGEF x1 (inert self)   SPEL x1 (Open Field Kit lesser power)")
    print(f"  FROZEN FormIDs: MGEF 0x{FID_FIELDKIT_MGEF & 0xFFFFFF:03X}  "
          f"SPEL 0x{FID_FIELDKIT_SPELL & 0xFFFFFF:03X}  (ESL-flagged, master Skyrim.esm)")


if __name__ == "__main__":
    main()
