# Technical notes

How the port works internally, for people who want to hack on it. The player-facing
documentation is the [README](../README.md).

## Pipeline

```
your disc image (.cue/.bin)
   │  tools/iso.py                 ISO 9660 reader: SLUS_014.11
   │  tools/ovlextract.py          the scene code overlays in DATA/WA_MRG.MRG and DATA/SU.MRG
   ▼  tools/modelcode.py           the monster-model code in DATA/MODEL.MRG (and SU.MRG)
extract/                           verified by SHA-256 (tools/game_files.json)
   │  tools/recomp.py              static recompiler: MIPS R3000A machine code -> C
   ▼
generated/                         ~3,450 C functions, ~2.4M lines (executable + 9 overlays + 662 model segments)
   │  tools/compile.py             clang (llvm-mingw) on Windows, cc on Linux, zig cc for Linux
   ▼                               cross builds
build/<config>/YuGiOhFM(.exe)      runtime/ + generated/ linked into one native program
   │  tools/package.py
   ▼
dist/YuGiOhFM-<OS>-x64/            exe + SDL2 + yugiohfm.ini + README (+ YuGiOhFM.pak)
```

`build.py` runs all of it and only redoes the steps whose inputs changed (`generated/.stamp`
holds a hash of the game files and the recompiler sources).

## The game's files

* `SLUS_014.11`: the main executable, 1.9 MB loaded at 0x80010000, entry 0x800129D8. Code runs
  from 0x80010000 to 0x800906D4; the rest is data (card tables, text, graphics).
* `DATA/WA_MRG.MRG`, `DATA/SU.MRG`: archives of scene data. Among the graphics and tables are
  the game's **code overlays**, read to fixed addresses: 0x80146000 (the duel, 0x16000 bytes;
  WA_MRG holds seven identical copies, one per scene package), 0x80168000 (menus and other
  scenes, several overlays replacing each other) and 0x80180000 (two more). Each starts with
  one id word. The list, with disc offsets and sizes, is `tools/overlays.json`.
* `DATA/MODEL.MRG`: the 3D monsters, one 276-sector block per card that has a model (cards
  300-349, 650-699 and 720 have none; block = card number minus the skipped ones, see the
  loader at 0x800566A0). Every block carries **code**: the function that animates the monster
  in a battle. The block's chunks are streamed to fixed buffers listed in the table at
  0x80010000: two code segments of 10 sectors per monster slot (block sectors 180 and 200 for
  the first monster at 0x8013B000, 190 and 210 for the second at 0x8017B000) and a 2-sector
  header segment per slot (220 → 0x8013A000, 222 → 0x8017A000). Card 777 has its block in
  SU.MRG (sector 948, code at 194 and 204). Each segment is one word (the card number)
  followed by the function the game calls at +4. `tools/modelcode.py` extracts them: 3,728
  segments, 662 after removing identical code.
* `DATA/MOVIE.STR`: MDEC movies; `DATA/MASTER.XA`: XA-ADPCM music and voices;
  `DATA/SD_BGM.DAT`, `DATA/SD_SE.DAT`: sequenced music and sound effects for the SPU.

## The recompiler (`tools/recomp.py`, configured by `tools/modules.json`)

* **Function discovery** is a fixpoint over: the entry point, `jal` targets, `lui`/`addiu`
  code-address constants (including ones built in branch delay slots), data words that point
  at code, a sweep after each `jr ra`, and jump-table targets. Candidates must look like
  function starts (a prologue, a BIOS call stub, or the previous function's `jr ra` before
  them, maybe followed by nop padding), must not look like ASCII text, and must trace without
  invalid instructions. `tools/extra_entries.json` adds the one entry point that can't be found
  statically (the libetc `setjmp` return used by HookEntryInt). Calls from the executable into
  overlay space become function starts in every overlay that has code there.
* **Code generation.** Each MIPS function becomes `void f_<address>(CPU *c)` (overlay
  functions `o<overlay>_<address>`). Registers live in the `CPU` struct, memory accesses use
  the inline helpers in `runtime/psx_recomp.h`, branches become `goto`s (the delay-slot
  instruction is emitted before the jump, after the condition is evaluated), recovered jump
  tables become `switch` statements, and indirect calls go through the runtime dispatcher
  `call_addr()`. Unaligned `lwl/lwr/swl/swr` and `div` by zero follow the R3000A rules;
  overflow traps are not modelled.
* **Overlays** are analysed at their load address like the executable. The many small ones
  are packed into `overlays_NNN.c` files of similar size.
