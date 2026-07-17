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
import json
import os
import re
import struct
import sys
from io import BytesIO


def read_mao_version(default="0.0.0"):
    """The single source of truth for the version is kPluginVersion in
    native/plugin.cpp; read it so the build-stamped MCM readout can never drift
    from the DLL/log/console string. Falls back to a placeholder if unreadable.
    (Ported from MEO's read_meo_version — the live-Papyrus version display
    renders blank; the stamp is the proven shape.)"""
    try:
        src = open(os.path.join(os.path.dirname(__file__), 'native', 'plugin.cpp')).read()
        m = re.search(r'kPluginVersion\s*=\s*"([^"]+)"', src)
        if m:
            return m.group(1)
    except OSError:
        pass
    return default


def stamp_mcm_version():
    """Stamp the DLL version into the committed MCM config's Debug page as a
    static text control (MRO/MEO-style build stamp). Idempotent: replaces any
    existing 'Version' control. Unlike MEO, MAO's config.json is hand-authored,
    so this edits it in place rather than generating it.

    MEO-shape rules (marth 2026-07-16 'MCM has formatting errors'): the page
    OPENS with a header before the Version row (a bare text row at the top
    renders mis-aligned), and the stamped value is the NUMERIC version only —
    kPluginVersion's parenthetical description overflows the value column."""
    path = os.path.join(os.path.dirname(__file__), 'data', 'MCM', 'Config', 'MAO', 'config.json')
    with open(path, encoding='utf-8') as f:
        config = json.load(f)
    full = read_mao_version()
    m = re.match(r'[0-9][0-9.]*', full)
    ver = m.group(0) if m else full
    header = {"text": "Debug", "type": "header"}
    control = {
        "text": "Version",
        "type": "text",
        "help": f"MAO version, stamped from the build ({full}). Matches the MAO.dll built from the same commit.",
        "valueOptions": {"value": f"v{ver}"},
    }
    for page in config.get("pages", []):
        if page.get("pageDisplayName") == "Debug":
            content = page.setdefault("content", [])
            content[:] = [c for c in content
                          if not (c.get("text") == "Version" and c.get("type") == "text")
                          and not (c.get("text") == "Debug" and c.get("type") == "header")]
            content.insert(0, control)
            content.insert(0, header)
            break
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(config, f, indent='\t', ensure_ascii=False)
        f.write('\n')
    print(f"Stamped MCM version v{ver} -> {path}")

# ── FormIDs (single master Skyrim.esm -> own file index 0x01) ──
OWN = 0x01000000
FREF_EQUP_VOICE = 0x00025BEE  # Skyrim.esm EQUP "Voice" (lesser-power slot)

FID_FIELDKIT_MGEF  = OWN | 0x800  # FROZEN — inert effect the power carries
FID_FIELDKIT_SPELL = OWN | 0x801  # FROZEN — the Open Field Kit lesser power (dormant)
FID_FLASK_BASE     = OWN | 0x810  # FROZEN — 6 dedicated flask AlchemyItems (0x810..0x815)
NUM_FLASKS         = 6            # = kMaxFlaskSlots in the DLL
FID_PERK_CAPSTONE  = OWN | 0x816  # FROZEN — MAO capstone perk (the 6/9 kit ceiling)
FID_MCM_QUEST      = OWN | 0x817  # FROZEN — MCM Helper config quest (attaches MAO_MCM)
FID_PERK_BASE      = OWN | 0x818  # FROZEN — MAO flag perks 0x818..0x823 (MAO_PERKS order)
NEXT_OBJECT_ID     = 0x824        # first unused local; grows as forms are added

# Papyrus VMAD (script attachment) format — versions per MEO's proven generator.
VMAD_VERSION, OBJECT_FORMAT = 5, 2

