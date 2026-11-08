#!/usr/bin/env python3
"""Static recompiler: lift nanpa2.exe game code (x86, 0x401000-0x407800) to C.

usage: recomp.py EXE OUT.c [--hle LIST]

The output defines `void x86_exec(uint32_t pc)`: one function holding every
basic block as a label, with x86 state in the global `cpu` (src/x86/x86.h) and
the native stack in emulated memory. Direct jumps become gotos; returns and
indirect jumps go through a switch on the target address, so the original's
stack tricks (RPN evaluation on the stack, `mov esp,[saved]` unwinding,
jump tables) behave as on x86.

Calls to addresses outside the lifted code, and to lifted functions replaced
by hand-written code, go to `hle_call(addr)` (src/hle). The return address is
pushed first, as on x86; hle_call pops it, runs, and leaves EAX.

The generated file is a translation of the game's code: it is written into
the build folder, never into the source tree, and never committed.
"""
import struct
import sys

import capstone
from capstone import x86 as X

CODE_START, CODE_END = 0x401000, 0x407800

# Lifted functions replaced by hand-written code (src/hle/*.c). Calls to and
# dispatches into these addresses go to hle_call().
HLE_OVERRIDES = {
    0x401e20: 'logo movie (MCI avivideo)',
    0x401bf0: 'message pump',
    0x4028c0: 'multimedia timer callback (x87 palette fade)',
    0x402ad0: 'DirectSound init',
    0x402c70: 'sound shutdown',
    0x402f10: 'find WAV data chunk',
    0x402f60: 'DirectSound buffer write',
    0x4031d0: 'MP3 decode into buffer',
    0x403220: 'music pause',
    0x403250: 'music resume',
    0x4032c0: 'music stop',
    0x403440: 'voice stop',
    0x4034a0: 'music stream pump',
    0x4075f0: 'GP4/ANI decompressor (src/lzh.c)',
    0x402cb0: 'play music N (MUSIC\\%02d.MP3 through the linked MP3 decoder)',
    0x403360: 'play voice (SPEECH%s through DirectSound)',
}

# Game logic reimplemented in C (src/game, docs/reimpl.md). They are
# overrides like the ones above, but their lifted bodies are kept as the
# differential-test oracle: reachable only through the tagged entry
# ORACLE_TAG | addr (x86_exec), never from game code.
GAME_OVERRIDES = {
    0x4037fc: 'script expression evaluator (src/game/expr.c)',
    0x4039f7: 'script constant reader (src/game/expr.c)',
    0x406455: 'script flag write (src/game/expr.c)',
}
HLE_OVERRIDES.update(GAME_OVERRIDES)
ORACLE_TAG = 0x40000000  # src/x86/x86.h X86_ORACLE_TAG

# Known jump tables in .text: base -> number of entries (computed if None)
TEXT_TABLES = [0x40247c, 0x402508, 0x402550, 0x403521, 0x40368b, 0x40386d,
               0x40620b, 0x406262, 0x406922, 0x407216, 0x4072b7]
# Code-pointer tables in .data (dwords that point into lifted code)
DATA_TABLE_RANGES = [(0x435576, 0x4355fa), (0x435dfc, 0x435e60)]

# Timing probes (x86_probe, logged with NANPA2_PROBE=1) before these
# instructions: wipe 0x401a50 entry and its return in opcode 0x407386, the
# two text-character draws (0x4062ff ASCII, 0x40636d Big5), and the
# wait-for-click at 0x406acc; 0x40616e is the script VM's opcode fetch
# (logged with NANPA2_VMTRACE=1); 0x4071bb is inside the script hit test
# 0x4071a4 with edi = its rectangle table (NANPA2_HOTSPOTS=1).
PROBES = {0x40616e, 0x401a50, 0x40738b, 0x4062ff, 0x40636d, 0x406acc, 0x4071bb}

# entry points only referenced as immediates: WndProc, the logo window
# subclass proc
EXTRA_ENTRIES = [0x401000, 0x4021c0, 0x401fd0]


