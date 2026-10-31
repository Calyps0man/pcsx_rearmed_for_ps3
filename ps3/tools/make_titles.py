#!/usr/bin/env python3
"""Builds data/ps1titles.txt (serial -> title, for naming the disc in the drive)
from libretro-database's metadat/redump/Sony - PlayStation.dat (CC BY-SA 4.0,
https://github.com/libretro/libretro-database). One line per serial,
"SLUS00707<TAB>Silent Hill", sorted; region, version and language tags are
dropped from the names, "(Disc N)" is kept.
usage: make_titles.py "Sony - PlayStation.dat" > ../data/ps1titles.txt"""
import re, sys
out = {}
name = None
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    m = re.match(r'\s*name "(.*)"\s*$', line)
    if m: name = m.group(1); continue
    m = re.match(r'\s*serial "(.*)"\s*$', line)
    if m and name:
        disc = re.search(r'\((Disc \d+)\)', name)
        t = re.sub(r'\s*\([^)]*\)', '', name).strip()
        t = re.sub(r'\s*\[[^\]]*\]', '', t).strip()
        if disc: t += ' (' + disc.group(1) + ')'
        for s in re.split(r'[,\s]+', m.group(1)):
            k = re.sub(r'[^A-Z0-9]', '', s.upper())
            if re.fullmatch(r"[A-Z]{4}[0-9]{5}", k) and k not in out: out[k] = t
for k in sorted(out): print(k + '\t' + out[k])
