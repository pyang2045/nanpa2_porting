#!/usr/bin/env python3
"""Run the exploration bot (NANPA2_EXPLORE, src/hle/explore.c) headless for several seeds and
report MES coverage and problems.

usage: explore.py --bin BIN --game GAME --out DIR [--seeds 1-6] [--frames 300000]
                  [--turbo 8] [--jobs 3] [--timeout 1800] [--saves DIR]
       explore.py --out DIR --report-only

Each seed runs in DIR/SEED with its own NANPA2_SAVE_DIR (a copy of --saves if given, else
empty, which starts the game from the Prologue). Its log goes to DIR/SEED/log.txt.
The report lists every *.MES under GAME/DATA that no run reached, and per seed:
  - traps ("x86 trap");
  - watchdog warnings ("WARNING:");
  - stalls ("explore: stall");
  - exit status.
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import time


def seeds_arg(s):
    out = []
    for part in s.split(','):
        if '-' in part:
            a, b = part.split('-')
            out += range(int(a), int(b) + 1)
        else:
            out.append(int(part))
    return out


def run(args):
    procs = {}
    todo = list(args.seeds)
    while todo or procs:
        while todo and len(procs) < args.jobs:
            seed = todo.pop(0)
            d = os.path.join(args.out, str(seed))
            shutil.rmtree(d, ignore_errors=True)
            os.makedirs(d)
            saves = os.path.join(d, 'saves')
            if args.saves:
                shutil.copytree(args.saves, saves)
            else:
                os.makedirs(saves)
            env = dict(os.environ, NANPA2_HEADLESS='1', NANPA2_SAVE_DIR=saves,
                       NANPA2_TURBO=str(args.turbo), NANPA2_EXPLORE=str(seed),
                       NANPA2_EXPLORE_FRAMES=str(args.frames))
            log = open(os.path.join(d, 'log.txt'), 'w')
            p = subprocess.Popen([args.bin, '--game', args.game], env=env, stdout=log,
                                 stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
            procs[seed] = (p, time.time(), log)
        time.sleep(2)
        for seed, (p, t0, log) in list(procs.items()):
            if p.poll() is None and time.time() - t0 > args.timeout:
                p.kill()
                p.wait()
                log.write('explore.py: killed after %d s\n' % args.timeout)
            if p.poll() is not None:
                log.write('explore.py: exit %d\n' % p.returncode)
                log.close()
                del procs[seed]


def report(args):
    reached = {}
    for log in sorted(glob.glob(os.path.join(args.out, '*', 'log.txt'))):
        seed = os.path.basename(os.path.dirname(log))
        text = open(log, errors='replace').read()
        names = re.findall(r'^explore: reached (\S+) frame (\d+)', text, re.M)
        for n, f in names:
            reached.setdefault(n, (seed, int(f)))
        problems = re.findall(r'^(x86 trap.*|.*WARNING:.*|explore: stall.*|explore\.py: .*)$', text, re.M)
        last = names[-1][0] if names else '-'
        print('seed %s: %d MES, last %s' % (seed, len(names), last))
        for p in problems:
            print('  ' + p.strip())
    if args.game:
        allmes = sorted(os.path.basename(p).lower() for p in glob.glob(os.path.join(args.game, 'DATA', '*'))
                        if p.upper().endswith('.MES'))
        got = [m for m in allmes if m in reached]
        print('coverage: %d of %d MES files' % (len(got), len(allmes)))
        print('reached:', ' '.join(got))
        print('not reached:', ' '.join(m for m in allmes if m not in reached))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin')
    ap.add_argument('--game')
    ap.add_argument('--out', required=True)
    ap.add_argument('--seeds', type=seeds_arg, default=seeds_arg('1-3'))
    ap.add_argument('--frames', type=int, default=300000)
    ap.add_argument('--turbo', type=int, default=8)
    ap.add_argument('--jobs', type=int, default=3)
    ap.add_argument('--timeout', type=int, default=1800)
    ap.add_argument('--saves')
    ap.add_argument('--report-only', action='store_true')
    args = ap.parse_args()
    if not args.report_only:
        if not args.bin or not args.game:
            sys.exit('--bin and --game are needed to run')
        run(args)
    report(args)


if __name__ == '__main__':
    main()
