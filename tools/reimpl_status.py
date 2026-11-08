#!/usr/bin/env python3
"""How much of the game code is reimplemented (docs/reimpl.md).

usage: reimpl_status.py [--exe game/nanpa2.exe] [--doc docs/reimpl.md] [--list]

Game code is what tools/recomp.py lifts (0x401000-0x4077a0; the MP3 decoder
library after it and the MSVC runtime from 0x412c90 are not lifted). The
code is walked twice from the same roots (WinMain, the window procedures, the
code pointers in .data), once with no overrides and once with
recomp.HLE_OVERRIDES (which includes GAME_OVERRIDES and the opcode handlers
of GAME_JUMP_OVERRIDES) as barriers; jump
tables are followed only from a reachable indirect jump. Bytes reachable in
the first walk but not the second are no longer needed: replaced. Lifted
oracle copies of reimplemented functions do not count as needed.

Functions are the direct call targets plus the extra entries; each
instruction belongs to the nearest entry at or below it, and an entry that
the code before it falls into is an alternate entry of that function.
Modules come from the inventory table in docs/reimpl.md (first column the
address, third the module). --list prints the table rows (address, bytes,
callers, callees, imports) as a starting point for the inventory.
"""
import argparse
import bisect
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import recomp  # noqa: E402
import capstone  # noqa: E402
from capstone import x86 as X  # noqa: E402


def imports(pe):
    """IAT slot address -> 'dll!name'"""
    d = pe.d
    off = struct.unpack_from('<I', d, 0x3c)[0]
    rva = struct.unpack_from('<I', d, off + 0x80)[0]
    out = {}
    if not rva:
        return out
    p = pe.base + rva
    while True:
        ilt, _, _, name, iat = struct.unpack('<IIIII', pe.read(p, 20))
        if not name:
            break
        dll = pe.read(pe.base + name, 64).split(b'\0')[0].decode()
        k = 0
        while True:
            v = pe.u32(pe.base + (ilt or iat) + 4 * k)
            if not v:
                break
            if v & 0x80000000:
                fn = '#%d' % (v & 0xffff)
            else:
                fn = pe.read(pe.base + v + 2, 64).split(b'\0')[0].decode()
            out[pe.base + iat + 4 * k] = '%s!%s' % (dll.split('.')[0].upper(), fn)
            k += 1
        p += 20
    return out


class Walker:
    def __init__(self, pe):
        self.pe = pe
        self.cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.cs.detail = True
        self.cache = {}
        self.table_len = {}
        lf = recomp.Lifter(pe)
        for t in recomp.TEXT_TABLES:
            self.table_len[t] = lf.table_len(t)

    def insn(self, a):
        if a not in self.cache:
            self.cache[a] = next(self.cs.disasm(self.pe.read(a, 16), a), None)
        return self.cache[a]

    def roots(self):
        r = list(recomp.EXTRA_ENTRIES) + [0x402090]
        for a, b in recomp.DATA_TABLE_RANGES:
            for p in range(a, b, 4):
                v = self.pe.u32(p)
                if recomp.in_code(v):
                    r.append(v)
        return r

    def walk(self, barriers, roots=None):
        """address -> insn for all code reachable from the roots"""
        seen = {}
        work = list(self.roots() if roots is None else roots)
        while work:
            a = work.pop()
            while recomp.in_code(a) and a not in seen and a not in barriers:
                i = self.insn(a)
                seen[a] = i
                m, ops = i.mnemonic, i.operands
                nxt = a + i.size
                if m == 'jmp':
                    if ops[0].type == X.X86_OP_IMM:
                        work.append(ops[0].imm)
                    elif ops[0].type == X.X86_OP_MEM and ops[0].mem.disp in self.table_len:
                        t = ops[0].mem.disp
                        work += [self.pe.u32(t + 4 * k) for k in range(self.table_len[t])]
                    break
                if m in ('ret', 'retf', 'int3', 'hlt'):
                    break
                if m.startswith('j') or m.startswith('loop'):
                    work.append(ops[0].imm)
                elif m == 'call' and ops[0].type == X.X86_OP_IMM:
                    work.append(ops[0].imm)
                # immediates naming code (window procedures, callbacks)
                for op in ops:
                    if op.type == X.X86_OP_IMM and m != 'call' and not m.startswith('j') \
                            and op.imm in (recomp.EXTRA_ENTRIES + [0x4028c0]):
                        work.append(op.imm)
                a = nxt
        return seen


