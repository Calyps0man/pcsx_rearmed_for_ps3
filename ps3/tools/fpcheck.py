#!/usr/bin/env python3
"""Checks cgc fp40 output for a PSL1GHT cgcomp hazard: cgcomp puts the colour
output in register 0 (H0 for a half result, R0 for a float one; R0 overlaps H0
and H1) but does not keep temporaries off it. After the first write to oCol, any
temporary written to H0/H1/R0 overwrites the colour, and a read of one of them
gets the colour instead of the temporary (this gave the RSX FXAA its dark
"cartoon" outlines). usage: fpcheck.py file.fp40 ..."""
import re, sys
ALIAS = ('H0', 'H1', 'R0')
def comps(op):
    m = re.search(r'\.([xyzw]+)', op)
    return set(m.group(1)) if m else set('xyzw')
def read_comps(opcode, dst, src):
    """components of src an instruction reads (component-wise ops: only those
    feeding the components it writes)"""
    m = re.search(r'\.([xyzw]+)', src)
    swz = m.group(1) if m else 'xyzw'
    if re.match(r'(DP|TEX|TXP|TXB|TXD|TXL|NRM|RSQ|RCP|EX2|LG2|POW|COS|SIN|KIL|IF|DIV)', opcode):
        return set(swz)
    if len(swz) == 1:
        return set(swz)
    return {swz['xyzw'.index(c)] for c in comps(dst)}
bad = 0
for path in sys.argv[1:]:
    written = set()
    for n, line in enumerate(open(path), 1):
        line = line.split('#')[0].strip().rstrip(';')
        if not line or re.match(r'(!!|OPTION|PARAM|TEMP|OUTPUT|END\b|\{|\})', line) or line.startswith(('{', '}')):
            continue
        parts = line.split(None, 1)
        if len(parts) < 2:
            continue
        ops = [o.strip() for o in parts[1].split(',')]
        dst, srcs = ops[0], ops[1:]
        if written:
            for s in srcs:
                r = re.match(r'-?\|?(H0|H1|R0)\b', s)
                if r and read_comps(parts[0], dst, s) & written:
                    print('%s:%d: reads %s after the colour output: %s' % (path, n, s, line)); bad += 1
        if dst.startswith('oCol'):
            written |= comps(dst)
            continue
        if written and re.match(r'(H0|H1|R0)\b', dst):
            print('%s:%d: temporary %s written after the colour output: %s' % (path, n, dst, line)); bad += 1
sys.exit(1 if bad else 0)
