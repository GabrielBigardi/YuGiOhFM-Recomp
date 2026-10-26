#!/usr/bin/env python3
"""Build the native port from YOUR OWN copy of Yu-Gi-Oh! Forbidden Memories (PlayStation, USA, SLUS-01411).

  1. reads the game executable and its code overlays from your disc image   -> extract/
  2. statically recompiles the PlayStation (MIPS) code to C               -> generated/
  3. compiles the runtime + the generated code into a native program      -> build/
  4. puts a ready-to-play folder together                                  -> dist/

usage: python build.py [--disc PATH] [--pak] [--zip] [--linux] [--debug] [--jobs N] [--clean]

  --disc PATH  your disc image (.cue or .bin); default: the first .cue (or .bin) in disc/
  --pak        also turn your disc image into the data pack YuGiOhFM.pak and put it
               in the dist folder, so the game runs without the disc image
  --zip        also create a .zip of the dist folder
  --linux      on Windows: cross-build the Linux version (run "python tools/fetch_deps.py --linux")
  --debug      unoptimised build with a console window, for development
  --clean      rebuild everything from scratch

Windows needs the toolchain in deps/ first:  python tools/fetch_deps.py
Linux needs a C compiler and the SDL2 development package (e.g. apt install build-essential libsdl2-dev).
"""
import glob, hashlib, os, platform, subprocess, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import compile as native            # noqa: E402
from iso import Disc, find_file, resolve_bin   # noqa: E402
from package import package         # noqa: E402

import json                         # noqa: E402
import modelcode, ovlextract        # noqa: E402

# the files the recompiled code is generated from, and their SHA-256 for the USA release: the
# executable, and digests of the code overlays (extract/ovl) and monster-model code (extract/models)
GAME = json.load(open(os.path.join(ROOT, 'tools', 'game_files.json')))
EXE = 'SLUS_014.11'
# inputs of the recompiler; generated/ is redone when any of them changes
RECOMP_INPUTS = ['tools/recomp.py', 'tools/mips.py', 'tools/modules.json', 'tools/extra_entries.json',
                 'tools/overlays.json', 'tools/modelcode.py', 'extract/models/models.json']


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def tree_digest(folder):
    """SHA-256 over the names and contents of the .bin files in folder"""
    h = hashlib.sha256()
    for p in sorted(glob.glob(os.path.join(folder, '*.bin'))):
        h.update(os.path.basename(p).encode() + b'\0')
        h.update(open(p, 'rb').read())
    return h.hexdigest()


def find_disc(arg):
    if arg:
        if not os.path.isfile(arg):
            sys.exit('disc image not found: ' + arg)
        return os.path.abspath(arg)
    for ext in ('*.cue', '*.bin'):
        hits = sorted(glob.glob(os.path.join(ROOT, 'disc', ext)))
        if hits:
            return hits[0]
    sys.exit('No disc image found.\n'
             'Put your own Yu-Gi-Oh! Forbidden Memories (PS1, USA) .cue + .bin into the "disc" folder,\n'
             'or pass it with:  python build.py --disc "path/to/Yu-Gi-Oh! Forbidden Memories.cue"')


def extracted_ok():
    out = os.path.join(ROOT, 'extract')
    return (os.path.exists(os.path.join(out, EXE)) and sha256_file(os.path.join(out, EXE)) == GAME['files'][EXE]
            and os.path.exists(os.path.join(out, 'models', 'models.json'))
            and tree_digest(os.path.join(out, 'ovl')) == GAME['overlays']
            and tree_digest(os.path.join(out, 'models')) == GAME['models'])