SRC_GAME = os.path.join(os.path.dirname(HERE), 'src', 'game')
# Where the C side stops being "the fallback": the fetch of the VM loop is
# C (loop.c); the click path that precedes it is the native part.
FALLBACK_STOPS = {0x40616e}


def native_calls():
    """Native game routines the C side calls (game_ncall / ncall / blit with
    a game code address or a #define of one): they run, so they are needed."""
    out = {}
    for fn in sorted(os.listdir(SRC_GAME)):
        if not fn.endswith('.c'):
            continue
        src = open(os.path.join(SRC_GAME, fn), encoding='utf-8').read()
        defs = dict((m.group(1), int(m.group(2), 16)) for m in
                    re.finditer(r'#define\s+(\w+)\s+(0x[0-9a-fA-F]+)u?\b', src))
        for m in re.finditer(r'\b(?:game_ncall|ncall|blit)\(\s*(\w+)', src):
            a = m.group(1)
            v = int(a, 16) if a.startswith('0x') else defs.get(a)
            if v is not None and recomp.in_code(v):
                out.setdefault(v, set()).add(fn)
    return out


def native_fallbacks():
    """Units whose `native` predicate sends cases to the original code
    (units.c: JUMP_SYS / CALL_NATIVE / PIECE with a last argument other than
    NULL): their original body still runs."""
    out = []
    src = open(os.path.join(SRC_GAME, 'units.c'), encoding='utf-8').read()
    for m in re.finditer(r'^\s*(JUMP_SYS|CALL_NATIVE|PIECE)\((0x[0-9a-f]+),.*,\s*(\w+)\),?\s*$',
                         src, re.M):
        if m.group(3) != 'NULL':
            out.append(int(m.group(2), 16))
    return out


def load_modules(doc):
    mods, purpose = {}, {}
    if not os.path.exists(doc):
        return mods, purpose
    for line in open(doc, encoding='utf-8'):
        m = re.match(r'\|\s*`?0x([0-9a-f]{6})`?\s*\|[^|]*\|\s*([^|]+?)\s*\|\s*([^|]*?)\s*\|', line)
        if m:
            mods[int(m.group(1), 16)] = m.group(2)
            purpose[int(m.group(1), 16)] = m.group(3)
    return mods, purpose


