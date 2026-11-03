#!/usr/bin/env python3
"""Disassemble nanpa2.exe game code (0x401000-0x407800) with objdump, treating
the jump tables embedded in .text as data so the code after them decodes.
usage: disasm.py EXE > game.asm"""
import re, struct, subprocess, sys, importlib.util, os

spec = importlib.util.spec_from_file_location('peek', os.path.join(os.path.dirname(__file__), 'peek.py'))
peek = importlib.util.module_from_spec(spec); spec.loader.exec_module(peek)

START, END = 0x401000, 0x407800
# jump-table bases inside .text (from 'jmp dword ptr [4*reg + base]')
TABLES = [0x40247c, 0x402508, 0x402550, 0x403521, 0x40368b, 0x40386d,
          0x40620b, 0x406262, 0x406922, 0x407216, 0x4072b7]

def objdump(exe, a, b):
    out = subprocess.run(['objdump', '-d', '--x86-asm-syntax=intel', '--no-show-raw-insn',
                          '--start-address=0x%x' % a, '--stop-address=0x%x' % b, exe],
                         capture_output=True, text=True).stdout
    lines = []
    for l in out.splitlines():
        if re.match(r'\s+[0-9a-f]+:', l):
            lines.append(re.sub(r' <\.text\+0x[0-9a-f]+>', '', l))
    return lines

def main():
    exe = sys.argv[1]
    d, S = peek.load(exe)
    spans = []
    for t in TABLES:
        n = 0
        while True:
            v = struct.unpack('<I', peek.read(d, S, t + 4 * n, 4))[0]
            if not (START <= v < END) or (n and any(t + 4 * n == u for u in TABLES)):
                break
            n += 1
        spans.append((t, t + 4 * n))
    spans.sort()
    pos = START
    for a, b in spans:
        print('\n'.join(objdump(exe, pos, a)))
        for k in range(a, b, 4):
            v = struct.unpack('<I', peek.read(d, S, k, 4))[0]
            print('  %x:      \t.table\t[%d] -> 0x%x' % (k, (k - a) // 4, v))
        pos = b
    print('\n'.join(objdump(exe, pos, END)))

main()