# Vanilla Skyrim.esm forms referenced by the flask records.
KWD_VENDOR_POTION   = 0x0008CDEC  # KYWD VendorItemPotion (classifies it as a potion)
KWD_VENDOR_NO_SALE  = 0x000FF9FB  # KYWD VendorNoSale — vendors refuse to trade it
                                  # (the kit is permanent hardware, not merchandise)
MGEF_RESTORE_HEALTH = 0x0003EB15  # placeholder effect (the DLL mutates it at runtime)
FLASK_MODEL         = "Clutter\\Potions\\PotionFortifySkill01.nif"  # a generic potion mesh

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


# ── Papyrus script attachment (VMAD) + MCM quest, ported verbatim from MEO's
# proven generator. The MCM page is invisible to MCM Helper unless a quest
# carrying an MCM_ConfigBase-derived script is registered with SkyUI; MCM Helper
# derives the modName from the quest's plugin stem ("MAO") and then reads
# Data/MCM/Config/MAO/config.json. ──
class VMADBuilder:
    def __init__(self):
        self.scripts = []

    def add_script(self, name, props):
        self.scripts.append((name, props))

    def build(self):
        b = BytesIO()
        b.write(struct.pack('<HHH', VMAD_VERSION, OBJECT_FORMAT, len(self.scripts)))
        for name, props in self.scripts:
            e = name.encode('ascii')
            b.write(struct.pack('<H', len(e)) + e + struct.pack('<B', 0) + struct.pack('<H', len(props)))
            for pn, pv in props:
                pe = pn.encode('ascii')
                b.write(struct.pack('<H', len(pe)) + pe + bytes([pv[0]]) + struct.pack('<B', 1) + pv[1:])
        return b.getvalue()

# QUST DNAM: flags at offset 4. 0x0001 = Start Game Enabled.
def qust_dnam(flags=0x0001):
    return struct.pack('<B', 20) + b'\x01\x00\xff' + struct.pack('<HHI', flags, 0, 0)

def make_mcm_quest():
    # Start-game-enabled quest whose only job is to carry the MAO_MCM script
    # (extends MCM Helper's MCM_ConfigBase). Zero VMAD properties: MCM Helper
    # renders from Data/MCM/Config/MAO/config.json and persists to
    # Data/MCM/Settings/MAO.ini; the DLL reads that INI. modName is derived by
    # MCM Helper from this quest's plugin stem ("MAO"), so no property needed.
    vmad = VMADBuilder()
    vmad.add_script("MAO_MCM", [])
    body = subrec('EDID', zstr("MAO_MCMQuest")) + subrec('FULL', zstr("MAO MCM")) \
        + subrec('VMAD', vmad.build())
    body += subrec('DNAM', qust_dnam(0x0001)) + subrec('NEXT', b'') \
        + subrec('ANAM', struct.pack('<I', 0))
    return group('QUST', record('QUST', FID_MCM_QUEST, 0, body))


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


# ── Dedicated flask potions. Each is a real, permanent AlchemyItem the player
# favorites / puts on Wheeler; the DLL RENAMES it to the configured variant at
# runtime (and casts that variant on drink), so reconfiguring changes CONTENTS
# not IDENTITY and the binding survives. ENIT flags 0x10000 = kMedicine. The
# Restore Health effect here is a placeholder (the flask's item card shows it;
# the drink cast comes from the variant). Byte order per the vanilla ALCH recipe. ──
def make_flask(fid, edid):
    body  = subrec('EDID', zstr(edid))
    body += subrec('OBND', b'\x00' * 12)
    body += subrec('FULL', zstr("Field Flask"))
    # VendorItemPotion: auto-potion mods + UI treat the flask as a potion (the
    # §6 compat contract). VendorNoSale: no vendor will buy the flask — selling
    # your permanent kit hardware was possible until v0.20.2 (marth).
    body += subrec('KSIZ', struct.pack('<I', 2))
    body += subrec('KWDA', struct.pack('<II', KWD_VENDOR_POTION, KWD_VENDOR_NO_SALE))
    body += subrec('MODL', zstr(FLASK_MODEL))
    body += subrec('DATA', struct.pack('<f', 0.5))                       # weight
    body += subrec('ENIT', struct.pack('<iIIfI', 10, 0x00010000, 0, 0.0, 0))
    body += subrec('EFID', struct.pack('<I', MGEF_RESTORE_HEALTH))
    body += subrec('EFIT', struct.pack('<fII', 5.0, 0, 0))               # mag, area, dur
    return record('ALCH', fid, 0, body)

