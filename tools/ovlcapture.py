#!/usr/bin/env python3
"""Find code overlays in a debug log (--log 3, "CD DMA lba=... -> addr" lines): runs of sectors
read straight into memory that contain MIPS code. Prints overlays.json entries for the runs that
are not in tools/overlays.json yet.

usage: ovlcapture.py <disc.cue|disc.bin> <log file> [--add]
"""
import json, os, re, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from iso import Disc, walk, resolve_bin

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STREAM_BUFFERS = [(0x801DC000, 0x801DD800)]      # sector-by-sector staging buffers (not code)


def code_score(data):
    w = struct.unpack('<%dI' % (len(data) // 4), data[:len(data) // 4 * 4])
    return sum(1 for x in w if x == 0x03E00008), sum(1 for x in w if (x >> 16) == 0x27BD and (x & 0x8000))


def main():
    disc = Disc(resolve_bin(sys.argv[1]))
    files = [e for e in walk(disc) if not e['dir']]
    runs = []
    for line in open(sys.argv[2], errors='replace'):
        m = re.search(r'CD DMA lba=(-?\d+) -> ([0-9A-F]+) words=512\b', line)
        if not m:
            continue
        lba, dest = int(m.group(1)), int(m.group(2), 16) | 0x80000000
        if any(lo <= dest < hi for lo, hi in STREAM_BUFFERS) or not 0x80100000 <= dest < 0x80200000:
            continue
        r = runs[-1] if runs else None
        if r and r['lba_end'] == lba and r['dest_end'] == dest:
            r['lba_end'] += 1
            r['dest_end'] += 2048
        else:
            runs.append(dict(lba=lba, lba_end=lba + 1, dest=dest, dest_end=dest + 2048))
    cfg_path = os.path.join(ROOT, 'tools', 'overlays.json')
    cfg = json.load(open(cfg_path))
    known = {(o['file'], int(o['offset'], 16), int(o['base'], 16)) for o in cfg['overlays']}
    seen = set()
    new = []
    for r in runs:
        f = next((e for e in files if e['lba'] <= r['lba'] < e['lba'] + (e['size'] + 2047) // 2048), None)
        if not f:
            continue
        data = b''.join(disc.user(l) for l in range(r['lba'], r['lba_end']))
        jr, pro = code_score(data)
        if jr < 2 or pro < 2:
            continue
        name = f['path'].lstrip('/').split(';')[0]
        off = (r['lba'] - f['lba']) * 2048
        key = (name, off, r['dest'])
        if key in seen or key in known:
            continue
        seen.add(key)
        stem = os.path.splitext(os.path.basename(name))[0].lower().replace('_mrg', '').replace('wa', 'wa')
        entry = {'name': '%s_%06x' % (stem, off), 'file': name, 'offset': '%X' % off,
                 'size': '%X' % len(data), 'base': '%08X' % r['dest']}
        print('%-28s %s+0x%06X %6d bytes -> %08X (jr ra %d, prologues %d)' % (
            entry['name'], name, off, len(data), r['dest'], jr, pro))
        new.append(entry)
    if '--add' in sys.argv and new:
        cfg['overlays'].extend(new)
        json.dump(cfg, open(cfg_path, 'w'), indent=1)
        print('added %d overlays to %s' % (len(new), cfg_path))


if __name__ == '__main__':
    main()