* **RAM mirrors.** Part of the monster code is linked at 0x8053xxxx / 0x8057xxxx, mirrors of
  the 2 MB of RAM (0x80000000-0x807FFFFF repeats it). Jump and call targets, address constants
  and jump tables are folded back into 0x80000000-0x801FFFFF, and the generated `switch`
  statements and the dispatcher do the same at run time (`RAMADDR`).
* **setjmp / longjmp.** After a game over the game restarts from `main` with libc
  `longjmp` (0x8008FB8C) to the buffer `main` filled with `setjmp` (0x8008FB50). A translated
  `jr ra` is a C `return`, so a translated `longjmp` would return to its caller with `main`'s
  registers. The recompiler recognises both functions by their code: a call to `setjmp`
  becomes `GUEST_SETJMP(...)`, a native `setjmp` in the calling C function's frame keyed by
  the guest buffer, and `longjmp` ends in `guest_longjmp()`, which, after the guest registers
  are restored, does the native `longjmp` back to that frame (`runtime/cpu.c`).
* **Timing.** Every block adds its cost to `c->cycles` (loads ~5 cycles, mult 7, div 36, GTE
  commands with their documented latencies); the game's VSync timeouts and its CD/SPU/DMA
  waits depend on time advancing realistically.

A synthetic example of the translation (not code from the game):

```
MIPS                                   generated C
---------------------------------      ----------------------------------------------
loop: lw    t0, 0(a0)                  L_loop:
      addiu a0, a0, 4                    R[8] = MEM_R32(R[4]);
      addiu a1, a1, -1                   R[4] = R[4] + 4u;
      bne   a1, zero, loop               R[5] = R[5] - 1u;
      addu  v0, v0, t0   (delay slot)    c->cycles += 9;
      jr    ra                           { int br_ = R[5] != 0;
                                           R[2] = R[2] + R[8];          /* delay slot */
                                           if (br_) { EVCHK(); goto L_loop; } }
                                         return;
```

`EVCHK()` lets the scheduler run hardware events (timers, VBlank, CD, SPU) inside long loops.

## The dispatcher (`runtime/cpu.c`)

Indirect calls (`jalr`, function pointers) and calls into overlay space go through
`call_addr()`. An address in the main executable maps directly to its function. Overlays share
addresses (up to 325 monster segments start at 0x8013B000), so the dispatcher finds the one
in RAM: among the overlays that have a function at the address it checks a few spread-out
words, then an FNV-1a hash of the overlay's code (`hash`, `hash_len` in `generated/modules.c`;
the code extent only, because the data after it changes while the overlay runs). If several
match, the one with the longest matching code wins. Results are cached until DMA writes into
overlay memory again (`code_written()` bumps a generation counter; the CD and MDEC DMA paths
call it), and hashes are memoised per generation.

## The runtime (`runtime/`)

| File | What it reimplements |
|---|---|
| `cpu.c` | Dispatcher (above), COP0, syscall/break, exceptions |
| `sched.c` | Cycle-driven scheduler, interrupt controller, the 3 root counters, NTSC video timing (263 lines × 3413 GPU clocks → 59.29 Hz) |
| `mem.c` | RAM (with its mirrors), scratchpad, I/O map, the BIOS ROM version string |
| `bios.c` | The BIOS kernel, natively: exception/IRQ chains, HookEntryInt/ReturnFromException (setjmp/longjmp), events, memory-card file system on a raw `.mcd` image, device table, libc helpers, `printf` → log. No Sony BIOS code is used |
| `gpu.c` | GP0/GP1 command processing and a software rasterizer reproducing the PS1 GPU, 15- and 24-bit display |
| `gte.c` | Geometry Transformation Engine (COP2), bit-exact including flags |
| `spu.c` | Sound chip: 24 voices, ADPCM, ADSR, gaussian interpolation, reverb |
| `mdec.c` | Motion decoder for the movies: run-length/zigzag decoding, IDCT and YUV→RGB with the hardware's rounding, 4/8/15/24-bit output, FIFOs and status bits; DMA0/DMA1 move data on the MDEC's requests (`dma.c`) |
| `cdrom.c` | CD controller and drive: command/response/IRQ protocol, seek/read timing, 2048/2340-byte sectors, XA-ADPCM decoding with the hardware's 37.8 → 44.1 kHz zigzag resampler |
| `cdpak.c` | Reader for the optional data pack (below) |
| `dma.c` | DMA channels (GPU linked lists, OTC, CD, SPU, MDEC) |
| `sio.c`, `memcard.c` | SIO0: two DualShock controllers (digital/analog mode, config mode, 0x4D vibration mapping) or digital pads, and the memory-card protocol for both slots (`card1.mcd`, `card2.mcd`: 2P Duel and Trade read one save from each) |
| `platform_sdl.c` | Window, scaling, keyboard + two game controllers (analog sticks, rumble), audio output, frame pacing, screenshots, headless test mode. `sdl_dyn.h` loads SDL2 at run time on Linux |
| `main.c` | Configuration (`yugiohfm.ini`, command line), locating the game data, boot |

