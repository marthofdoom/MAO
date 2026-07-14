#!/usr/bin/env python3
"""Merge missing keys from an MCM Settings seed into an existing Settings file.

MCM Helper reads a control's initial value from Data/MCM/Settings/<mod>.ini and
does NOT fall back to config.json's defaultValue for a key that is absent from an
ALREADY-EXISTING settings file — it reads such a key as the type's zero (a bool
shows OFF) and won't persist changes reliably. So every time we add a new MCM
control, existing installs need its key backfilled into their settings file, or
the control appears dead.

This backfills: for each `key = value` in the seed, if `key` is missing from the
target (anywhere, since the DLL and MCM match on the flat key name), append it
under its seed section — creating the section if needed. Existing target keys,
values, order, comments and BOM are left untouched, so user edits survive.

Usage: merge_mcm_settings.py <seed.ini> <target.ini>
Writes target.ini in place. Prints the keys it added (nothing if already merged).
"""
import sys


def parse(text):
    """Return (list of (section, key, value)) for every key line, in order."""
    section = ""
    out = []
    for line in text.splitlines():
        s = line.strip().lstrip("﻿")
        if s.startswith("[") and s.endswith("]"):
            section = s[1:-1]
        elif "=" in s and not s.startswith((";", "#")):
            key = s.split("=", 1)[0].strip()
            val = s.split("=", 1)[1].strip()
            out.append((section, key, val))
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: merge_mcm_settings.py <seed.ini> <target.ini>")
    seed_path, target_path = sys.argv[1], sys.argv[2]
    with open(seed_path, encoding="utf-8-sig") as f:
        seed = parse(f.read())
    with open(target_path, encoding="utf-8") as f:
        target_text = f.read()
    have = {k for _sec, k, _v in parse(target_text)}

    # Group missing keys by their seed section, preserving seed order.
    missing = [(sec, k, v) for sec, k, v in seed if k not in have]
    if not missing:
        print("merge: nothing to add")
        return

    body = target_text
    if body and not body.endswith("\n"):
        body += "\n"
    added = []
    last_section = None
    for sec, k, v in missing:
        # Always re-emit the section header before its appended keys. A repeated
        # [Section] header is harmless — SimpleIni (what MCM Helper uses) merges
        # duplicate sections — and it guarantees the key binds to the right
        # section even when that section already appears earlier in the file.
        if sec != last_section:
            body += f"\n[{sec}]\n"
            last_section = sec
        body += f"{k} = {v}\n"
        added.append(f"[{sec}] {k}")

    with open(target_path, "w", encoding="utf-8") as f:
        f.write(body)
    print("merge: added " + ", ".join(added))


if __name__ == "__main__":
    main()
