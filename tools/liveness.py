#!/usr/bin/env python3
"""Register liveness over the lifted game code (docs/reimpl.md, "Register contracts").

usage: liveness.py EXE [--why] ADDR...    live registers on entry to each ADDR
       liveness.py EXE [--why] --after FN...
                                          live registers at the return sites of
                                          the calls to FN (each, and the union)

A register is live at a point if some path from there may read it before
writing it. Registers are tracked in three lanes, the low byte (AL), the
high byte (AH) and the upper half, so `mov al, x` or `mov bx, y` kill only
what they write. The flags are two registers, CF and the rest (ZF SF OF
PF), since inc/dec write all but CF. --why prints,
for each live lane, one path (within the function) to an instruction that
reads it, or to the function's return.

Method: interprocedural, with function summaries, over every instruction
tools/recomp.py lifts. A function is a lifted call target; its body is what
recomp.Lifter.body reaches (jumps and jump tables, not calls). Each function
gets a summary GEN (read before written on some path to its return) and
KILL (written on every path), solved bottom-up to a fixpoint; at a call,
live = GEN | (live at the return site & ~KILL). The liveness at each
function's return (RET) is the union over its call sites, solved top-down.
Both are least fixpoints of monotone equations, so the answers are sound
for "dead" (a register reported dead is written before any read on every
path), up to these modelling rules:

- `push r` in a function's straight-line prologue with a matching `pop r`
  before every `ret` (a callee-saved register) goes through a slot: the push
  reads r only if r is live at the return;
- `mov esp, [0x445cdc]` (0x40638d, the VM's exit, reached on WM_QUIT and
  script end) unwinds to the frame of the outermost VM 0x4060d4: its `ret`
  continues at WinMain's return site, whatever function reached it;
- `xor r, r` / `sub r, r` kill r
  without reading it;
- a call to code that is not lifted reads ESP and what HLE_READS lists.
  Imports, COM methods and CRT functions kill EAX (their result). Overrides
  of game functions (port code) kill only what HLE_WRITES lists, such as the
  message pump 0x401bf0 (src/hle/hle.c: EAX, ECX, EDX).
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import recomp  # noqa: E402
from capstone import x86 as X  # noqa: E402

REGS = ['EAX', 'ECX', 'EDX', 'EBX', 'ESP', 'EBP', 'ESI', 'EDI']
LO, HI, UP = 1, 2, 4  # lanes of a register: bits 0-7, 8-15, 16-31
CF = 1 << 24      # carry
ZS = 1 << 25      # ZF SF OF PF
FLAGS = CF | ZS
ALL = (1 << 26) - 1
SLOT0 = 26        # callee-saved slots: bits 26-39; push/pop pairs from 40


def lanes(reg, which=LO | HI | UP):
    return which << (3 * REGS.index(reg))


def R(reg):
    return lanes(reg)


NAMES = {}
for _r in REGS:
    _l = _r[1:].lower()  # 'ax'
    NAMES[_r.lower()] = lanes(_r)
    NAMES[_l] = lanes(_r, LO | HI)
    if _r in ('EAX', 'ECX', 'EDX', 'EBX'):
        NAMES[_l[0] + 'l'] = lanes(_r, LO)
        NAMES[_l[0] + 'h'] = lanes(_r, HI)

# Calls into code that is not lifted: what they read besides ESP and what
# they kill. Game units (src/game/units.c) read their inputs and write only
# their outputs (their scratch registers are not written by the C code).
HLE_READS = {
    0x4037fc: R('ESI'),                                # script_eval
    0x4039f7: R('ESI'),                                # constant reader
    0x406455: lanes('EAX', LO | HI) | R('EBX'),        # flag write: AX, EBX
}
HLE_WRITES = {
    0x4037fc: R('EAX') | R('ESI'),
    0x4039f7: R('ESI') | lanes('EAX', LO),
    0x406455: lanes('EAX', LO | HI),
    0x401bf0: R('EAX') | R('ECX') | R('EDX'),          # message pump
}

VM_ENTRY = 0x4060d4  # its frame is what 0x40638d unwinds to
UNWIND = 0x40638d    # mov esp, [0x445cdc]

# flag writers that set all of CF ZF SF OF PF
FLAG_KILL = {'cmp', 'test', 'add', 'sub', 'and', 'or', 'xor', 'neg', 'adc', 'sbb', 'sahf',
             'popfd', 'cmpsb', 'cmpsw', 'cmpsd', 'scasb', 'scasw', 'scasd'}
FLAG_READ = {'adc', 'sbb', 'pushfd', 'lahf', 'cmc', 'rcl', 'rcr'}


def reg_sets(insn):
    """(read mask, kill mask) of one instruction, calls and jumps aside."""
    m = insn.mnemonic.split(' ')[-1]
    rd, wr = insn.regs_access()
    read = kill = 0
    for r in rd:
        read |= NAMES.get(insn.reg_name(r), 0)
    for r in wr:
        kill |= NAMES.get(insn.reg_name(r), 0)
    ops = insn.operands
    if m in ('xor', 'sub') and len(ops) == 2 and ops[0].type == X.X86_OP_REG \
            and ops[1].type == X.X86_OP_REG and ops[0].reg == ops[1].reg:
        read &= ~NAMES.get(insn.reg_name(ops[0].reg), 0)
    cond = None
    if m.startswith('j') and m not in ('jmp', 'jecxz', 'jcxz'):
        cond = m[1:]
    elif m.startswith(('set', 'cmov')):
        cond = m[3:] if m.startswith('set') else m[4:]
    if cond is not None:
        read |= (CF if cond in ('b', 'ae', 'nae', 'nb', 'c', 'nc', 'be', 'a', 'na', 'nbe')
                 else 0) | (0 if cond in ('b', 'ae', 'nae', 'nb', 'c', 'nc') else ZS)
    if m in FLAG_READ:
        read |= CF if m in ('adc', 'sbb', 'cmc', 'rcl', 'rcr') else FLAGS
    if insn.mnemonic.startswith(('repe', 'repne')):
        read |= ZS
    if m.startswith('loop') or m == 'jecxz':
        read |= R('ECX')
    if m == 'jcxz':
        read |= NAMES['cx']
    if m in FLAG_KILL:
        kill |= FLAGS
    if m in ('inc', 'dec'):
        kill |= ZS
    return read, kill


class Liveness:
    def __init__(self, exe):
        self.pe = recomp.PE(exe)
        self.lf = recomp.Lifter(self.pe)
        self.lf.explore()
        self.insns = self.lf.insns
        self.build()

    def table_targets(self, disp):
        if disp in self.lf.tables:
            return [self.pe.u32(disp + 4 * i) for i in range(self.lf.tables[disp])]
        for a, b in recomp.DATA_TABLE_RANGES:
            if a <= disp < b:
                # the .data ranges hold several tables: this one runs from
                # disp to the next table base used by an indirect branch
                if not hasattr(self, '_data_bases'):
                    self._data_bases = set()
                    for i in self.insns.values():
                        if i.mnemonic in ('call', 'jmp') and i.operands[0].type == X.X86_OP_MEM:
                            self._data_bases.add(i.operands[0].mem.disp)
                out, q = [], disp
                while q < b and (q == disp or q not in self._data_bases):
                    v = self.pe.u32(q)
                    if v not in self.insns:
                        break
                    out.append(v)
                    q += 4
                return out
        return None

    def build(self):
        """Per instruction: read/kill masks and its kind: ('next', [succ]),
        ('call', [lifted callees], return site), ('ret',), ('stop',)."""
        ins = self.insns
        self.kind, self.read, self.kill = {}, {}, {}
        self.sites = {}  # lifted callee -> [(call address, return site)]
        for a, insn in ins.items():
            m = insn.mnemonic.split(' ')[-1]
            ops = insn.operands
            nxt = a + insn.size
            r, k = reg_sets(insn)
            if m == 'call':
                r |= R('ESP')
                targets = None
                if ops[0].type == X.X86_OP_IMM:
                    targets = [ops[0].imm]
                elif ops[0].type == X.X86_OP_MEM:
                    targets = self.table_targets(ops[0].mem.disp)
                lifted = [t for t in (targets or []) if t in ins and t not in recomp.HLE_OVERRIDES]
                if lifted:
                    kind = ('call', lifted, nxt)
                    for t in lifted:
                        self.sites.setdefault(t, []).append((a, nxt))
                    k = 0
                else:
                    t = targets[0] if targets else None
                    r |= HLE_READS.get(t, 0)
                    k = HLE_WRITES.get(t, 0) if t in recomp.HLE_OVERRIDES else R('EAX')
                    kind = ('next', [nxt])
            elif m == 'jmp':
                if ops[0].type == X.X86_OP_IMM:
                    kind = ('next', [ops[0].imm])
                else:
                    tt = self.table_targets(ops[0].mem.disp) if ops[0].type == X.X86_OP_MEM else None
                    if tt is None:  # import thunk (jmp [IAT]): a tail call
                        kind, r, k = ('ret',), r | R('ESP'), R('EAX')
                    else:
                        kind = ('next', tt)
            elif m in ('ret', 'retf'):
                kind = ('ret',)
                r |= R('ESP')
            elif m.startswith('j') or m.startswith('loop'):
                kind = ('next', [ops[0].imm, nxt])
            elif m in ('int3', 'hlt'):
                kind = ('stop',)
            else:
                kind = ('next', [nxt])
            self.kind[a], self.read[a], self.kill[a] = kind, r, k
        # the ret after the unwind belongs to the outermost VM's frame
        self.unwound = set()
        a = UNWIND
        while a in ins and self.kind[a][0] == 'next':
            self.unwound.add(a)
            a = self.kind[a][1][0]
        self.unwound.add(a)
        self.unwind_ret = a
        self.funcs = sorted(set(self.sites) | set(recomp.EXTRA_ENTRIES) | {0x402090})
        self.bodies = {e: self.lf.body(e) for e in self.funcs}
        self.saves = {}
        for e in self.funcs:
            self.saves[e] = self.callee_saves(e)
        self.block_saves()
        self.specialize()
        self.reachable()

    def reachable(self):
        """Instructions the port can run: from the roots, without entering
        overridden functions (the lifted oracle copies of game units are
        reached only by the differential tests)."""
        work = list(recomp.EXTRA_ENTRIES) + [0x402090]
        # LIVENESS_ROOTS=hex,hex: extra roots, for code whose only caller is
        # now C (the map sub-ops, called through 0x403a70 by game_op_map)
        work += [int(x, 16) for x in os.environ.get('LIVENESS_ROOTS', '').split(',') if x]
        seen = set()
        while work:
            a = work.pop()
            if a in seen or a not in self.insns or a in recomp.HLE_OVERRIDES:
                continue
            seen.add(a)
            k = self.kind[a]
            if k[0] == 'next':
                work += k[1]
            elif k[0] == 'call':
                work += [t if isinstance(t, int) else t[0] for t in k[1]] + [k[2]]
        self.reach = seen

    def specialize(self):
        """A call made with EAX = n (`mov eax, n` just before) to a function
        whose straight-line entry dispatches on EAX through a jump table
        (0x406dff calling 0x403510 sub-op 7) uses a summary of that one
        case: key (function, n)."""
        ins = self.insns
        targets = self.jump_targets()
        by_end = getattr(self, '_by_end', None) or {i.address + i.size: i for i in ins.values()}
        self._by_end = by_end
        self.override = {}
        for c, k in list(self.kind.items()):
            if k[0] != 'call' or len(k[1]) != 1:
                continue
            f = k[1][0]
            # n: mov eax, imm before the call, EAX not written after it
            n, p = None, by_end.get(c)
            for _ in range(4):
                if p is None or p.address in targets or self.kind[p.address][0] != 'next':
                    break
                w = {p.reg_name(r) for r in p.regs_access()[1]}
                if p.mnemonic == 'mov' and p.operands[0].type == X.X86_OP_REG \
                        and p.reg_name(p.operands[0].reg) == 'eax' and p.operands[1].type == X.X86_OP_IMM:
                    n = p.operands[1].imm
                    break
                if w & {'eax', 'ax', 'al', 'ah'}:
                    break
                p = by_end.get(p.address)
            if n is None:
                continue
            a = f
            for _ in range(8):
                i = ins.get(a)
                if i is None:
                    break
                w = {i.reg_name(r) for r in i.regs_access()[1]}
                if i.mnemonic == 'jmp' and i.operands[0].type == X.X86_OP_MEM \
                        and i.operands[0].mem.disp in self.lf.tables \
                        and i.reg_name(i.operands[0].mem.index) == 'eax':
                    t = i.operands[0].mem.disp
                    if n < self.lf.tables[t]:
                        key = (f, n)
                        self.override[key] = {a: ('next', [self.pe.u32(t + 4 * n)])}
                        self.kind[c] = ('call', [key], k[2])
                        self.sites.setdefault(key, []).append((c, k[2]))
                        self.sites[f].remove((c, k[2]))
                        if key not in self.bodies:
                            self.funcs.append(key)
                            self.bodies[key] = self.bodies[f]
                            self.saves[key] = self.saves[f]
                    break
                if self.kind[a] != ('next', [a + i.size]) or w & {'eax', 'ax', 'al', 'ah'}:
                    break
                a += i.size

    def intra_succ(self, a):
        k = self.kind[a]
        return k[1] if k[0] == 'next' else [k[2]] if k[0] == 'call' else []

    def block_saves(self):
        """`push r` ... `pop r` (and `pushal` ... `popal`) in a single-entry
        region: the code reached from the push through instructions whose
        predecessors all lie in the region (calls stepped over), with the
        stack depth the same on every path, up to the pop at depth 0. r is
        saved around the region through a slot, as for callee saves. Paths
        that leave the region carry r on unchanged, so they need no special
        case; the pop is reached only through the push."""
        ins = self.insns
        preds = {}
        for a in ins:
            for t in self.intra_succ(a):
                preds.setdefault(t, set()).add(a)
        self.pair_push, self.pair_pop, self.pair_end = {}, {}, {}
        bit = 1 << 40

        def delta(j):
            m, ops = j.mnemonic, j.operands
            if m in ('push', 'pushal'):
                return 1
            if m in ('pop', 'popal'):
                return -1
            if m == 'add' and ops[0].type == X.X86_OP_REG and j.reg_name(ops[0].reg) == 'esp' \
                    and ops[1].type == X.X86_OP_IMM:
                return -(ops[1].imm // 4)
            if m != 'call' and any(j.reg_name(r) in ('esp', 'sp') for r in j.regs_access()[1]):
                return None  # some other stack change
            return 0

        for a, i in ins.items():
            if i.mnemonic == 'pushal':
                regs, closer = [r.lower() for r in REGS if r != 'ESP'], 'popal'
            elif i.mnemonic == 'push' and i.operands[0].type == X.X86_OP_REG \
                    and i.operands[0].size == 4:
                regs, closer = [i.reg_name(i.operands[0].reg)], 'pop'
            else:
                continue
            depth = {a: 1}  # stack depth (in pushes) after each region node
            region, pops, work, bad = {a}, set(), [a], False
            while work and not bad and len(region) < 400:
                x = work.pop()
                for t in self.intra_succ(x):
                    if t in region or t == a:
                        continue
                    if not preds.get(t, set()) <= region:
                        continue  # entered from outside: not ours (an exit)
                    j = ins[t]
                    d0 = depth[x]
                    if j.mnemonic == closer and d0 == 1:
                        if closer == 'popal' or (j.operands[0].type == X.X86_OP_REG
                                                 and j.reg_name(j.operands[0].reg) == regs[0]):
                            pops.add(t)
                        region.add(t)
                        depth[t] = 0
                        continue
                    dd = delta(j)
                    if dd is None or d0 + dd < 1:
                        bad = True
                        break
                    region.add(t)
                    depth[t] = d0 + dd
                    work.append(t)
                # a node already in the region must agree on the depth
                for t in self.intra_succ(x):
                    if t in depth and t != a and t not in pops:
                        j = ins[t]
                        dd = delta(j)
                        if dd is not None and depth[x] + dd != depth[t]:
                            bad = True
            if bad or not pops or work:
                continue
            slots = []
            for n in regs:
                slots.append((n, bit))
                bit <<= 1
            self.pair_push[a] = slots
            for q in pops:
                self.pair_pop[q] = slots
            self.pair_end[a] = min(pops)  # for --why: continue after a pop
            self.pair_end[a] += ins[self.pair_end[a]].size

    def jump_targets(self):
        if not hasattr(self, '_targets'):
            t = set()
            for a, k in self.kind.items():
                if k[0] == 'next':
                    nxt = a + self.insns[a].size
                    t.update(x for x in k[1] if x != nxt)
            self._targets = t
        return self._targets

    def callee_saves(self, e):
        """{push address: (reg, slot bit)} and {pop address: ...} of the
        callee-saved registers of the function at e: pushed in the
        straight-line code from its entry (before being written), popped in
        the straight-line code before every ret."""
        ins = self.insns
        targets = self.jump_targets()
        saved = ('ebx', 'ebp', 'esi', 'edi')
        pushed, written, a = [], set(), e
        for _ in range(12):
            i = ins.get(a)
            if i is None or self.kind[a] != ('next', [a + i.size]) or (a != e and a in targets):
                break
            m, ops = i.mnemonic, i.operands
            w = {i.reg_name(r) for r in i.regs_access()[1]}
            if m == 'push' and ops[0].type == X.X86_OP_REG and ops[0].size == 4:
                n = i.reg_name(ops[0].reg)
                if n in saved and n not in written and n not in [x for _, x in pushed]:
                    pushed.append((a, n))
            elif 'esp' in w and not (m == 'sub' and ops[0].type == X.X86_OP_REG):
                break
            written |= w
            a += i.size
        rets = [a for a in self.bodies[e] if self.kind[a][0] == 'ret']
        if not pushed or not rets:
            return {}, {}
        by_end = getattr(self, '_by_end', None)
        if by_end is None:
            by_end = self._by_end = {i.address + i.size: i for i in ins.values()}
        epi = []
        for r in rets:
            pops = {}
            p = None if r in targets else by_end.get(r)
            while p is not None and self.kind[p.address] == ('next', [p.address + p.size]):
                m, ops = p.mnemonic, p.operands
                w = {p.reg_name(x) for x in p.regs_access()[1]}
                if m == 'pop' and ops[0].type == X.X86_OP_REG:
                    pops.setdefault(p.reg_name(ops[0].reg), p.address)
                elif 'esp' in w and not (m == 'add' and ops[0].type == X.X86_OP_REG):
                    break
                elif w & set(saved):
                    break  # writes a callee-saved register after its pop
                if p.address in targets:
                    break
                p = by_end.get(p.address)
            epi.append(pops)
        push_slot, pop_slot = {}, {}
        for k, (a, n) in enumerate(pushed):
            if all(n in pops for pops in epi):
                bit = 1 << (SLOT0 + k)
                push_slot[a] = (n, bit)
                for pops in epi:
                    pop_slot[pops[n]] = (n, bit)
        return push_slot, pop_slot

    # ---------------------------------------------------------------- solve
    def solve_body(self, e, ret_live, summ, ret_out):
        """Liveness over the body of e with ret_live at its returns."""
        body = self.bodies[e]
        push_slot, pop_slot = self.saves[e]
        over = self.override.get(e, {})
        kind = lambda a: over.get(a) or self.kind[a]  # noqa: E731
        entry = e if isinstance(e, int) else e[0]
        live = {a: 0 for a in body}
        preds = {a: [] for a in body}
        for a in body:
            k = kind(a)
            succ = k[1] if k[0] == 'next' else [k[2]] if k[0] == 'call' else []
            for t in succ:
                if t in preds:
                    preds[t].append(a)
        work = list(body)
        while work:
            a = work.pop()
            k = kind(a)
            if k[0] == 'next':
                out = 0
                for t in k[1]:
                    out |= live[t] if t in live else ALL
            elif k[0] == 'call':
                after = live.get(k[2], ALL)
                out = 0
                for t in k[1]:
                    g, kl = summ.get(t, (0, ALL))
                    out |= g | (after & ~kl)
            elif k[0] == 'ret':
                out = ret_out[VM_ENTRY] if a == self.unwind_ret else ret_live
            else:
                out = 0
            read = self.read[a]
            if a in push_slot:  # the saved value is read only through the slot
                read &= ~NAMES[push_slot[a][0]]
            for n, _ in self.pair_push.get(a, ()):
                read &= ~NAMES[n]
            if k[0] == 'call':
                v = read | out
            else:
                v = read | (out & ~self.kill[a])
            if a in pop_slot:
                n, bit = pop_slot[a]
                if out & NAMES[n]:
                    v |= bit
            if a in push_slot:
                n, bit = push_slot[a]
                v &= ~bit
                if out & bit:
                    v |= NAMES[n]
            for n, bit in self.pair_pop.get(a, ()):
                if out & NAMES[n]:
                    v |= bit
            for n, bit in self.pair_push.get(a, ()):
                v &= ~bit
                if out & bit:
                    v |= NAMES[n]
            if v != live[a]:
                live[a] = v
                work += preds[a]
        live['entry'] = live[entry]
        return live

    def solve(self):
        summ = {e: (0, ALL) for e in self.funcs}
        ret_out = {e: 0 for e in self.funcs}
        # roots (WinMain, window procedures) return to the CRT / Windows
        roots = [e for e in self.funcs if e not in self.sites]
        for e in roots:
            ret_out[e] = R('EAX') | R('ESP')
        changed = True
        while changed:
            changed = False
            for e in self.funcs:
                g = self.solve_body(e, 0, summ, ret_out)['entry'] & ALL
                full = self.solve_body(e, ALL, summ, ret_out)['entry'] & ALL
                s = (g, ALL & ~full)
                if s != summ[e]:
                    summ[e] = s
                    changed = True
            for e in self.funcs:
                live = self.solve_body(e, ret_out[e], summ, ret_out)
                for t, sites in self.sites.items():
                    for c, r in sites:
                        if r in live and c in self.reach:
                            v = ret_out[t] | (live[r] & ALL)
                            if v != ret_out[t]:
                                ret_out[t] = v
                                changed = True
        self.summ, self.ret_out = summ, ret_out
        self.live = {}
        self.owner = {}
        for e in self.funcs:
            live = self.solve_body(e, ret_out[e], summ, ret_out)
            del live['entry']
            for a, v in live.items():
                self.live[a] = self.live.get(a, 0) | (v & ALL)
                self.owner.setdefault(a, []).append(e)
        return self.live

    def why(self, start, bit):
        """A shortest path from start to an instruction reading bit, or to a
        return of a function holding start, through instructions not killing
        it (calls are stepped over using the callee summaries)."""
        prev = {start: None}
        q = [start]
        top_ret = 0
        for e in self.owner.get(start, []):
            top_ret |= self.ret_out[e]
        depth = {start: 0}
        for a in q:
            k = self.kind[a]
            reads = self.read[a] & bit
            if any(NAMES[n] & bit for n, _ in self.pair_push.get(a, ())):
                # saved here, restored at the pop: the value goes on past it
                reads = 0
                t = self.pair_end[a]
                if t not in prev:
                    prev[t] = a
                    depth[t] = depth[a]
                    q.append(t)
            ret_out = top_ret if depth[a] == 0 else 0
            if a == self.unwind_ret:
                ret_out = self.ret_out[VM_ENTRY]
            if reads or (k[0] == 'ret' and ret_out & bit):
                path = []
                while a is not None:
                    path.append(a)
                    a = prev[a]
                return path[::-1], 'read' if reads else 'returns'
            succ = []
            if k[0] == 'call':
                # into a callee that reads it, else over the call
                succ = [(t, depth[a] + 1) for t in k[1] if self.summ[t][0] & bit]
                if not all(self.summ[t][1] & bit for t in k[1]):
                    succ.append((k[2], depth[a]))
            elif k[0] == 'next' and not self.kill[a] & bit:
                succ = [(t, depth[a]) for t in k[1]]
            for t, d in succ:
                if t in self.insns and t not in prev:
                    prev[t] = a
                    depth[t] = d
                    q.append(t)
        return None, None


def names(mask):
    out = []
    for i, r in enumerate(REGS):
        m = (mask >> (3 * i)) & 7
        if m == 7:
            out.append(r)
        elif m:
            out.append('%s(%s)' % (r, '+'.join(n for b, n in ((LO, 'lo'), (HI, 'hi'), (UP, 'up'))
                                                if m & b)))
    if mask & FLAGS == FLAGS:
        out.append('FLAGS')
    elif mask & CF:
        out.append('CF')
    elif mask & ZS:
        out.append('ZF/SF/OF/PF')
    return ' '.join(out) or '-'


def explain(lv, a, mask):
    done = set()
    for b in range(26):
        bit = 1 << b
        if not mask & bit:
            continue
        p, what = lv.why(a, bit)
        if not p:
            continue
        key = (p[-1], what, b // 3)
        if key in done:
            continue
        done.add(key)
        i = lv.insns[p[-1]]
        print('      %s: %s at %x (%s %s), path %s' % (
            names(bit), what, p[-1], i.mnemonic, i.op_str,
            fmt_path(p if len(p) <= 10 else p[:4] + ['...'] + p[-5:])))


def fmt_path(p):
    return ' '.join(x if isinstance(x, str) else '%x' % x for x in p)


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    lv = Liveness(args[0])
    live = lv.solve()
    after = why = False
    for a in args[1:]:
        if a == '--after':
            after = True
            continue
        if a == '--why':
            why = True
            continue
        x = int(a, 16)
        if after:
            sites = list(lv.sites.get(x, []))
            for key, more in lv.sites.items():  # specialized calls
                if isinstance(key, tuple) and key[0] == x:
                    sites += more
            if x not in lv.sites:  # an override: its call sites are HLE calls
                sites = sorted((i.address, i.address + i.size) for i in lv.insns.values()
                               if i.mnemonic == 'call' and i.operands[0].type == X.X86_OP_IMM
                               and i.operands[0].imm == x)
            sites = [(c, r) for c, r in sites if c in lv.reach]  # not oracle code
            m = 0
            for c, r in sites:
                m |= live.get(r, ALL)
            print('%x after (%d sites): %s' % (x, len(sites), names(m)))
            for c, r in sites:
                print('   %x: %s' % (r, names(live.get(r, ALL))))
                if why:
                    explain(lv, r, live.get(r, 0))
        else:
            print('%x: %s' % (x, names(live.get(x, ALL))))
            if why:
                explain(lv, x, live.get(x, 0))


if __name__ == '__main__':
    main()