class PE:
    def __init__(self, path):
        d = open(path, 'rb').read()
        self.d = d
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        n = struct.unpack_from('<H', d, pe + 6)[0]
        self.base = struct.unpack_from('<I', d, pe + 52)[0]
        self.entry = self.base + struct.unpack_from('<I', d, pe + 40)[0]
        secs = pe + 24 + struct.unpack_from('<H', d, pe + 20)[0]
        self.secs = []
        for i in range(n):
            o = secs + i * 40
            vs, va, rs, ro = struct.unpack_from('<IIII', d, o + 8)
            self.secs.append((self.base + va, vs, rs, ro))

    def read(self, va, n):
        for sva, vs, rs, ro in self.secs:
            if sva <= va < sva + max(vs, rs):
                off = va - sva
                b = self.d[ro + off: ro + min(rs, off + n)]
                return b + b'\0' * (n - len(b))
        raise KeyError(hex(va))

    def u32(self, va):
        return struct.unpack('<I', self.read(va, 4))[0]


def in_code(a):
    return CODE_START <= a < CODE_END


# ---------------------------------------------------------------- operands

REG32 = {'eax': 0, 'ecx': 1, 'edx': 2, 'ebx': 3, 'esp': 4, 'ebp': 5, 'esi': 6, 'edi': 7}
REG16 = {'ax': 0, 'cx': 1, 'dx': 2, 'bx': 3, 'sp': 4, 'bp': 5, 'si': 6, 'di': 7}
REG8 = {'al': (0, 0), 'cl': (1, 0), 'dl': (2, 0), 'bl': (3, 0),
        'ah': (0, 8), 'ch': (1, 8), 'dh': (2, 8), 'bh': (3, 8)}
RNAME = ['EAX', 'ECX', 'EDX', 'EBX', 'ESP', 'EBP', 'ESI', 'EDI']


class Unsupported(Exception):
    pass


