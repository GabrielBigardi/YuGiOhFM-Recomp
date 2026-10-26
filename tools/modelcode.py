#!/usr/bin/env python3
"""Extract the monster-model code from MODEL.MRG (and SU.MRG) as fixed-address overlays.

For each monster in a battle the game streams the card's block of MODEL.MRG: 276 sectors per
block, block = card number with the cards that have no 3D model left out (the loader at
0x800566A0). The block's chunks go to fixed buffers (the table at 0x80010000): two code
segments of 10 sectors for each monster slot (sectors 180 and 200 of the block for the first
monster at 0x8013B000, 190 and 210 for the second at 0x8017B000) and a header segment of 2
sectors for each slot (220 -> 0x8013A000, 222 -> 0x8017A000). Card 777 has its own block in
SU.MRG (sector 948) with code at 194 and 204. Each segment is one word followed by the function
the game calls at +4; the code is linked for its slot, sometimes at a RAM mirror address.

Each overlay is the whole segment (the code, then its data: switch tables can sit anywhere in
it). It is identified by its code, found by following the function's control flow (the data
after it may change while it runs, and the segments carry stale bytes from older builds).
Identical code at the same address is kept once.

usage: modelcode.py <disc.cue|disc.bin> [outdir]   -> outdir/<name>.bin + outdir/models.json
"""
import collections, hashlib, json, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from iso import Disc, find_file, resolve_bin
from mips import decode

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CODE_A, CODE_B, HEAD_A, HEAD_B = 0x8013B000, 0x8017B000, 0x8013A000, 0x8017A000


def ram_addr(a):
    return 0x80000000 | (a & 0x1FFFFF) if (a & 0xFF800000) == 0x80000000 else a


def trace(data, off, seg):
    """follow the control flow of the function at off+4 (branches, jumps and calls inside the
    segment, switch tables) -> (code length, votes for the code slots A and B)"""
    word = lambda o: struct.unpack_from('<I', data, off + o)[0]
    votes = collections.Counter()

    def local(t):
        # segment offset of an absolute address in either code slot (RAM mirrors folded)
        t = ram_addr(t)
        for slot, b in (('A', CODE_A), ('B', CODE_B)):
            if b + 4 <= t < b + seg and not t & 3:
                return slot, t - b
        return None, None

    todo, seen, hi = [4], set(), 4
    while todo:
        o = todo.pop()
        hist = []
        while 4 <= o < seg and o not in seen:
            seen.add(o)
            i = decode(CODE_A + o, word(o))     # branches are relative: the slot doesn't matter
            if i.kind == 'invalid':
                break
            hi = max(hi, o + 4)
            hist.append(i)
            if i.mn == 'lui' and 0x8000 <= i.imm < 0x8080:
                votes['A' if (i.imm & 0x1F) in (0x13, 0x14) else 'B' if (i.imm & 0x1F) in (0x17, 0x18) else '-'] += 1
            if i.kind == 'branch':
                todo.append(o + 4 + (i.simm << 2))
            elif i.kind in ('jump', 'call'):
                slot, t = local(i.target)
                if slot:
                    votes[slot] += 1
                    todo.append(t)
            if i.kind == 'jumpr' and i.rs != 31:
                # switch: the table address comes from a lui/addiu pair shortly before
                his = {}
                for p in hist[-40:]:
                    if p.mn == 'lui':
                        his[p.rt] = p.imm << 16
                    elif p.mn == 'addiu' and p.rs in his:
                        slot, t = local((his[p.rs] + p.simm) & 0xFFFFFFFF)
                        while slot and t + 4 <= seg:
                            s2, c = local(word(t))
                            if s2 != slot:
                                break
                            todo.append(c)
                            t += 4
            if i.kind in ('jump', 'jumpr') or (i.mn == 'beq' and i.rs == 0 and i.rt == 0):
                if o + 4 < seg and o + 4 not in seen:   # delay slot
                    seen.add(o + 4)
                    hi = max(hi, o + 8)
                break
            o += 4
    return hi, votes


BLOCK = 276                 # sectors per card block in MODEL.MRG
SEGMENTS = [                # (sector in the block, load address, sectors, name suffix)
    (180, CODE_A, 10, ''), (190, CODE_B, 10, ''), (200, CODE_A, 10, '_2'), (210, CODE_B, 10, '_2'),
    (220, HEAD_A, 2, ''), (222, HEAD_B, 2, ''),
]
SU_BLOCK = 948              # card 777: its block in SU.MRG
SU_SEGMENTS = [(194, CODE_A, 10, ''), (204, CODE_B, 10, '')]
TAG = {CODE_A: 'ma', CODE_B: 'mb', HEAD_A: 'ha', HEAD_B: 'hb'}


def segments(disc):
    """-> list of (name, segment bytes, load address)"""
    out = []
    e = find_file(disc, 'DATA/MODEL.MRG')
    if not e:
        raise SystemExit('DATA/MODEL.MRG is not on this disc')
    data = disc.read_extent(e['lba'], e['size'])
    for b in range(len(data) // (BLOCK * 0x800)):
        for sec, base, n, sfx in SEGMENTS:
            off = (b * BLOCK + sec) * 0x800
            out.append(('%s_%03d%s' % (TAG[base], b, sfx), data[off:off + n * 0x800], base))
    e = find_file(disc, 'DATA/SU.MRG')
    if e:
        data = disc.read_extent(e['lba'] + SU_BLOCK, 256 * 0x800)
        for sec, base, n, sfx in SU_SEGMENTS:
            out.append(('%s_777%s' % (TAG[base], sfx), data[sec * 0x800:(sec + n) * 0x800], base))
    return out


def extract(disc, outdir):
    seen = {}
    for name, seg, base in segments(disc):
        code, votes = trace(seg, 0, len(seg))
        if base in (CODE_A, CODE_B) and votes[{CODE_A: 'B', CODE_B: 'A'}[base]] > votes[TAG[base][1].upper()]:
            print('warning: %s looks linked for the other slot' % name)
        key = (base, hashlib.sha1(seg[:code]).digest())
        if key not in seen:
            seen[key] = ({'name': name, 'size': len(seg), 'code': code, 'base': '%08X' % base}, seg)
    os.makedirs(outdir, exist_ok=True)
    models = sorted(seen.values(), key=lambda m: (m[0]['base'], m[0]['name']))
    for m, seg in models:
        with open(os.path.join(outdir, m['name'] + '.bin'), 'wb') as f:
            f.write(seg)
    models = [m for m, _ in models]
    with open(os.path.join(outdir, 'models.json'), 'w') as f:
        json.dump(models, f, indent=0)
    return models


if __name__ == '__main__':
    d = Disc(resolve_bin(sys.argv[1]))
    ms = extract(d, sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'extract', 'models'))
    print('%d model code overlays, %d bytes of code' % (len(ms), sum(m['code'] for m in ms)))