def make_flasks():
    out = b''.join(make_flask(FID_FLASK_BASE + i, "MAO_Flask%d" % i) for i in range(NUM_FLASKS))
    return group('ALCH', out)


# ── MAO capstone perk (DESIGN §5.2 "Master Alchemist's Crucible"). A minimal,
# effect-less BGSPerk: the DLL reads HasPerk to unlock the 6/9 kit ceiling, so
# the record needs no entry points. Not placed in any tree yet — granted via the
# debug/MCM toggle now, positioned by the load-order-aware installer later. DATA
# = trait0 level0 ranks1 playable1 hidden0 (PERK recipe ported from MEO). ──

# MAO flag perks (DESIGN §5.2). Minimal marker perks — no entry points; the DLL
# reads HasPerk and applies effects. Order/index FROZEN (the DLL looks them up
# by these local FormIDs). The C# installer wires these into the load order's
# alchemy tree (MAO - Patch.esp); interim the DLL auto-grants them by skill.
# (edid, name, desc, skill_req, next_slot). skill_req becomes a CTDA
# GetBaseActorValue(Alchemy) >= req on the record (the skill menu greys the
# perk until it passes) and mirrors the DLL's interim auto-grant thresholds.
# next_slot chains ranked perks via NNAM (Requiem convention) so the five
# Kit Calibrations display as one 5-rank node in the installer-written tree.
# Vehicle ported verbatim from MEO_PERKS; only content and AV (16 = Alchemy,
# was 23 = Enchanting) differ. Vanilla survivors Snakeblood/Green Thumb stay
# in the tree untouched — these perks ADD effects, they replace nothing.
MAO_PERKS = [
    ("MAO_Perk_KitCalibration1",    "Kit Calibration I",   "Your field kit holds an extra charge per flask (2 flasks / 3 charges).",                     0, 1),   # 0x818
    ("MAO_Perk_KitCalibration2",    "Kit Calibration II",  "Your field kit mounts a third flask (3 flasks / 3 charges).",                               20, 2),   # 0x819
    ("MAO_Perk_KitCalibration3",    "Kit Calibration III", "Tier I and Tier II essences are 15% more efficient during automated refills.",              40, 3),   # 0x81A
    ("MAO_Perk_FieldDeployment",    "Field Deployment",    "Bulk capacity burst: your kit scales to 4 flasks / 5 charges.",                             60, 4),   # 0x81B
    ("MAO_Perk_KitCalibration5",    "Kit Calibration V",   "Tier I and Tier II essence maintenance costs are halved during resting checkouts.",         80, None),# 0x81C
    ("MAO_Perk_FluidMotion",        "Fluid Motion",        "The movement penalty while drinking from a flask is halved.",                               20, None),# 0x81D
    ("MAO_Perk_VanguardCoating",    "Vanguard Coating",    "Unlocks weapon coatings: offensive blueprints can be applied to your weapons for 45 seconds. Without this perk they cannot be prepared.", 30, None),# 0x81E
    ("MAO_Perk_ApexStabilization",  "Apex Stabilization",  "Tier III Apex essence costs are reduced by 35%.",                                           30, None),# 0x81F
    ("MAO_Perk_FieldExtraction",    "Field Extraction",    "Gathering flora and looting creatures yields 10% more essence.",                            50, None),# 0x820
    ("MAO_Perk_CorrosiveRetention", "Corrosive Retention", "Weapon coatings persist for 120 seconds.",                                                  60, None),# 0x821
    ("MAO_Perk_PouchExpansion",     "Pouch Expansion",     "Harvesting flora has a 15% chance to also yield a Tier II Catalyst essence.",               70, None),# 0x822
    ("MAO_Perk_ExtendedSynthesis",  "Extended Synthesis",  "Drinkable flask buff durations are extended by 30 seconds.",                                80, None),# 0x823
]