Things that cost time to find, in case you touch these areas:

* The game checks its region by reading the BIOS ROM's version string (the byte at
  0xBFC7FF52 must be `A` for America); without a BIOS image the runtime provides the string of
  the SCPH-7001's ROM (`System ROM Version 4.1 12/16/97 A`).
* MODEL.MRG is full of stale bytes from older builds of the archive: partial copies of other
  cards' code inside the model data and after each segment's real code. Scanning the archive
  for code finds hundreds of these; only the segments at the loader's fixed block offsets are
  real, and a segment's code extent has to be found by following its control flow, not by
  scanning until the first invalid instruction.
* Monster code reaches its switch tables through RAM-mirror addresses (`lui 0x8058`), and
  some tables sit near the end of the 10-sector segment, far from the code.
* The executable calls functions inside overlays directly (`jal 0x801680F4`), so those
  addresses are function starts even when nothing in the overlay itself points at them.
* Libraries patch the real BIOS's kernel code (pad/card handler hooks in kernel RAM around
  0xDF80); the native BIOS's tables point into placeholder ROM space so the patches are
  harmless.
* Pending interrupts must be dispatched again right after an exception handler returns, and
  libcd never sets the CD interrupt mask (the runtime starts with 0x1F like the real BIOS).

## The data pack (`YuGiOhFM.pak`, optional)

The game reads by CD sector address and streams movies and music with interleaved audio, so
the pack keeps the exact sector layout instead of loose files. `tools/makepak.py` stores all
220,184 sectors as subheader + payload, zlib-compressed in 16-sector chunks.

```
"YGOFMPAK"  u32 version=1, total_sectors, stored_sectors, sectors_per_chunk=16, nchunks, 0
u32 offsets[nchunks + 1]      file offset of each zlib chunk
u32 crc32[nchunks]            CRC32 of the ORIGINAL raw 2352-byte sectors of each chunk
zlib chunks                   per sector: flags (bit0 form 2, bit1 form-2 EDC is zero),
                              4-byte subheader, 2048 (form 1) or 2324 (form 2) bytes
```

`runtime/cdpak.c` inflates a chunk, rebuilds each sector bit-exactly (sync, BCD MSF header,
subheader copy, EDC, ECC P/Q for form 1), compares the chunk with its CRC32 the first time it
is used, and caches recent chunks. `YuGiOhFM --verify-data` checks the whole file.

## Testing and debugging

```
python tools/run_tests.py     unit tests: GTE, GPU, SPU, MDEC, DualShock pads, memory-card file system
python build.py --debug       console build in build/debug with debug info
```

The executable has options for automated testing (all deterministic, so two runs with the
same options produce identical output):

| Option | |
|---|---|
| `--headless` | no window, no audio |
| `--unthrottled` | run as fast as possible |
| `--frames N` | quit after N frames |
| `--shots N --shotdir DIR` | screenshot every N frames |
| `--dumpram F` | write RAM + VRAM at frame F (repeatable) |
| `--wav FILE` | record the audio output |
| `--input "frame:BTN[:dur],start-end/period:BTN[:dur]"` | scripted button presses (`P2+BTN` = second controller, which plugs it in) |
| `--fuzz SEED --fuzzstart F --fuzzend F` | random button presses between two frames |
| `--tracefrom F` | log every function call from frame F |
| `--calltest frame:addr:a0`, `--poke frame:addr:val` | call a game function / write memory at a frame |
| `--trace`, `--tracecalls` | log dispatcher activity / every call |
| `--log 0..4`, `--logfile F` | log level / log file (level 3 logs every CD sector DMA with its address) |

`tools/sheet.py out.png a.bmp b.bmp ...` builds contact sheets from screenshots;
`tools/mdis.py` disassembles and cross-references the extracted executable.
`tools/ovlcapture.py` turns a `--log 3` run's CD DMA lines into `overlays.json` entries for
code read into overlay memory, which is how the overlay list was checked against the game's
actual loads.