def main():
    ap = argparse.ArgumentParser()
    root = os.path.dirname(HERE)
    ap.add_argument('--exe', default=os.path.join(root, 'game', 'nanpa2.exe'))
    ap.add_argument('--doc', default=os.path.join(root, 'docs', 'reimpl.md'))
    ap.add_argument('--list', action='store_true')
    args = ap.parse_args()

    pe = recomp.PE(args.exe)
    w = Walker(pe)
    full = w.walk(set())
    barriers = set(recomp.HLE_OVERRIDES)
    live = w.walk(barriers)
    # what the C side still runs natively counts as not done: routines it
    # calls (game_ncall) and the original bodies behind its native predicates
    ncalls = native_calls()
    extra = [a for a in ncalls if a not in barriers]
    live.update(w.walk(barriers, extra))
    fallbacks = native_fallbacks()
    for fb in fallbacks:
        live.update(w.walk((barriers - {fb}) | FALLBACK_STOPS, [fb]))
    iat = imports(pe)

    # functions
    entries = set(recomp.EXTRA_ENTRIES) | {0x402090}
    for a, i in full.items():
        if i.mnemonic == 'call' and i.operands[0].type == X.X86_OP_IMM and recomp.in_code(i.operands[0].imm):
            entries.add(i.operands[0].imm)
    entries = sorted(e for e in entries if e in full)
    addrs = sorted(full)
    alt = set()
    for e in entries:
        k = bisect.bisect_left(addrs, e)
        if k:
            p = full[addrs[k - 1]]
            if p.address + p.size == e and p.mnemonic not in ('ret', 'jmp', 'int3', 'hlt'):
                alt.add(e)
    heads = [e for e in entries if e not in alt]
    # ownership: flood from each head through fallthrough, jumps and jump
    # tables (not calls), stopping at other heads; heads in address order,
    # first claim wins (the VM loop 0x4060d4 owns its opcode handlers).
    # Code only reachable from .data code pointers gets its own heads.
    hs = set(heads)
    owner = {}

    def flood(h):
        work = [h]
        while work:
            a = work.pop()
            while a in full and a not in owner and (a == h or a not in hs):
                owner[a] = h
                i = full[a]
                m, ops = i.mnemonic, i.operands
                if m in ('ret', 'retf', 'int3', 'hlt'):
                    break
                if m == 'jmp':
                    if ops[0].type == X.X86_OP_IMM:
                        work.append(ops[0].imm)
                    elif ops[0].type == X.X86_OP_MEM and ops[0].mem.disp in w.table_len:
                        t = ops[0].mem.disp
                        work += [pe.u32(t + 4 * k) for k in range(w.table_len[t])]
                    break
                if (m.startswith('j') or m.startswith('loop')) and ops[0].type == X.X86_OP_IMM:
                    work.append(ops[0].imm)
                a += i.size

    for h in heads:
        flood(h)
    for r in sorted(w.roots()):
        if r in full and r not in owner:
            heads.append(r)
            hs.add(r)
            flood(r)
    heads.sort()
    for a in addrs:  # anything left: the nearest head below
        if a not in owner:
            owner[a] = heads[bisect.bisect_right(heads, a) - 1]
    funcs = {h: {'bytes': 0, 'live': 0, 'callers': set(), 'callees': set(), 'imports': set(),
                 'alt': sorted(e for e in alt if owner.get(e) == h)} for h in heads}
    for a in addrs:
        i = full[a]
        f = funcs[owner[a]]
        f['bytes'] += i.size
        if a in live:
            f['live'] += i.size
        if i.mnemonic == 'call':
            op = i.operands[0]
            if op.type == X.X86_OP_IMM:
                t = op.imm
                if t in owner:
                    funcs[owner[t]]['callers'].add(owner[a])
                    f['callees'].add(owner[t])
                else:
                    f['callees'].add(t)
            elif op.type == X.X86_OP_MEM and op.mem.disp in iat:
                f['imports'].add(iat[op.mem.disp].split('!')[1])

    mods, purpose = load_modules(args.doc)
    total = sum(f['bytes'] for f in funcs.values())
    left = sum(f['live'] for f in funcs.values())

    if args.list:
        for h in heads:
            f = funcs[h]
            ov = 'game' if h in recomp.ORACLES else 'hle' if h in recomp.HLE_OVERRIDES else \
                'dead' if not f['live'] else ''
            print('| 0x%06x | %d | %s | %s | %s | %s | %s |%s' % (
                h, f['bytes'], mods.get(h, '?'), purpose.get(h, ''),
                ' '.join('%x' % c for c in sorted(f['callers'])),
                ' '.join('%x' % c for c in sorted(f['callees'])) +
                (' ' + ' '.join(sorted(f['imports'])) if f['imports'] else ''), ov,
                ' alt ' + ' '.join('%x' % e for e in f['alt']) if f['alt'] else ''))
        return

    def done(f):
        return f['live'] == 0

    print('game code: %d bytes in %d functions (0x%x-0x%x)' % (total, len(heads), addrs[0],
                                                              addrs[-1] + full[addrs[-1]].size))
    nrep = sum(1 for f in funcs.values() if done(f))
    print('replaced:  %d bytes (%.1f%%), %d functions (%.1f%%)' % (
        total - left, 100.0 * (total - left) / total, nrep, 100.0 * nrep / len(heads)))
    hle_only = w.walk(set(recomp.HLE_OVERRIDES) - recomp.ORACLES)
    hb = total - sum(i.size for a, i in hle_only.items())
    print('  by HLE overrides (sound, timer, pump, decompressor, logo): %d bytes' % hb)
    print('  by the reimplemented game logic (src/game, %d functions and %d opcode handlers):'
          ' %d bytes (%.1f%%)' % (len(recomp.GAME_OVERRIDES), len(recomp.GAME_JUMP_OVERRIDES),
                                  total - left - hb, 100.0 * (total - left - hb) / total))
    print('  still run natively from the C: %s called by src/game (game_ncall), %s original bodies behind '
          'native predicates' % (' '.join('%x' % a for a in sorted(extra)),
                                 ' '.join('%x' % a for a in sorted(fallbacks))))
    print()
    print('%-12s %5s %5s %7s %7s %6s' % ('module', 'funcs', 'done', 'bytes', 'done', '%'))
    per = {}
    for h in heads:
        m = mods.get(h, 'unclassified')
        p = per.setdefault(m, [0, 0, 0, 0])
        p[0] += 1
        p[1] += done(funcs[h])
        p[2] += funcs[h]['bytes']
        p[3] += funcs[h]['bytes'] - funcs[h]['live']
    for m in sorted(per, key=lambda k: -per[k][2]):
        p = per[m]
        print('%-12s %5d %5d %7d %7d %5.1f%%' % (m, p[0], p[1], p[2], p[3], 100.0 * p[3] / p[2]))


if __name__ == '__main__':
    main()