class Lifter:
    def __init__(self, pe):
        self.pe = pe
        self.cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.cs.detail = True
        self.insns = {}          # addr -> insn
        self.labels = set()      # block starts (dispatchable)
        self.hle_targets = set()
        self.unsupported = {}
        self.tables = {}

    # ------------------------------------------------------------ traversal
    def table_len(self, base):
        n = 0
        while True:
            v = self.pe.u32(base + 4 * n)
            if not in_code(v) or (n and (base + 4 * n) in TEXT_TABLES):
                return n
            n += 1

    def explore(self):
        work = list(EXTRA_ENTRIES)
        for t in TEXT_TABLES:
            n = self.table_len(t)
            self.tables[t] = n
            work += [self.pe.u32(t + 4 * i) for i in range(n)]
        for a, b in DATA_TABLE_RANGES:
            for p in range(a, b, 4):
                v = self.pe.u32(p)
                if in_code(v):
                    work.append(v)
        # the CRT entry calls WinMain (0x402090); start there too
        work.append(0x402090)
        # oracle bodies of the reimplemented functions
        work += list(GAME_OVERRIDES)
        for w in work:
            self.labels.add(w)
        seen = set()
        while work:
            a = work.pop()
            if a in seen or not in_code(a) or (a in HLE_OVERRIDES and a not in GAME_OVERRIDES):
                continue
            while True:
                if a in seen:
                    break
                seen.add(a)
                if a in self.table_ranges():
                    raise Unsupported('decoding into table at %x' % a)
                code = self.pe.read(a, 16)
                insn = next(self.cs.disasm(code, a), None)
                if insn is None:
                    raise Unsupported('cannot decode at %x' % a)
                self.insns[a] = insn
                nxt = a + insn.size
                m = insn.mnemonic
                ops = insn.operands
                # immediates that look like code pointers (callbacks, wndproc)
                for op in ops:
                    if op.type == X.X86_OP_IMM and in_code(op.imm) and m not in ('call', 'jmp') \
                            and not m.startswith('j') and not m.startswith('loop'):
                        if op.imm in self.pe_code_starts():
                            work.append(op.imm)
                            self.labels.add(op.imm)
                if m == 'jmp':
                    if ops[0].type == X.X86_OP_IMM:
                        self.labels.add(ops[0].imm)
                        work.append(ops[0].imm)
                    break
                if m.startswith('j') or m.startswith('loop'):
                    self.labels.add(ops[0].imm)
                    self.labels.add(nxt)
                    work.append(ops[0].imm)
                    a = nxt
                    continue
                if m == 'call':
                    self.labels.add(nxt)
                    if ops[0].type == X.X86_OP_IMM:
                        t = ops[0].imm
                        if in_code(t) and t not in HLE_OVERRIDES:
                            self.labels.add(t)
                            work.append(t)
                        else:
                            self.hle_targets.add(t)
                    a = nxt
                    continue
                if m in ('ret', 'retf', 'int3', 'hlt'):
                    break
                a = nxt
        self.check_oracles()
        return seen

    def body(self, entry):
        """Instructions reachable from entry without following calls."""
        out, work = set(), [entry]
        while work:
            a = work.pop()
            if a in out or a not in self.insns:
                continue
            out.add(a)
            insn = self.insns[a]
            m, ops = insn.mnemonic, insn.operands
            if m in ('ret', 'retf', 'int3', 'hlt'):
                continue
            if m == 'jmp':
                if ops[0].type == X.X86_OP_IMM:
                    work.append(ops[0].imm)
                else:  # jump table: its targets were seeded from TEXT_TABLES
                    mm = ops[0].mem if ops[0].type == X.X86_OP_MEM else None
                    if mm is not None and mm.disp in self.tables:
                        work += [self.pe.u32(mm.disp + 4 * i) for i in range(self.tables[mm.disp])]
                continue
            if (m.startswith('j') or m.startswith('loop')) and ops[0].type == X.X86_OP_IMM:
                work.append(ops[0].imm)
            work.append(a + insn.size)
        return out

    def check_oracles(self):
        # A direct jump into a reimplemented entry from outside its own body
        # would become a goto past the override; refuse to build that.
        self.oracle_body = {}
        for e in GAME_OVERRIDES:
            self.oracle_body[e] = self.body(e)
        for a, insn in self.insns.items():
            m, ops = insn.mnemonic, insn.operands
            if (m == 'jmp' or m.startswith('j') or m.startswith('loop')) and ops \
                    and ops[0].type == X.X86_OP_IMM and ops[0].imm in GAME_OVERRIDES \
                    and a not in self.oracle_body[ops[0].imm]:
                raise Unsupported('jump into reimplemented %x from %x' % (ops[0].imm, a))

    def table_ranges(self):
        r = set()
        for t, n in self.tables.items():
            r.update(range(t, t + 4 * n))
        return r

    def pe_code_starts(self):
        # immediates are only followed when they hit a known label or table target
        return self.labels

    # ------------------------------------------------------------ operand C
    def mem_addr(self, insn, op):
        mm = op.mem
        parts = []
        if mm.base:
            parts.append(self.reg_expr(insn.reg_name(mm.base)))
        if mm.index:
            idx = self.reg_expr(insn.reg_name(mm.index))
            parts.append(idx if mm.scale == 1 else '%s*%du' % (idx, mm.scale))
        if mm.disp or not parts:
            parts.append('0x%xu' % (mm.disp & 0xffffffff))
        return '(uint32_t)(' + ' + '.join(parts) + ')'

    def reg_expr(self, name):
        if name in REG32:
            return 'cpu.r[%s]' % RNAME[REG32[name]]
        if name in REG16:
            return '(cpu.r[%s] & 0xffffu)' % RNAME[REG16[name]]
        if name in REG8:
            i, sh = REG8[name]
            return '((cpu.r[%s] >> %d) & 0xffu)' % (RNAME[i], sh)
        raise Unsupported('register ' + name)

    def get(self, insn, op, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg_expr(insn.reg_name(op.reg))
        if op.type == X.X86_OP_IMM:
            return '0x%xu' % (op.imm & ((1 << (8 * size)) - 1))
        if op.type == X.X86_OP_MEM:
            return 'rd%d(%s)' % (8 * size, self.mem_addr(insn, op))
        raise Unsupported('operand')

    def put(self, insn, op, val):
        if op.type == X.X86_OP_REG:
            name = insn.reg_name(op.reg)
            if name in REG32:
                return 'cpu.r[%s] = (uint32_t)(%s);' % (RNAME[REG32[name]], val)
            if name in REG16:
                return 'set16(%s, %s);' % (RNAME[REG16[name]], val)
            if name in REG8:
                i, sh = REG8[name]
                return 'set8%s(%s, %s);' % ('h' if sh else 'l', RNAME[i], val)
        if op.type == X.X86_OP_MEM:
            return 'wr%d(%s, %s);' % (8 * op.size, self.mem_addr(insn, op), val)
        raise Unsupported('dest operand')

    # ------------------------------------------------------------ emit
    def goto(self, target, here=None):
        if target in self.insns:
            # backward jumps poll the host (timer ticks, screen), since the
            # native code spins on counters its timer thread updates
            poll = 'X86_POLL(0x%xu); ' % here if here is not None and target <= here else ''
            return poll + 'goto L_%x;' % target
        if target in HLE_OVERRIDES or not in_code(target):
            return 'pc = 0x%xu; goto dispatch;' % target
        raise Unsupported('jump to unlifted %x' % target)

    COND = {
        'jo': 'cpu.of', 'jno': '!cpu.of', 'jb': 'cpu.cf', 'jae': '!cpu.cf',
        'je': 'cpu.zf', 'jne': '!cpu.zf', 'jbe': '(cpu.cf || cpu.zf)', 'ja': '(!cpu.cf && !cpu.zf)',
        'js': 'cpu.sf', 'jns': '!cpu.sf', 'jp': 'cpu.pf', 'jnp': '!cpu.pf',
        'jl': '(cpu.sf != cpu.of)', 'jge': '(cpu.sf == cpu.of)',
        'jle': '(cpu.zf || cpu.sf != cpu.of)', 'jg': '(!cpu.zf && cpu.sf == cpu.of)',
    }

    def lift(self, insn):
        m = insn.mnemonic
        rep_kw = None
        if ' ' in m:  # capstone spells prefixes into the mnemonic: "rep movsd"
            rep_kw, m = m.split(' ', 1)
        ops = insn.operands
        nxt = insn.address + insn.size
        bits = ops[0].size * 8 if ops else 32
        g = lambda i, s=None: self.get(insn, ops[i], s)
        p = lambda i, v: self.put(insn, ops[i], v)

        if m == 'nop':
            return ''
        if m == 'mov':
            return p(0, g(1))
        if m in ('movzx', 'movsx'):
            src = g(1)
            if m == 'movsx':
                sb = ops[1].size * 8
                src = '(uint32_t)(int32_t)(int%d_t)(%s)' % (sb, src)
            return p(0, src)
        if m == 'lea':
            return p(0, self.mem_addr(insn, ops[1]))
        if m in ('add', 'adc', 'sub', 'sbb', 'cmp'):
            carry = 'cpu.cf' if m in ('adc', 'sbb') else '0'
            fn = 'f_add' if m in ('add', 'adc') else 'f_sub'
            b = g(1, ops[0].size) if ops[1].type != X.X86_OP_IMM else \
                '0x%xu' % (ops[1].imm & ((1 << bits) - 1))
            e = '%s(%s, %s, %s, %d)' % (fn, g(0), b, carry, bits)
            return e + ';' if m == 'cmp' else p(0, e)
        if m in ('and', 'or', 'xor', 'test'):
            op = {'and': '&', 'or': '|', 'xor': '^', 'test': '&'}[m]
            b = g(1) if ops[1].type != X.X86_OP_IMM else '0x%xu' % (ops[1].imm & ((1 << bits) - 1))
            e = 'f_logic((%s) %s (%s), %d)' % (g(0), op, b, bits)
            return e + ';' if m == 'test' else p(0, e)
        if m in ('inc', 'dec'):
            return p(0, 'f_%s(%s, %d)' % (m, g(0), bits))
        if m == 'neg':
            return p(0, 'f_neg(%s, %d)' % (g(0), bits))
        if m == 'not':
            return p(0, '~(%s)' % g(0))
        if m in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr'):
            cnt = '1u' if len(ops) == 1 else g(1)
            fn = 'shl' if m == 'sal' else m
            return p(0, 'f_%s(%s, %s, %d)' % (fn, g(0), cnt, bits))
        if m in ('shld', 'shrd'):
            return p(0, 'f_%s(%s, %s, %s, %d)' % (m, g(0), g(1), g(2), bits))
        if m == 'xchg':
            return '{ uint32_t t_ = %s; %s %s }' % (g(0), p(0, g(1)), p(1, 't_'))
        if m == 'bswap':
            return p(0, 'bswap32(%s)' % g(0))
        if m == 'push':
            v = g(0) if ops[0].type != X.X86_OP_IMM else '0x%xu' % (ops[0].imm & 0xffffffff)
            if ops[0].size == 2 and ops[0].type != X.X86_OP_IMM:
                return 'push16(%s);' % v
            return 'push32(%s);' % v
        if m == 'pop':
            if ops[0].size == 2:
                return p(0, 'pop16()')
            # pop into memory addressed via esp uses esp after the pop
            return '{ uint32_t t_ = pop32(); %s }' % p(0, 't_')
        if m == 'pushal':
            return 'pushal();'
        if m == 'popal':
            return 'popal();'
        if m == 'pushfd':
            return 'push32(get_eflags());'
        if m == 'popfd':
            return 'set_eflags(pop32());'
        if m == 'leave':
            return 'cpu.r[ESP] = cpu.r[EBP]; cpu.r[EBP] = pop32();'
        if m == 'cdq':
            return 'cpu.r[EDX] = (cpu.r[EAX] & 0x80000000u) ? 0xffffffffu : 0;'
        if m == 'cwde':
            return 'cpu.r[EAX] = (uint32_t)(int32_t)(int16_t)cpu.r[EAX];'
        if m == 'cbw':
            return 'set16(EAX, (uint32_t)(int16_t)(int8_t)cpu.r[EAX]);'
        if m == 'cwd':
            return 'set16(EDX, (cpu.r[EAX] & 0x8000u) ? 0xffffu : 0);'
        if m == 'clc':
            return 'cpu.cf = 0;'
        if m == 'stc':
            return 'cpu.cf = 1;'
        if m == 'cmc':
            return 'cpu.cf ^= 1;'
        if m == 'cld':
            return 'cpu.df = 0;'
        if m == 'std':
            return 'cpu.df = 1;'
        if m == 'lahf':
            return 'set8h(EAX, get_eflags() & 0xffu);'
        if m == 'sahf':
            return 'set_eflags((get_eflags() & ~0xffu) | ((cpu.r[EAX] >> 8) & 0xffu));'
        if m.startswith('set'):
            return p(0, '(%s) ? 1u : 0u' % self.COND['j' + m[3:]])
        if m in ('mul', 'imul', 'div', 'idiv'):
            if len(ops) == 1:
                return '%s%d(%s);' % (m, bits, g(0))
            if len(ops) == 2:
                return p(0, 'f_imul2(%s, %s, %d)' % (g(0), g(1), bits))
            return p(0, 'f_imul2(%s, %s, %d)' % (g(1), g(2), bits))
        if m == 'bt':
            if ops[0].type == X.X86_OP_MEM and ops[1].type == X.X86_OP_REG:
                return 'cpu.cf = bt_mem(%s, %s);' % (self.mem_addr(insn, ops[0]), g(1))
            return 'cpu.cf = ((%s) >> ((%s) & %d)) & 1;' % (g(0), g(1), bits - 1)
        if m in ('bts', 'btr', 'btc'):
            if ops[0].type != X.X86_OP_REG:
                raise Unsupported(m + ' mem')
            v, b = g(0), '((%s) & %d)' % (g(1), bits - 1)
            op = {'bts': '| (1u << b_)', 'btr': '& ~(1u << b_)', 'btc': '^ (1u << b_)'}[m]
            return '{ uint32_t b_ = %s; cpu.cf = ((%s) >> b_) & 1; %s }' % (b, v, p(0, '(%s) %s' % (v, op)))

        # string instructions
        if m in ('movsb', 'movsw', 'movsd', 'stosb', 'stosw', 'stosd', 'lodsb', 'lodsw', 'lodsd',
                 'scasb', 'scasw', 'scasd', 'cmpsb', 'cmpsw', 'cmpsd'):
            kind, sz = m[:4], {'b': 1, 'w': 2, 'd': 4}[m[4]]
            mode = 'REP_NONE'
            if rep_kw in ('rep', 'repe', 'repz'):
                mode = 'REP_E' if kind in ('scas', 'cmps') else 'REP_PLAIN'
            elif rep_kw in ('repne', 'repnz'):
                mode = 'REP_NE'
            elif rep_kw:
                raise Unsupported('prefix ' + rep_kw)
            if mode == 'REP_NONE':
                return 'str_%s(%d, %s);' % (kind, sz, mode)
            # each repeated iteration costs about one instruction's time
            # (counted after the fact: `repne scasb` with ecx = -1 is strlen)
            return '{ uint32_t c_ = cpu.r[ECX]; str_%s(%d, %s); X86_COUNT(c_ - cpu.r[ECX]); }' \
                % (kind, sz, mode)

        # control flow
        if m == 'jmp':
            if ops[0].type == X.X86_OP_IMM:
                return self.goto(ops[0].imm, insn.address)
            return 'pc = %s; goto dispatch;' % g(0)
        if m in self.COND:
            return 'if (%s) { %s }' % (self.COND[m], self.goto(ops[0].imm, insn.address))
        if m == 'jecxz':
            return 'if (cpu.r[ECX] == 0) { %s }' % self.goto(ops[0].imm, insn.address)
        if m == 'jcxz':
            return 'if ((cpu.r[ECX] & 0xffff) == 0) { %s }' % self.goto(ops[0].imm, insn.address)
        if m in ('loop', 'loope', 'loopne'):
            cond = {'loop': '', 'loope': ' && cpu.zf', 'loopne': ' && !cpu.zf'}[m]
            return 'if (--cpu.r[ECX] != 0%s) { %s }' % (cond, self.goto(ops[0].imm, insn.address))
        if m == 'call':
            ret = 'push32(0x%xu); ' % nxt
            if ops[0].type == X.X86_OP_IMM:
                t = ops[0].imm
                if in_code(t) and t not in HLE_OVERRIDES:
                    return ret + 'goto L_%x;' % t
                return ret + 'hle_call(0x%xu);' % t
            return ret + 'pc = %s; goto dispatch;' % g(0)
        if m == 'ret':
            n = ops[0].imm if ops else 0
            return 'pc = pop32();%s goto dispatch;' % (' cpu.r[ESP] += %d;' % n if n else '')
        if m == 'int3':
            return 'x86_trap(0x%xu, "int3");' % insn.address
        raise Unsupported(m)

    def emit(self, out_path):
        addrs = sorted(self.insns)
        lines = []
        w = lines.append
        w('/* Generated by tools/recomp.py from nanpa2.exe. Do not edit or commit. */')
        w('#include "x86/x86.h"')
        w('#include "hle/hle.h"')
        w('')
        w('void x86_exec(uint32_t pc)')
        w('{')
        w('dispatch:')
        w('\tif (pc == X86_RET_SENTINEL) return;')
        w('\tswitch (pc) {')
        for a in addrs:
            if a in GAME_OVERRIDES:
                # the lifted original, for the differential tests only
                w('\tcase 0x%x: goto L_%x; /* oracle */' % (ORACLE_TAG | a, a))
            elif a in self.labels:
                w('\tcase 0x%x: goto L_%x;' % (a, a))
        w('\tdefault:')
        w('\t\tif (hle_is_target(pc)) {')
        w('\t\t\tuint32_t ra_ = rd32(cpu.r[ESP]);')
        w('\t\t\thle_call(pc);')
        w('\t\t\tpc = ra_;')
        w('\t\t\tgoto dispatch;')
        w('\t\t}')
        w('\t\tx86_trap(pc, "dispatch to unknown address");')
        w('\t\treturn;')
        w('\t}')
        # instructions from each block start to the next block start, for
        # the emulated-CPU-speed throttle (X86_COUNT, src/x86/x86.h)
        run_len = {}
        start = None
        prev_end = None
        for a in addrs:
            if a != prev_end or a in self.labels:
                start = a
                run_len[a] = 0
            run_len[start] += 1
            prev_end = a + self.insns[a].size
        prev_end = None
        for a in addrs:
            insn = self.insns[a]
            if a != prev_end or a in self.labels:
                if prev_end is not None and a != prev_end:
                    w('\tx86_trap(0x%xu, "fell off block");' % prev_end)
                w('L_%x:' % a)
                w('\tX86_COUNT(%du);' % run_len[a])
            try:
                body = self.lift(insn)
            except Unsupported as e:
                self.unsupported.setdefault(str(e), []).append(a)
                body = 'x86_trap(0x%xu, "unsupported: %s");' % (a, insn.mnemonic)
            if a in PROBES:
                body = 'x86_probe(0x%xu); ' % a + body
            w('\t%s /* %x: %s %s */' % (body, a, insn.mnemonic, insn.op_str))
            prev_end = a + insn.size
        w('}')
        w('')
        # which call targets need hand-written code
        w('const uint32_t x86_hle_targets[] = {')
        for t in sorted(self.hle_targets | set(HLE_OVERRIDES)):
            w('\t0x%xu,' % t)
        w('\t0')
        w('};')
        open(out_path, 'w').write('\n'.join(lines) + '\n')


def main():
    pe = PE(sys.argv[1])
    lf = Lifter(pe)
    lf.explore()
    lf.emit(sys.argv[2])
    sys.stderr.write('lifted %d instructions, %d labels, %d HLE call targets\n'
                     % (len(lf.insns), len(lf.labels), len(lf.hle_targets)))
    if '--list-hle' in sys.argv:
        for t in sorted(lf.hle_targets):
            sys.stderr.write('  hle %x\n' % t)
    if lf.unsupported:
        for k, v in lf.unsupported.items():
            sys.stderr.write('unsupported %s at %s\n' % (k, ' '.join('%x' % x for x in v[:8])))
        sys.exit(1)


if __name__ == '__main__':
    main()
