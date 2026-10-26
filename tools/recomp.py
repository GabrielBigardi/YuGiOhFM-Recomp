#!/usr/bin/env python3
"""Static recompiler: PS1 (MIPS R3000A) machine code -> C.

Every MIPS function becomes a C function `void f_<addr>(CPU *c)`.  Registers live in the CPU
struct, memory accesses go through the runtime's memory helpers, branches become gotos, jump
tables become switch statements and indirect calls go through the runtime dispatcher
(call_addr).

Three kinds of modules:
  fixed        the main executable (SLUS_014.11), linked to its load address
  overlay      code the game reads from its archives to a fixed address, many sharing an
               address: the scene code in WA_MRG.MRG / SU.MRG (tools/overlays.json) and the
               monster-model code in MODEL.MRG (tools/modelcode.py). The runtime finds the
               one in memory by a hash of its code. Functions are named o<overlay>_<addr>.
  relocatable  modules linked at address 0 that the game loads anywhere and patches from a
               relocation table (<name>.bin + <name>.rel, analysed in offset space, patched
               constants emitted relative to BASE). Supported for other games; this one has
               none (tools/modules.json).
"""
import struct, sys, os, json, re
from collections import defaultdict
from mips import decode, REG

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# .rel record types (relocatable modules)
REL_WORD, REL_HI16, REL_LO16, REL_J26 = 0, 1, 2, 3


def ram_addr(a):
    # 0x80000000-0x807FFFFF repeats the 2 MB of RAM (some code is linked at a mirror address)
    return 0x80000000 | (a & 0x1FFFFF) if (a & 0xFF800000) == 0x80000000 else a