def extract(disc_path):
    """copy the game code the recompiler needs out of the disc image and verify it"""
    out = os.path.join(ROOT, 'extract')
    if extracted_ok():
        return
    binpath = resolve_bin(disc_path)
    if not os.path.isfile(binpath):
        sys.exit('the .bin file named in the .cue sheet is missing: ' + binpath)
    if os.path.getsize(binpath) % 2352:
        sys.exit('%s is not a raw Mode2/2352 image (.bin from a .cue/.bin dump)' % binpath)
    disc = Disc(binpath)
    print('reading game files from', binpath)
    bad = ('%s on this disc does not match the USA release (SLUS-01411).\n'
           'Only the original USA version is supported; check that your dump is good.')
    e = find_file(disc, EXE)
    if not e:
        sys.exit("%s is not on this disc - this is not Yu-Gi-Oh! Forbidden Memories (PS1, USA)" % EXE)
    exe = disc.read_extent(e['lba'], e['size'])
    if hashlib.sha256(exe).hexdigest() != GAME['files'][EXE]:
        sys.exit(bad % EXE)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, EXE), 'wb') as f:
        f.write(exe)
    for sub in ('ovl', 'models'):
        for old in glob.glob(os.path.join(out, sub, '*')):
            os.remove(old)
    ovlextract.extract(disc, os.path.join(out, 'ovl'))
    modelcode.extract(disc, os.path.join(out, 'models'))
    if tree_digest(os.path.join(out, 'ovl')) != GAME['overlays']:
        sys.exit(bad % 'The code in DATA/WA_MRG.MRG / DATA/SU.MRG')
    if tree_digest(os.path.join(out, 'models')) != GAME['models']:
        sys.exit(bad % 'The code in DATA/MODEL.MRG')


def recompile(clean):
    h = hashlib.sha256()
    h.update(json.dumps(GAME, sort_keys=True).encode())
    for p in RECOMP_INPUTS:
        h.update(open(os.path.join(ROOT, p), 'rb').read())
    key = h.hexdigest()
    stamp = os.path.join(ROOT, 'generated', '.stamp')
    if not clean and os.path.exists(stamp) and open(stamp).read() == key:
        return
    print('recompiling the game code (MIPS -> C) ...')
    os.makedirs(os.path.join(ROOT, 'generated'), exist_ok=True)
    os.makedirs(os.path.join(ROOT, 'build'), exist_ok=True)
    for old in glob.glob(os.path.join(ROOT, 'generated', '*')):
        os.remove(old)
    subprocess.check_call([sys.executable, os.path.join(ROOT, 'tools', 'recomp.py')])
    with open(stamp, 'w') as f:
        f.write(key)


def make_pak(disc_path):
    pak = os.path.join(ROOT, 'build', 'YuGiOhFM.pak')
    src = resolve_bin(disc_path)
    stamp = pak + '.src'
    key = '%s|%d|%d' % (os.path.abspath(src), os.path.getsize(src), int(os.path.getmtime(src)))
    if not (os.path.exists(pak) and os.path.exists(stamp) and open(stamp).read() == key):
        print('building the data pack from your disc image ...')
        subprocess.check_call([sys.executable, os.path.join(ROOT, 'tools', 'makepak.py'), disc_path, pak])
        with open(stamp, 'w') as f:
            f.write(key)
    return pak


def main():
    args = sys.argv[1:]
    if '-h' in args or '--help' in args:
        print(__doc__)
        return
    opt = lambda n: n in args
    disc_arg = args[args.index('--disc') + 1] if '--disc' in args else None
    jobs = int(args[args.index('--jobs') + 1]) if '--jobs' in args else None
    linux = opt('--linux') or platform.system() != 'Windows'

    disc_path = find_disc(disc_arg)
    print('disc image:', disc_path)
    extract(disc_path)
    recompile(opt('--clean'))
    exe = native.build(release=not opt('--debug'), linux_cross=opt('--linux'), clean=opt('--clean'), jobs=jobs)
    pak = make_pak(disc_path) if opt('--pak') else None

    files = [(exe, os.path.basename(exe))]
    if exe.endswith('.exe'):
        files.append((os.path.join(os.path.dirname(exe), 'SDL2.dll'), 'SDL2.dll'))
    name = 'YuGiOhFM-%s-x64%s' % ('Linux' if linux else 'Windows', '-debug' if opt('--debug') else '')
    # remember the disc image location in yugiohfm.ini, unless the build is for another machine
    out = package(name, files, pak=pak, disc_path=None if opt('--linux') else disc_path, make_zip=opt('--zip'))
    print('\nDone. Play:  %s' % os.path.join(out, os.path.basename(exe)))


if __name__ == '__main__':
    main()