def ctda_skill_req(av_index, value):
    """CTDA: GetBaseActorValue(av) >= value, run on subject. 32 bytes.
    (Byte recipe verbatim from MEO.)"""
    return struct.pack('<B3xfH2xiiiii',
                       0x60,        # operator GreaterThanOrEqual (3<<5)
                       float(value),
                       277,         # function index GetBaseActorValue
                       av_index, 0, # param1 = actor value, param2 unused
                       0, 0, -1)    # runOn Subject, reference, param3


AV_ALCHEMY = 16  # GetBaseActorValue index (MEO used 23 = Enchanting)


def make_perks():
    out = BytesIO()
    data = struct.pack('<BBBBB', 0, 0, 1, 1, 0)  # trait0 level0 ranks1 playable1 hidden0
    # The capstone keeps its FROZEN pre-band FormID 0x816; it gains the same
    # skill gate as the rest (Alchemy 100) now that the vehicle is real perks.
    body = subrec('EDID', zstr("MAO_Perk_Capstone")) \
        + subrec('FULL', zstr("Master Alchemist's Crucible")) \
        + subrec('DESC', zstr("Your field kit reaches its master configuration: "
                              "6 flasks, 9 charges each.")) \
        + subrec('CTDA', ctda_skill_req(AV_ALCHEMY, 100)) \
        + subrec('DATA', data)
    out.write(record('PERK', FID_PERK_CAPSTONE, 0, body))
    for i, (edid, name, desc, req, nxt) in enumerate(MAO_PERKS):
        body = subrec('EDID', zstr(edid)) + subrec('FULL', zstr(name)) + subrec('DESC', zstr(desc))
        if req > 0:
            body += subrec('CTDA', ctda_skill_req(AV_ALCHEMY, req))
        body += subrec('DATA', data)
        if nxt is not None:
            body += subrec('NNAM', struct.pack('<I', FID_PERK_BASE + nxt))
        out.write(record('PERK', FID_PERK_BASE + i, 0, body))
    return group('PERK', out.getvalue())


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "out"
    os.makedirs(out_dir, exist_ok=True)
    esp = BytesIO()
    esp.write(make_tes4(NEXT_OBJECT_ID))
    esp.write(make_mgef())
    esp.write(make_flasks())
    esp.write(make_spel())
    esp.write(make_perks())
    esp.write(make_mcm_quest())
    data = esp.getvalue()
    path = os.path.join(out_dir, "MAO.esp")
    with open(path, 'wb') as f:
        f.write(data)
    stamp_mcm_version()
    print(f"Written: {path} ({len(data):,} bytes)")
    print(f"  MGEF x1 (inert)   ALCH x{NUM_FLASKS} (flasks 0x{FID_FLASK_BASE & 0xFFFFFF:03X}.."
          f"0x{(FID_FLASK_BASE + NUM_FLASKS - 1) & 0xFFFFFF:03X})   SPEL x1 (dormant)"
          f"   PERK x{1 + len(MAO_PERKS)} (capstone 0x{FID_PERK_CAPSTONE & 0xFFFFFF:03X}"
          f" + flags 0x{FID_PERK_BASE & 0xFFFFFF:03X}..0x{(FID_PERK_BASE + len(MAO_PERKS) - 1) & 0xFFFFFF:03X})"
          f"   QUST x1 (MCM 0x{FID_MCM_QUEST & 0xFFFFFF:03X})")
    print("  ESL-flagged, master Skyrim.esm")


if __name__ == "__main__":
    main()
