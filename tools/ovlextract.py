#!/usr/bin/env python3
"""Copy the code overlays listed in tools/overlays.json out of a disc image into extract/ovl/.

usage: ovlextract.py <disc.cue|disc.bin> [outdir]
"""
import json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from iso import Disc, find_file, resolve_bin

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def extract(disc, outdir):
    """returns {name: bytes} and writes extract/ovl/<name>.bin"""
    cfg = json.load(open(os.path.join(ROOT, 'tools', 'overlays.json')))
    files = {}
    out = {}
    os.makedirs(outdir, exist_ok=True)
    for o in cfg['overlays']:
        if o['file'] not in files:
            e = find_file(disc, o['file'])
            if not e:
                raise SystemExit('%s is not on this disc' % o['file'])
            files[o['file']] = e
        e = files[o['file']]
        off, size = int(o['offset'], 16), int(o['size'], 16)
        data = disc.read_extent(e['lba'] + off // 2048, size + off % 2048)[off % 2048:]
        out[o['name']] = data
        with open(os.path.join(outdir, o['name'] + '.bin'), 'wb') as f:
            f.write(data)
    return out


if __name__ == '__main__':
    d = Disc(resolve_bin(sys.argv[1]))
    r = extract(d, sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'extract', 'ovl'))
    print('%d overlays extracted' % len(r))