def parse_rel(data):
    """offset -> (type, target): HI16 records are followed by the full target offset"""
    words = struct.unpack('<%dI' % (len(data) // 4), data[:len(data) // 4 * 4])
    rel = {}
    k = 0
    while k < len(words) and words[k] != 0xFFFFFFFF:
        w = words[k]
        t, off = w & 3, w & ~3
        k += 1
        target = None
        if t == REL_HI16:
            target = words[k]
            k += 1
        rel[off] = (t, target)
    return rel


# ----------------------------------------------------------------------------------------
# modules
# ----------------------------------------------------------------------------------------
class Module:
    def __init__(self, name, base, data, code_lo, code_hi, rel=None, reloc_index=None):
        self.name = name
        self.base = base
        self.data = data
        self.end = base + len(data)
        self.code_lo = code_lo
        self.code_hi = code_hi
        self.rel = rel                   # relocatable modules: offset -> (type, target)
        self.reloc_index = reloc_index
        self.flat = []                   # [(lo, hi)]: hand-written code emitted as one function
        self.overlay = False             # fixed-address overlay (shares its addresses with others)
        self._cache = {}
        self.strong = set()
        self.weak = set()
        self.funcs = {}          # start -> FuncInfo
        self.jtables = {}        # jr address -> list of targets
        self.notes = []

    @property
    def reloc(self):
        return self.rel is not None

    def flat_region(self, a):
        for lo, hi in self.flat:
            if lo <= a < hi:
                return lo, hi
        return None

    def inside_flat(self, a):
        # inside a flat region but not its entry point: never a function of its own
        r = self.flat_region(a)
        return r is not None and a != r[0]

    def in_code(self, a):
        return self.code_lo <= a < self.code_hi and (a & 3) == 0

    def word(self, a):
        o = a - self.base
        if o < 0 or o + 4 > len(self.data):
            return 0                     # past the end (e.g. a delay slot after the last word)
        return struct.unpack_from('<I', self.data, o)[0]

    def insn(self, a):
        i = self._cache.get(a)
        if i is None:
            i = decode(a, self.word(a))
            if not self.reloc and i.kind in ('call', 'jump') and i.target is not None:
                i.target = ram_addr(i.target)
            if self.reloc and i.kind in ('call', 'jump'):
                field = (i.word & 0x3FFFFFF) << 2
                # patched j/jal: a target inside this module; unpatched: the main executable
                i.target = field if self.relocation(a) == REL_J26 else 0x80000000 | field
            self._cache[a] = i
        return i

    def relocation(self, a):
        r = self.rel.get(a) if self.reloc else None
        return None if r is None else r[0]

    def lui_value(self, i):
        """value a lui puts in its register (as if loaded at 0 for patched ones)"""
        r = self.rel.get(i.addr) if self.reloc else None
        if r is not None and r[0] == REL_HI16:
            return (r[1] + 0x8000) & 0xFFFF0000
        return i.imm << 16

    @property
    def prefix(self):
        return 'o%s' % self.name if self.reloc or self.overlay else 'f'


class FuncInfo:
    def __init__(self, start):
        self.start = start
        self.body = set()        # instruction addresses executed as normal instructions
        self.delay = set()       # addresses emitted as delay slots
        self.labels = set()
        self.calls = set()
        self.tails = set()
        self.dyn_jr = []
        self.bad = False         # hit an invalid instruction or left the code (not a real function)
        self.flat = None         # (lo, hi) when this is a flat region
        self.flat_rets = set()   # flat region: return points of internal jal


def load_modules():
    """returns (modules, entry point of the main executable)"""
    cfg = json.load(open(os.path.join(ROOT, 'tools', 'modules.json')))
    exe = open(os.path.join(ROOT, 'extract', cfg['main']['file']), 'rb').read()
    pc, _, t_addr, t_size = struct.unpack_from('<4I', exe, 0x10)
    mods = [Module('main', t_addr, exe[0x800:0x800 + t_size], t_addr, int(cfg['main']['code_hi'], 16))]
    mods[0].flat = [(int(lo, 16), int(hi, 16)) for lo, hi in cfg['main'].get('flat', [])]
    for k, name in enumerate(cfg.get('relocatable', [])):
        d = open(os.path.join(ROOT, 'extract', 'ovl', name + '.bin'), 'rb').read()
        rel = parse_rel(open(os.path.join(ROOT, 'extract', 'ovl', name + '.rel'), 'rb').read())
        mods.append(Module(name, 0, d, 0, len(d) & ~3, rel=rel, reloc_index=k))
    # fixed-address overlays: code the game reads to a fixed address (several share an address)
    for o in load_overlay_list():
        d = open(os.path.join(ROOT, 'extract', o.get('dir', 'ovl'), o['name'] + '.bin'), 'rb').read()
        base = int(o['base'], 16)
        code_hi = base + (o['code'] if 'code' in o else len(d) & ~3)
        m = Module(o['name'], base, d, base + o.get('code_at', 0), code_hi)
        m.overlay = True
        m.ident_end = code_hi if 'code' in o else None     # identify by exactly this much of it
        m.strong.update(int(x, 16) for x in o.get('entries', []))
        mods.append(m)
    return mods, pc


def load_overlay_list():
    # tools/overlays.json: overlays listed by hand / captured; extract/models/models.json: the
    # monster-model code found by tools/modelcode.py
    out = []
    p = os.path.join(ROOT, 'tools', 'overlays.json')
    if os.path.exists(p):
        out += json.load(open(p))['overlays']
    p = os.path.join(ROOT, 'extract', 'models', 'models.json')
    if os.path.exists(p):
        for m in json.load(open(p)):
            # one header word, then the function the game calls at +4
            out.append({'name': m['name'], 'base': m['base'], 'dir': 'models', 'code_at': 4, 'code': m['code'],
                        'entries': ['%08X' % (int(m['base'], 16) + 4)]})
    return out


# ----------------------------------------------------------------------------------------
# analysis
# ----------------------------------------------------------------------------------------
def scan_constants(mod, addrs):
    """lui/addiu|ori constant pairs inside the given instruction addresses (straight-line tracking)."""
    out = set()
    hi = {}
    for a in sorted(addrs):
        i = mod.insn(a)
        if i.mn == 'lui':
            hi[i.rt] = (mod.lui_value(i), a)
            continue
        if i.mn in ('addiu', 'ori') and i.rs in hi and a - hi[i.rs][1] < 64:
            v = (hi[i.rs][0] + (i.imm if i.mn == 'ori' else i.simm)) & 0xFFFFFFFF
            out.add(ram_addr(v))
        d = i.dest()
        if d is not None:
            hi.pop(d, None)
    return out


def resolve_jump_table(mod, jr_addr, body_hint):
    """Recognise gcc jump-table dispatch ending at `jr rX`. Returns list of targets or None."""
    jr = mod.insn(jr_addr)
    X = jr.rs
    seq = []
    a = jr_addr - 4
    while a >= mod.code_lo and len(seq) < 24:
        seq.append(mod.insn(a))
        a -= 4
    # find lw X, off(B)
    lw = None
    k = 0
    for k, i in enumerate(seq):
        if i.dest() == X:
            if i.mn == 'lw':
                lw = i
            break
    if lw is None:
        return None
    B = lw.rs
    off = lw.simm
    base = None
    rest = seq[k + 1:]
    # registers holding the table address may be set much earlier (e.g. once at the top of a
    # large function): keep looking further back for lui/addiu definitions
    a = jr_addr - 4 - 4 * len(seq)
    while a >= mod.code_lo and len(rest) < 4096:
        rest.append(mod.insn(a))
        a -= 4

    def const_of(reg, start):
        """value of reg if it is a lui(/addiu) constant, scanning rest[start:] backwards in time."""
        for n in range(start, len(rest)):
            i = rest[n]
            if i.dest() == reg:
                if i.mn == 'lui':
                    return mod.lui_value(i)
                if i.mn == 'addiu':
                    hv = const_of(i.rs, n + 1)
                    return None if hv is None else (hv + i.simm) & 0xFFFFFFFF
                return None
        return None

    defB = None
    j = 0
    for j, i in enumerate(rest):
        if i.dest() == B:
            defB = i
            break
    if defB is None:
        return None
    if defB.mn == 'addu':
        for P in (defB.rs, defB.rt):
            cv = const_of(P, j + 1)
            if cv is not None:
                base = cv
                break
    elif defB.mn in ('lui', 'addiu'):
        base = const_of(B, j)
    if base is None:
        return None
    table = ram_addr((base + off) & 0xFFFFFFFF)
    # table size from sltiu on the index
    count = None
    for i in rest:
        if i.mn == 'sltiu' and i.simm > 0:
            count = i.simm
            break
    # locate table data (may be in this module or main)
    targets = []
    n = 0
    while True:
        if count is not None and n >= count:
            break
        if not (mod.base <= table + n * 4 < mod.end):
            break
        t = mod.word(table + n * 4)
        if not mod.reloc:
            t = ram_addr(t)
        if not mod.in_code(t):
            break
        if count is None and n > 512:
            break
        targets.append(t)
        n += 1
    if not targets:
        return None
    return targets


def trace_function(mod, start, strong, all_strong_global):
    fi = FuncInfo(start)
    work = [start]
    seen = set()
    flat = mod.flat_region(start) if mod.flat_region(start) and mod.flat_region(start)[0] == start else None
    fi.flat = flat

    def is_other_start(t):
        if flat and flat[0] <= t < flat[1]:
            return False
        return t != start and t in strong

    while work:
        a = work.pop()
        if a in seen:
            continue
        if not mod.in_code(a):
            fi.notes = getattr(fi, 'notes', []) + ['flow leaves code at %08x' % a]
            fi.bad = True
            continue
        seen.add(a)
        fi.body.add(a)
        i = mod.insn(a)
        k = i.kind
        if i.has_delay:
            fi.delay.add(a + 4)
        if k == 'branch':
            t = i.target
            if mod.in_code(t) and not is_other_start(t):
                work.append(t)
                fi.labels.add(t)
            else:
                fi.tails.add(t)
            if i.mn.endswith('al'):
                fi.calls.add(t)
            nxt = a + 8
            if is_other_start(nxt):
                fi.tails.add(nxt)
            else:
                work.append(nxt)
        elif k == 'jump':
            t = i.target
            if mod.in_code(t) and not is_other_start(t) and t not in all_strong_global:
                work.append(t)
                fi.labels.add(t)
            else:
                fi.tails.add(t)
        elif k == 'call' and flat and flat[0] <= i.target < flat[1]:
            # internal subroutine: control goes there, and comes back to a+8 through jr ra
            work.append(i.target)
            fi.labels.add(i.target)
            work.append(a + 8)
            fi.labels.add(a + 8)
            fi.flat_rets.add(a + 8)
        elif k == 'call':
            fi.calls.add(i.target)
            nxt = a + 8
            if is_other_start(nxt):
                fi.tails.add(nxt)
            else:
                work.append(nxt)
        elif k == 'callr':
            nxt = a + 8
            if is_other_start(nxt):
                fi.tails.add(nxt)
            else:
                work.append(nxt)
        elif k == 'jumpr':
            if i.rs == 31:
                pass
            else:
                tg = mod.jtables.get(a)
                if tg is None:
                    tg = resolve_jump_table(mod, a, None)
                    if tg is not None:
                        mod.jtables[a] = tg
                if tg:
                    for t in tg:
                        work.append(t)
                        fi.labels.add(t)
                else:
                    fi.dyn_jr.append(a)
        elif k == 'invalid':
            fi.bad = True  # emitted as a trap; flow stops
        else:
            nxt = a + 4
            if is_other_start(nxt):
                fi.tails.add(nxt)
            else:
                work.append(nxt)
    return fi


def looks_like_text(w):
    """all four bytes printable ASCII or NUL (string data between functions)"""
    return all(0x20 <= ((w >> s) & 0xFF) < 0x7F or ((w >> s) & 0xFF) == 0 for s in (0, 8, 16, 24))


def analyse(mods, extra, entry):
    main = mods[0]
    main.strong.add(entry)
    for name, addrs in extra.items():
        for m in mods:
            if m.name == name:
                m.strong.update(int(x, 16) for x in addrs)
    for m in mods:
        if m.reloc:
            m.strong.add(m.code_lo)      # offset 0: the init function the loader calls
        for lo, hi in m.flat:
            m.strong.add(lo)

    def mod_for(m, t):
        if m.reloc and t < 0x80000000:
            return m if m.in_code(t) else None
        if m.overlay and m.in_code(t):
            return m
        return main if main.in_code(t) else None

    # sweep starts: code following `jr ra` + delay slot (and nop padding)
    for m in mods:
        a = m.code_lo
        while a < m.code_hi - 8:
            if m.word(a) == 0x03E00008:
                b = a + 8
                while b < m.code_hi and m.word(b) == 0:
                    b += 4
                if (b < m.code_hi and m.insn(b).kind != 'invalid' and not looks_like_text(m.word(b))
                        and not (b + 4 < m.code_hi and looks_like_text(m.word(b + 4)))):
                    m.weak.add(b)
            a += 4
        m.weak.add(m.code_lo)

    overlays = [o for o in mods if o.overlay]

    def into_overlays(t, call):
        # a call/constant from elsewhere into fixed-overlay address space: a function start in every
        # overlay that has code there (a direct jal is enough evidence by itself)
        n = 0
        for o in overlays:
            if not o.in_code(t) or t in o.strong or looks_like_text(o.word(t)) or o.insn(t).kind == 'invalid':
                continue
            if call or plausible_start(o, t):
                o.strong.add(t)
                n += 1
        return n

    def promote(m, fi, weak):
        new = 0
        for t in fi.calls:
            tm = mod_for(m, t)
            if tm is None and not m.overlay and not weak:
                new += into_overlays(t, True)
                continue
            if tm is None or t in tm.strong:
                continue
            if weak and not plausible_start(tm, t):
                continue
            tm.strong.add(t)   # a jal target is always a function start
            new += 1
        for t in scan_constants(m, fi.body | fi.delay):
            tm = mod_for(m, t)
            if tm is None and not m.overlay and not weak:
                new += into_overlays(t, False)
                continue
            if tm is None or t in tm.strong:
                continue
            # an address constant is a function pointer only if it looks like a function start
            # (data such as strings sits between functions and is addressed the same way)
            if not plausible_start(tm, t, strict=True) or looks_like_text(tm.word(t)):
                continue
            if trace_function(tm, t, tm.strong, tm.strong).bad:
                continue
            tm.strong.add(t)
            new += 1
        return new

    def trace_all(m):
        m.funcs = {}
        m.jtables = {}
        m.strong = {a for a in m.strong if not m.inside_flat(a)}
        m.weak = {a for a in m.weak if not m.inside_flat(a)}
        for s in sorted(m.strong | m.weak):
            fi = trace_function(m, s, m.strong, m.strong)
            if fi.bad and s not in m.strong:
                m.weak.discard(s)          # data that only looked like code
                continue
            m.funcs[s] = fi

    rounds = 0
    while True:
        rounds += 1
        new = 0
        for m in mods:
            trace_all(m)
        for m in mods:
            for fi in list(m.funcs.values()):
                if not fi.bad:
                    new += promote(m, fi, fi.start not in m.strong)
        if new == 0 or rounds > 16:
            break
    # data words pointing into code (function pointer tables, callbacks) -> extra weak starts
    jt = defaultdict(set)
    for m in mods:
        for tg in m.jtables.values():
            jt[m.name].update(tg)
    covered = {}
    for m in mods:
        cov = set()
        for fi in m.funcs.values():
            cov |= fi.body
            cov |= fi.delay
        covered[m.name] = cov
    extra_weak = defaultdict(set)
    for m in mods:
        for off in range(0, len(m.data) - 3, 4):
            a = m.base + off
            if a in covered[m.name]:
                continue
            v = m.word(a) if m.reloc else ram_addr(m.word(a))
            if (m.reloc and m.relocation(a) == REL_WORD) or (m.overlay and m.in_code(v)):
                targets = [(m, v)]           # pointer into this module
            else:
                targets = [(main, v)]
            for tm, t in targets:
                if (tm.in_code(t) and t not in tm.funcs and t not in jt[tm.name] and t not in covered[tm.name]
                        and plausible_start(tm, t)):
                    extra_weak[tm.name].add(t)
    for m in mods:
        for w in sorted(extra_weak[m.name]):
            if m.inside_flat(w):
                continue
            fi = trace_function(m, w, m.strong, m.strong)
            if fi.bad:
                continue
            m.weak.add(w)
            m.funcs[w] = fi
        m.weak -= m.strong
    return rounds


BIOS_STUBS = (0x240A00A0, 0x240A00B0, 0x240A00C0)   # addiu t2, zero, 0xA0/0xB0/0xC0 (+ jr t2)


def plausible_start(m, t, strict=False):
    """does t look like the start of a function? strict: only after a jr ra (or a prologue)"""
    if not m.in_code(t):
        return False
    i = m.insn(t)
    if i.kind == 'invalid':
        return False
    if t == m.code_lo or (i.mn == 'addiu' and i.rt == 29 and i.rs == 29 and i.simm < 0):
        return True
    if m.word(t) in BIOS_STUBS and t + 8 <= m.end and m.word(t + 4) == 0x01400008:
        return True
    # the previous function ends right before t, possibly followed by nop padding
    k = t - 4
    for _ in range(4):
        if k - 4 < m.base:
            return False
        p = m.insn(k - 4)                    # is k the delay slot of an unconditional jump?
        if m.word(k - 4) == 0x03E00008 or (not strict and p.kind in ('jump', 'jumpr')):
            return True
        if m.word(k) != 0:
            return False
        k -= 4
    return False


# ----------------------------------------------------------------------------------------
# C emission
# ----------------------------------------------------------------------------------------
def R(n):
    return '0' if n == 0 else 'R[%d]' % n


def addr_expr(i):
    if i.rs == 0:
        return '0x%08Xu' % (i.simm & 0xFFFFFFFF)
    if i.simm == 0:
        return 'R[%d]' % i.rs
    return '(R[%d] + %du)' % (i.rs, i.simm & 0xFFFFFFFF) if i.simm > 0 else '(R[%d] - %du)' % (i.rs, -i.simm)


def emit_simple(i, mod):
    """C for a non-control-flow instruction (string, may be empty)."""
    m = i.mn
    rs, rt, rd, sa = i.rs, i.rt, i.rd, i.sa
    if i.word == 0:
        return ''
    r = mod.rel.get(i.addr) if mod.reloc else None
    if r is not None:
        return emit_relocated(i, r)

    def W(r, expr):
        if r == 0:
            return ''
        return 'R[%d] = %s;' % (r, expr)
    if m == 'sll':
        return W(rd, '%s << %d' % (R(rt), sa)) if sa else W(rd, R(rt))
    if m == 'srl':
        return W(rd, '%s >> %d' % (R(rt), sa)) if sa else W(rd, R(rt))
    if m == 'sra':
        return W(rd, '(uint32_t)((int32_t)%s >> %d)' % (R(rt), sa))
    if m == 'sllv':
        return W(rd, '%s << (%s & 31)' % (R(rt), R(rs)))
    if m == 'srlv':
        return W(rd, '%s >> (%s & 31)' % (R(rt), R(rs)))
    if m == 'srav':
        return W(rd, '(uint32_t)((int32_t)%s >> (%s & 31))' % (R(rt), R(rs)))
    if m == 'mfhi':
        return W(rd, 'c->hi')
    if m == 'mflo':
        return W(rd, 'c->lo')
    if m == 'mthi':
        return 'c->hi = %s;' % R(rs)
    if m == 'mtlo':
        return 'c->lo = %s;' % R(rs)
    if m == 'mult':
        return '{ int64_t m_ = (int64_t)(int32_t)%s * (int64_t)(int32_t)%s; c->lo = (uint32_t)m_; c->hi = (uint32_t)((uint64_t)m_ >> 32); }' % (R(rs), R(rt))
    if m == 'multu':
        return '{ uint64_t m_ = (uint64_t)%s * (uint64_t)%s; c->lo = (uint32_t)m_; c->hi = (uint32_t)(m_ >> 32); }' % (R(rs), R(rt))
    if m == 'div':
        return 'psx_div(c, %s, %s);' % (R(rs), R(rt))
    if m == 'divu':
        return 'psx_divu(c, %s, %s);' % (R(rs), R(rt))
    if m in ('add', 'addu'):
        if rt == 0:
            return W(rd, R(rs))
        if rs == 0:
            return W(rd, R(rt))
        return W(rd, '%s + %s' % (R(rs), R(rt)))
    if m in ('sub', 'subu'):
        return W(rd, '%s - %s' % (R(rs), R(rt)))
    if m == 'and':
        return W(rd, '%s & %s' % (R(rs), R(rt)))
    if m == 'or':
        if rt == 0:
            return W(rd, R(rs))
        if rs == 0:
            return W(rd, R(rt))
        return W(rd, '%s | %s' % (R(rs), R(rt)))
    if m == 'xor':
        return W(rd, '%s ^ %s' % (R(rs), R(rt)))
    if m == 'nor':
        return W(rd, '~(%s | %s)' % (R(rs), R(rt)))
    if m == 'slt':
        return W(rd, '(int32_t)%s < (int32_t)%s' % (R(rs), R(rt)))
    if m == 'sltu':
        return W(rd, '%s < %s' % (R(rs), R(rt)))
    if m in ('addi', 'addiu'):
        if rs == 0:
            return W(rt, '0x%Xu' % (i.simm & 0xFFFFFFFF))
        if i.simm == 0:
            return W(rt, R(rs))
        if i.simm > 0:
            return W(rt, '%s + %du' % (R(rs), i.simm))
        return W(rt, '%s - %du' % (R(rs), -i.simm))
    if m == 'slti':
        return W(rt, '(int32_t)%s < %d' % (R(rs), i.simm))
    if m == 'sltiu':
        return W(rt, '%s < 0x%Xu' % (R(rs), i.simm & 0xFFFFFFFF))
    if m == 'andi':
        return W(rt, '%s & 0x%Xu' % (R(rs), i.imm))
    if m == 'ori':
        return W(rt, '%s | 0x%Xu' % (R(rs), i.imm)) if rs else W(rt, '0x%Xu' % i.imm)
    if m == 'xori':
        return W(rt, '%s ^ 0x%Xu' % (R(rs), i.imm))
    if m == 'lui':
        return W(rt, '0x%08Xu' % (i.imm << 16))
    ae = addr_expr(i)
    if m == 'lb':
        return W(rt, '(uint32_t)(int32_t)(int8_t)MEM_R8(%s)' % ae) or 'MEM_R8(%s);' % ae
    if m == 'lbu':
        return W(rt, 'MEM_R8(%s)' % ae) or 'MEM_R8(%s);' % ae
    if m == 'lh':
        return W(rt, '(uint32_t)(int32_t)(int16_t)MEM_R16(%s)' % ae) or 'MEM_R16(%s);' % ae
    if m == 'lhu':
        return W(rt, 'MEM_R16(%s)' % ae) or 'MEM_R16(%s);' % ae
    if m == 'lw':
        return W(rt, 'MEM_R32(%s)' % ae) or 'MEM_R32(%s);' % ae
    if m == 'lwl':
        return W(rt, 'psx_lwl(%s, %s)' % (ae, R(rt)))
    if m == 'lwr':
        return W(rt, 'psx_lwr(%s, %s)' % (ae, R(rt)))
    if m == 'sb':
        return 'MEM_W8(%s, %s);' % (ae, R(rt))
    if m == 'sh':
        return 'MEM_W16(%s, %s);' % (ae, R(rt))
    if m == 'sw':
        return 'MEM_W32(%s, %s);' % (ae, R(rt))
    if m == 'swl':
        return 'psx_swl(%s, %s);' % (ae, R(rt))
    if m == 'swr':
        return 'psx_swr(%s, %s);' % (ae, R(rt))
    if m == 'lwc2':
        return 'gte_write_data(%d, MEM_R32(%s));' % (rt, ae)
    if m == 'swc2':
        return 'MEM_W32(%s, gte_read_data(%d));' % (ae, rt)
    if m == 'mfc0':
        return W(rt, 'cop0_read(c, %d)' % rd) or 'cop0_read(c, %d);' % rd
    if m == 'mtc0':
        return 'cop0_write(c, %d, %s);' % (rd, R(rt))
    if m == 'rfe':
        return 'cop0_rfe(c);'
    if m == 'mfc2':
        return W(rt, 'gte_read_data(%d)' % rd) or 'gte_read_data(%d);' % rd
    if m == 'cfc2':
        return W(rt, 'gte_read_ctrl(%d)' % rd) or 'gte_read_ctrl(%d);' % rd
    if m == 'mtc2':
        return 'gte_write_data(%d, %s);' % (rd, R(rt))
    if m == 'ctc2':
        return 'gte_write_ctrl(%d, %s);' % (rd, R(rt))
    if m == 'gte_cmd':
        return 'gte_command(0x%07Xu);' % (i.word & 0x1FFFFFF)
    if m == 'syscall':
        return 'psx_syscall(c, %s);' % Emitter.addr_c(mod, i.addr)
    if m == 'break':
        return 'psx_break(c, %s, %du);' % (Emitter.addr_c(mod, i.addr), (i.word >> 6) & 0xFFFFF)
    if m.startswith('cop0_'):
        return '/* %s */' % m
    if m == 'invalid':
        return 'psx_invalid(c, %s, 0x%08Xu); return;' % (Emitter.addr_c(mod, i.addr), i.word)
    raise ValueError('unhandled %s at %08x' % (m, i.addr))


def emit_relocated(i, r):
    """instruction whose immediate the game patches with the module's load address (BASE)"""
    t, target = r
    m = i.mn
    rs, rt = i.rs, i.rt

    def W(reg, expr):
        return '' if reg == 0 else 'R[%d] = %s;' % (reg, expr)
    if t == REL_HI16:
        if m != 'lui':
            raise ValueError('HI16 relocation on %s at %x' % (m, i.addr))
        return W(rt, 'RHI(0x%Xu)' % target)
    if t != REL_LO16:
        raise ValueError('relocation type %d on instruction %s at %x' % (t, m, i.addr))
    lo = 'RLO(0x%Xu)' % i.imm
    if m in ('addi', 'addiu'):
        return W(rt, lo if rs == 0 else '%s + %s' % (R(rs), lo))
    if m == 'ori':
        return W(rt, 'RLOU(0x%Xu)' % i.imm if rs == 0 else '%s | RLOU(0x%Xu)' % (R(rs), i.imm))
    ae = lo if rs == 0 else '(%s + %s)' % (R(rs), lo)
    loads = {'lb': '(uint32_t)(int32_t)(int8_t)MEM_R8(%s)', 'lbu': 'MEM_R8(%s)',
             'lh': '(uint32_t)(int32_t)(int16_t)MEM_R16(%s)', 'lhu': 'MEM_R16(%s)', 'lw': 'MEM_R32(%s)'}
    if m in loads:
        return W(rt, loads[m] % ae) or (loads[m] % ae) + ';'
    stores = {'sb': 'MEM_W8', 'sh': 'MEM_W16', 'sw': 'MEM_W32'}
    if m in stores:
        return '%s(%s, %s);' % (stores[m], ae, R(rt))
    if m == 'lwc2':
        return 'gte_write_data(%d, MEM_R32(%s));' % (rt, ae)
    if m == 'swc2':
        return 'MEM_W32(%s, gte_read_data(%d));' % (ae, rt)
    if m in ('lwl', 'lwr'):
        return W(rt, 'psx_%s(%s, %s)' % (m, ae, R(rt)))
    if m in ('swl', 'swr'):
        return 'psx_%s(%s, %s);' % (m, ae, R(rt))
    raise ValueError('LO16 relocation on %s at %x' % (m, i.addr))


GTE_CYCLES = {0x01: 15, 0x06: 8, 0x0C: 6, 0x10: 8, 0x11: 8, 0x12: 8, 0x13: 19, 0x14: 13, 0x16: 44, 0x1B: 17,
              0x1C: 11, 0x1E: 14, 0x20: 30, 0x28: 5, 0x29: 8, 0x2A: 17, 0x2D: 5, 0x2E: 6, 0x30: 23, 0x3D: 5,
              0x3E: 5, 0x3F: 39}


def insn_cycles(i):
    """Approximate R3000A cost: 1 cycle per instruction, RAM loads stall ~4 cycles,
    multiply/divide results are read right away by compiled code (full latency)."""
    if i.kind == 'load':
        return 5
    m = i.mn
    if m in ('mult', 'multu'):
        return 7
    if m in ('div', 'divu'):
        return 36
    if m == 'gte_cmd':
        return 1 + GTE_CYCLES.get(i.word & 0x3F, 8)
    return 1


def jmp_kind(m, s):
    # libc setjmp (sw ra, 0(a0) ... sw sp, 4(a0)) / longjmp (lw ra, 0(a0) ... lw sp, 4(a0))
    words = [m.word(s + 4 * k) for k in range(16)]
    if words[0] == 0xAC9F0000 and 0xAC9D0004 in words:
        return 'setjmp'
    if words[0] == 0x8C9F0000 and 0x8C9D0004 in words:
        return 'longjmp'
    return None


class Emitter:
    def __init__(self, mods):
        self.mods = mods
        self.main = mods[0]
        self.setjmps = {s for m in mods if not m.reloc for s in m.funcs if jmp_kind(m, s) == 'setjmp'}
        self.longjmps = {s for m in mods if not m.reloc for s in m.funcs if jmp_kind(m, s) == 'longjmp'}

    def fname(self, mod, a):
        return '%s_%08X' % (mod.prefix, a) if not mod.reloc else '%s_%05X' % (mod.prefix, a)

    @staticmethod
    def addr_c(mod, a):
        """C expression for the run-time address of module address/offset a"""
        return '(BASE + 0x%Xu)' % a if mod.reloc and a < 0x80000000 else '0x%08Xu' % a

    def call_expr(self, mod, t):
        """C statement calling address t (direct when statically known)."""
        if t in (0xA0, 0xB0, 0xC0) and not mod.reloc:      # BIOS entry points
            return 'call_addr(c, 0x%Xu);' % t
        own = not mod.reloc or t < 0x80000000
        if own and mod.in_code(t) and t in mod.funcs:
            return '%s(c);' % self.fname(mod, t)
        if mod is not self.main and self.main.in_code(t) and t in self.main.funcs:
            return '%s(c);' % self.fname(self.main, t)
        return 'call_addr(c, %s);' % self.addr_c(mod, t)

    def emit_function(self, mod, fi):
        out = []
        P = out.append
        name = self.fname(mod, fi.start)
        P('void %s(CPU *c) {' % name)
        P('  FUNC_ENTER(%s);' % self.addr_c(mod, fi.start))
        order = sorted(fi.body)
        if order[0] != fi.start:
            P('  goto L_%08X;' % fi.start)
            fi.labels.add(fi.start)
        labels = set(fi.labels)
        # any address that is a fallthrough target of a non-contiguous emission gets a label
        emitted_next = {}
        for k, a in enumerate(order):
            emitted_next[a] = order[k + 1] if k + 1 < len(order) else None
        # first pass to know which addresses need labels (gotos for discontinuities)
        need = set(labels)
        for a in order:
            i = mod.insn(a)
            if i.has_delay:
                ft = a + 8
            else:
                ft = a + 4
            if i.kind in ('jump', 'jumpr') or i.kind == 'invalid':
                continue
            if ft in fi.body and emitted_next.get(a) != ft:
                need.add(ft)
            if i.has_delay and (a + 4) in fi.body and emitted_next.get(a) == a + 4:
                # standalone copy of the delay slot is emitted next; fall through must skip it
                need.add(ft)
        cyc = 0

        def flush():
            nonlocal cyc
            if cyc:
                P('  c->cycles += %d;' % cyc)
                cyc = 0

        for a in order:
            i = mod.insn(a)
            if a in need:
                flush()
                P('L_%08X:' % a)
            body_c = None
            if not i.has_delay:
                cyc += insn_cycles(i)
                if i.mn == 'syscall':
                    flush()
                s = emit_simple(i, mod)
                if s:
                    P('  ' + s + '  /* %08x: %s */' % (a, i))
                ft = a + 4
                if i.kind == 'invalid':
                    continue
                if ft not in fi.body or emitted_next.get(a) != ft:
                    flush()
                    if ft in fi.body:
                        P('  goto L_%08X;' % ft)
                    else:
                        P('  ' + self.call_expr(mod, ft) + ' return;  /* fallthrough */')
                continue
            # control transfer with delay slot
            d = mod.insn(a + 4) if mod.in_code(a + 4) else None
            cyc += 1 + (insn_cycles(d) if d is not None else 1)
            ds = emit_simple(d, mod) if d is not None and not d.has_delay else ''
            if d is not None and d.has_delay:
                ds = '/* branch in delay slot ignored */'
            dcomment = '  /* %08x: %s */' % (a + 4, d) if d is not None else ''
            P('  /* %08x: %s */' % (a, i))
            k = i.kind
            m = i.mn
            ft = a + 8
            flush()
            if k == 'branch':
                cond = {
                    'beq': '%s == %s' % (R(i.rs), R(i.rt)),
                    'bne': '%s != %s' % (R(i.rs), R(i.rt)),
                    'blez': '(int32_t)%s <= 0' % R(i.rs),
                    'bgtz': '(int32_t)%s > 0' % R(i.rs),
                    'bltz': '(int32_t)%s < 0' % R(i.rs),
                    'bgez': '(int32_t)%s >= 0' % R(i.rs),
                    'bltzal': '(int32_t)%s < 0' % R(i.rs),
                    'bgezal': '(int32_t)%s >= 0' % R(i.rs),
                }[m]
                always = (m == 'beq' and i.rs == i.rt) or (m == 'bgez' and i.rs == 0)
                link = m.endswith('al')
                t = i.target
                P('  {')
                if not always:
                    P('    int br_ = %s;' % cond)
                if link:
                    P('    R[31] = %s;' % self.addr_c(mod, a + 8))
                if ds:
                    P('    ' + ds + dcomment)
                P('')
                guard = '' if always else 'if (br_) '
                if link:
                    P('    %s{ %s }' % (guard, self.call_expr(mod, t)))
                elif t in fi.body and t in fi.labels:
                    back = t <= a
                    if back:
                        P('    %s{ EVCHK(); goto L_%08X; }' % (guard, t))
                    else:
                        P('    %sgoto L_%08X;' % (guard, t))
                else:
                    P('    %s{ %s return; }' % (guard, self.call_expr(mod, t)))
                P('  }')
                if always and not link:
                    continue
            elif k == 'jump':
                t = i.target
                if ds:
                    P('  ' + ds + dcomment)
                if t in fi.body and t in fi.labels:
                    if t <= a:
                        P('  EVCHK(); goto L_%08X;' % t)
                    else:
                        P('  goto L_%08X;' % t)
                else:
                    P('  %s return;' % self.call_expr(mod, t))
                continue
            elif k == 'call' and fi.flat and fi.flat[0] <= i.target < fi.flat[1]:
                # flat region: internal subroutine, it comes back through jr ra (switch below)
                P('  R[31] = %s;' % self.addr_c(mod, a + 8))
                if ds:
                    P('  ' + ds + dcomment)
                P('  goto L_%08X;' % i.target)
                continue
            elif k == 'call':
                t = i.target
                P('  R[31] = %s;' % self.addr_c(mod, a + 8))
                if ds:
                    P('  ' + ds + dcomment)
                if t in self.setjmps and not mod.reloc:
                    # a guest longjmp comes back here (with the registers it restored)
                    P('  GUEST_SETJMP(%s)' % self.call_expr(mod, t))
                else:
                    P('  ' + self.call_expr(mod, t))
            elif k == 'callr':
                P('  { uint32_t t_ = %s;' % R(i.rs))
                if i.rd:
                    P('    R[%d] = %s;' % (i.rd, self.addr_c(mod, a + 8)))
                if ds:
                    P('    ' + ds + dcomment)
                P('')
                P('    call_addr(c, t_); }')
            elif k == 'jumpr':
                if i.rs == 31 and fi.flat and fi.flat_rets:
                    # flat region: return to an internal call site, or really return
                    P('  { uint32_t t_ = R[31];')
                    if ds:
                        P('    ' + ds + dcomment)
                    P('    switch (t_) {')
                    for t in sorted(fi.flat_rets):
                        P('      case %s: goto L_%08X;' % (self.addr_c(mod, t), t))
                    P('      default: return;')
                    P('    }')
                    P('  }')
                    continue
                if i.rs == 31:
                    if ds:
                        P('  ' + ds + dcomment)
                    if fi.start in self.longjmps and not mod.reloc:
                        P('  guest_longjmp(c); return;')   # back to the matching setjmp call
                        continue
                    P('  RETURN_CHECK(%s); return;' % self.addr_c(mod, a))
                    continue
                P('  { uint32_t t_ = %s;' % R(i.rs))
                if ds:
                    P('    ' + ds + dcomment)
                P('')
                tg = mod.jtables.get(a)
                if tg:
                    P('    switch (%s) {' % ('t_ - BASE' if mod.reloc else 'RAMADDR(t_)'))
                    for t in sorted(set(tg)):
                        P('      case 0x%08Xu: goto L_%08X;' % (t, t))
                    P('      default: jumptable_miss(c, %s, t_); return;' % self.addr_c(mod, a))
                    P('    }')
                else:
                    P('    call_addr(c, t_); return;')
                P('  }')
                continue
            # fallthrough after conditional branch / call
            if ft in fi.body:
                if emitted_next.get(a) != ft or (a + 4) in fi.body:
                    P('  goto L_%08X;' % ft)
            else:
                P('  ' + self.call_expr(mod, ft) + ' return;  /* fallthrough */')
        P('}')
        return '\n'.join(out)


HEADER = '''/* generated by tools/recomp.py - do not edit */
#include "psx_recomp.h"
#define R (c->r)
'''

RELOC_HEADER = '''/* relocatable module: BASE is its current load address */
#define BASE g_reloc_base[%d]
#define RHI(t) ((BASE + (t) + 0x8000u) & 0xFFFF0000u)
#define RLO(x) ((uint32_t)(int32_t)(int16_t)(((x) + BASE) & 0xFFFFu))
#define RLOU(x) (((x) + BASE) & 0xFFFFu)
'''


def check_words(m, fi):
    # two words of the function that the game never patches: (offset, word) pairs
    if not m.reloc:
        return [(0, m.word(fi.start)), (4, m.word(fi.start + 4))]
    picks = []
    for a in sorted(fi.body | fi.delay):
        if a < fi.start or a - fi.start > 0xFFF0 or m.relocation(a) is not None:
            continue
        w = m.word(a)
        if w == 0 and len(picks) == 0 and a + 4 in fi.body:
            continue
        picks.append((a - fi.start, w))
        if len(picks) == 2:
            break
    while len(picks) < 2:
        picks.append((0xFFFF, 0))
    return picks


def fnv1a(data):
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def overlay_ident(m, count=8):
    # code extent (the game always loads all of an overlay's code), its hash, and a few non-zero
    # words of it that quickly rule out other overlays at the same address
    end = max((max(fi.body | fi.delay) + 4 for fi in m.funcs.values()), default=m.base + 4)
    end = min(end, m.base + (len(m.data) & ~3))
    if m.ident_end:
        end = m.ident_end
    n = (end - m.base) // 4
    sig = []
    for k in range(count):
        a = m.base + ((n * k // count) * 4)
        while a < end and m.word(a) == 0:
            a += 4
        if a < end:
            sig.append((a - m.base, m.word(a)))
    return end - m.base, fnv1a(m.data[:end - m.base]), sorted(set(sig))


def overlay_signature(m, others, count=16):
    # words spread over a fixed-address overlay that identify it in RAM: non-zero and different
    # from what the other overlays covering the same address have there
    out = []
    size = len(m.data) & ~3
    for k in range(count):
        a = m.base + ((size * k // count) & ~3)
        while a < m.base + size:
            w = m.word(a)
            if w != 0 and not any(o.base <= a < o.base + (len(o.data) & ~3) and o.word(a) == w for o in others):
                out.append((a - m.base, w))
                break
            a += 4
    return sorted(set(out))


def signature(m, others, count=16):
    # words spread over a relocatable module that identify it in RAM: never patched, non-zero,
    # and different from the other modules' words at the same offset
    out = []
    size = len(m.data) & ~3
    for k in range(count):
        a = (size * k // count) & ~3
        while a < size:
            w = m.word(a)
            if (w != 0 and m.relocation(a) is None
                    and not any(o.reloc and a + 4 <= len(o.data) and o.word(a) == w for o in others)):
                out.append((a, w))
                break
            a += 4
    return sorted(set(out))


def generate(mods, outdir, per_file=60):
    em = Emitter(mods)
    os.makedirs(outdir, exist_ok=True)
    for f in os.listdir(outdir):
        if f.endswith('.c') or f.endswith('.h'):
            os.remove(os.path.join(outdir, f))
    decls = []
    files = []
    ovl_chunk, ovl_insns, ovl_n = [], 0, 0

    def flush_overlays():
        nonlocal ovl_chunk, ovl_insns, ovl_n
        if ovl_chunk:
            fn = 'overlays_%03d.c' % ovl_n
            with open(os.path.join(outdir, fn), 'w') as fh:
                fh.write(HEADER + '#include "funcs.h"\n\n' + '\n\n'.join(ovl_chunk) + '\n')
            files.append(fn)
            ovl_chunk, ovl_insns, ovl_n = [], 0, ovl_n + 1

    for m in mods:
        starts = sorted(m.funcs)
        for s in starts:
            decls.append('void %s(CPU *c);' % em.fname(m, s))
        if m.overlay:
            # fixed-address overlays are many and small: pack them into files of similar size
            for s in starts:
                ovl_chunk.append(em.emit_function(m, m.funcs[s]))
                ovl_insns += len(m.funcs[s].body)
            if ovl_insns >= 12000:
                flush_overlays()
            continue
        head = HEADER + (RELOC_HEADER % m.reloc_index if m.reloc else '') + '#include "funcs.h"\n\n'
        chunk = []
        n = 0
        for k, s in enumerate(starts):
            chunk.append(em.emit_function(m, m.funcs[s]))
            if len(chunk) >= per_file or k == len(starts) - 1:
                fn = '%s_%03d.c' % (m.name, n)
                with open(os.path.join(outdir, fn), 'w') as fh:
                    fh.write(head + '\n\n'.join(chunk) + '\n')
                files.append(fn)
                chunk = []
                n += 1
    flush_overlays()
    with open(os.path.join(outdir, 'funcs.h'), 'w') as fh:
        fh.write('#pragma once\n' + '\n'.join(decls) + '\n')
    # module tables
    t = [HEADER, '#include "funcs.h"\n']
    nreloc = sum(1 for m in mods if m.reloc)
    for m in mods:
        t.append('static const FuncEntry tab_%s[] = {' % m.name)
        for s in sorted(m.funcs):
            (c0, w0), (c1, w1) = check_words(m, m.funcs[s])
            t.append('  {0x%08Xu, %s, 0x%08Xu, 0x%08Xu, 0x%X, 0x%X},' % (s, em.fname(m, s), w0, w1, c0, c1))
        t.append('};')
        if m.reloc or m.overlay:
            if m.reloc:
                sig = signature(m, [o for o in mods if o is not m])
            else:
                m.hash_len, m.hash, sig = overlay_ident(m)
            t.append('static const uint32_t sig_%s[] = {%s};' % (
                m.name, ', '.join('0x%Xu, 0x%08Xu' % p for p in sig)))
            m.nsig = len(sig)
    t.append('const ModuleDesc g_modules[] = {')
    for m in mods:
        if m.overlay:
            t.append('  {"%s", MOD_OVERLAY, 0x%08Xu, 0x%08Xu, 0x%08Xu, 0x%08Xu, sig_%s, %d, -1, tab_%s, %d, 0x%08Xu, 0x%X},' % (
                m.name, m.base, m.end, m.code_lo, m.code_hi, m.name, m.nsig, m.name, len(m.funcs), m.hash, m.hash_len))
        elif m.reloc:
            t.append('  {"%s", MOD_RELOC, 0x%Xu, 0x%Xu, 0x%Xu, 0x%Xu, sig_%s, %d, %d, tab_%s, %d},' % (
                m.name, m.base, m.end, m.code_lo, m.code_hi, m.name, m.nsig, m.reloc_index, m.name, len(m.funcs)))
        else:
            t.append('  {"%s", MOD_FIXED, 0x%08Xu, 0x%08Xu, 0x%08Xu, 0x%08Xu, 0, 0, -1, tab_%s, %d},' % (
                m.name, m.base, m.end, m.code_lo, m.code_hi, m.name, len(m.funcs)))
    t.append('};')
    t.append('const int g_num_modules = %d;' % len(mods))
    t.append('uint32_t g_reloc_base[%d];' % max(nreloc, 1))
    t.append('const int g_num_reloc = %d;' % nreloc)
    with open(os.path.join(outdir, 'modules.c'), 'w') as fh:
        fh.write('\n'.join(t) + '\n')
    files.append('modules.c')
    return files


def main():
    mods, entry = load_modules()
    extra = {}
    xp = os.path.join(ROOT, 'tools', 'extra_entries.json')
    if os.path.exists(xp):
        extra = json.load(open(xp))
    rounds = analyse(mods, extra, entry)
    report = []
    for m in mods:
        nb = sum(len(f.body) for f in m.funcs.values())
        dyn = sum(len(f.dyn_jr) for f in m.funcs.values())
        report.append('%-9s funcs=%5d strong=%5d weak=%4d insns=%7d jtables=%4d dyn_jr=%d' % (
            m.name, len(m.funcs), len(m.strong), len(m.weak), nb, len(m.jtables), dyn))
    print('analysis rounds:', rounds)
    print('\n'.join(report))
    # dynamic jr listing for review
    os.makedirs(os.path.join(ROOT, 'build'), exist_ok=True)
    with open(os.path.join(ROOT, 'build', 'dyn_jr.txt'), 'w') as fh:
        for m in mods:
            for f in sorted(m.funcs.values(), key=lambda f: f.start):
                for a in f.dyn_jr:
                    fh.write('%s %08x in %08x: %s\n' % (m.name, a, f.start, m.insn(a)))
    files = generate(mods, os.path.join(ROOT, 'generated'))
    print('generated %d files' % len(files))


if __name__ == '__main__':
    main()


def coverage_report(mods):
    for m in mods:
        cov = set()
        for f in m.funcs.values():
            cov |= f.body
            cov |= f.delay
        gaps = []
        a = m.code_lo
        start = None
        while a < m.code_hi:
            if a not in cov and m.word(a) != 0:
                if start is None:
                    start = a
            else:
                if start is not None:
                    gaps.append((start, a))
                    start = None
            a += 4
        if start is not None:
            gaps.append((start, m.code_hi))
        tot = sum((e - s) // 4 for s, e in gaps)
        print('%-9s uncovered nonzero words: %d in %d gaps' % (m.name, tot, len(gaps)))
        for s, e in gaps[:400]:
            if (e - s) >= 8:
                print('    %08x-%08x (%d)' % (s, e, (e - s) // 4))
