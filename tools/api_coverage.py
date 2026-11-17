#!/usr/bin/env python3
"""api_coverage.py - which 3DO OS calls the runtime provides, folio by folio.

Compares the function tables of the Portfolio OS (vector and SWI numbers, as
listed in its folio sources) with what the runtime registers (TDORT_DUMP_API).
Only the call names and numbers are read from the OS sources.

    TDORT_DUMP_API=api.tsv  <any converted game> --headless     (TDORT_FRAMES=1)
    python tools/api_coverage.py <portfolio_os checkout> api.tsv  [--usage DIR]

--usage DIR counts, over the tdort.log files under DIR (e.g. batch-results),
how many games called each placeholder, to rank the work.
"""
import argparse
import collections
import os
import re
import sys

# folio source file, runtime vector-name prefix, SWI base
FOLIOS = [
    ("kernel",        "src/kernel/folio.c",                               "kernel",        0x10000),
    ("graphics",      "src/graphics/intgraf.c",                           "Graphics",      0x20000),
    ("file",          "src/filesystem/FileFolio.c",                       "File",          0x30000),
    ("audio",         "src/audio/audiofolio/audio_folio.c",               "audio",         0x40000),
    ("math",          "src/libs/operamath/mathfolio.c",                   "Operamath",     0x50000),
    ("international", "src/international/folio/international_folio.c",   "International", None),
    ("compression",   "src/compression/folio/compression_folio.c",        "Compression",   None),
    ("jstring",       "src/jstring/folio/jstring_folio.c",                "JString",       None),
]

# Placeholder entries in the OS tables (no real function behind the number).
FILLER = re.compile(r"^(NULROUTINE|illegal|swiNotImplemented|MathError|NotImplemented|Unimplemented|stackoverflow|cstartup)$", re.I)
ENTRY = re.compile(r"\(void\s*\*\s*\(\*\)\s*\(\)\)\s*(?:MISC_CODE\()?\s*(\w+)\)?\s*,?\s*/\*\s*(-?\d+)\s*\*/")


def parse_tables(path):
    """Returns {'swi': {n: name}, 'user': {vector index (negative): name}}."""
    text = open(path, encoding="latin-1").read()
    text = re.sub(r"/\*\s*\(void[^*]*\*/", "", text)          # commented-out entries
    swi, user = {}, {}
    for m in re.finditer(r"(\w+)\s*\[\s*\]\s*\)\s*\(\s*\)\s*=\s*\{(.*?)\};", text, re.S):
        table, body = m.group(1), m.group(2)
        entries = [(name, int(num)) for name, num in ENTRY.findall(body)]
        if not entries:
            continue
        if table == "KernelFunctions":
            # SWIs (0..n), then supervisor-only calls (-1..), then user vectors (n..1 = -n..-1)
            phase = "swi"
            for name, num in entries:
                if phase == "swi" and num < 0:
                    phase = "super"
                elif phase == "super" and num > 0:
                    phase = "user"
                if phase == "swi":
                    swi.setdefault(num, name)
                elif phase == "user":
                    user.setdefault(-num, name)
        elif "SWI" in table:
            if not swi:                                   # first variant only (math has several)
                for name, num in entries:
                    swi.setdefault(num, name)
        elif "User" in table:
            for name, num in entries:
                if num < 0:
                    user[num] = name                      # later entries win (a stray "-1" label heads the graphics table)
    return {"swi": swi, "user": user}


def parse_dump(path):
    vectors, swis = {}, {}
    for line in open(path, encoding="utf-8", errors="replace"):
        f = line.rstrip("\n").split("\t")
        if f[0] == "vector" and len(f) >= 3:
            m = re.match(r"(\w+)\[(-?\d+)\]$", f[1])
            if m:
                vectors[(m.group(1), int(m.group(2)))] = f[2]
        elif f[0] == "swi" and len(f) >= 4:
            swis[int(f[1], 16)] = (f[2], f[3])
    return vectors, swis


def usage_counts(root):
    """How many games' logs mention each stub / unimplemented call."""
    counts = collections.Counter()
    for dirpath, _, files in os.walk(root):
        for fn in files:
            if fn != "tdort.log":
                continue
            seen = set()
            for line in open(os.path.join(dirpath, fn), encoding="latin-1"):
                m = re.match(r"HLE stub (\w+\[-?\d+\])", line)
                if m:
                    seen.add(m.group(1))
                m = re.match(r"unimplemented SWI 0x([0-9A-Fa-f]+)", line)
                if m:
                    seen.add("swi:%X" % int(m.group(1), 16))
                m = re.match(r"HLE stub (audio:\w+)", line)
                if m:
                    seen.add(m.group(1))
            counts.update(seen)
    return counts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("portfolio_os")
    ap.add_argument("dump")
    ap.add_argument("--usage", help="folder of tdort.log files (e.g. batch-results)")
    a = ap.parse_args()

    vectors, swis = parse_dump(a.dump)
    used = usage_counts(a.usage) if a.usage else None
    total = collections.Counter()
    todo = []

    for folio, src, prefix, swibase in FOLIOS:
        path = os.path.join(a.portfolio_os, src)
        if not os.path.exists(path):
            print(f"(missing {src})", file=sys.stderr)
            continue
        t = parse_tables(path)
        rows = []
        for idx, name in sorted(t["user"].items(), reverse=True):
            if FILLER.match(name):
                continue
            state = vectors.get((prefix, idx), "missing")
            rows.append((f"{prefix}[{idx}]", name, state))
        if swibase is not None:
            for n, name in sorted(t["swi"].items()):
                if FILLER.match(name):
                    continue
                reg = swis.get(swibase + n)
                state = "missing" if reg is None else reg[1]
                rows.append((f"SWI {swibase + n:05X}", name, state))
        c = collections.Counter(r[2] for r in rows)
        total.update(c)
        print(f"\n== {folio}: {c['impl']} implemented, {c['stub']} placeholder, {c['missing']} missing (of {len(rows)})")
        for slot, name, state in rows:
            if state == "impl":
                continue
            key = slot if not slot.startswith("SWI") else "swi:%X" % int(slot.split()[1], 16)
            n = used.get(key, 0) if used else None
            todo.append((n or 0, folio, slot, name, state))
            print(f"  {state:8} {slot:18} {name}" + (f"   ({n} games)" if used else ""))

    print(f"\nTOTAL: {total['impl']} implemented, {total['stub']} placeholder, {total['missing']} missing")
    if used:
        print("\nMost used first:")
        for n, folio, slot, name, state in sorted(todo, key=lambda r: -r[0])[:40]:
            if n:
                print(f"  {n:4} games  {slot:18} {name}")


if __name__ == "__main__":
    main()
