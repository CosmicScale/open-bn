#!/usr/bin/env python3
"""Build the tests in tests/ into build/test/ (the first half of `make test`).

Usage: python3 tools/test.py [-jN]   (in the open-bn-env container, from the repository root,
       after tools/build.py: the tests link its objects)

Each test is a program for the console: its sources (TESTS), the helpers of tests/support/ and
Unity (build/dl/unity/, tools/fetch.py) are compiled like the units of bn, and linked with every
unit of bn. The program starts in the test's __wrap_main: ld --wrap main points crt1 there, so
bn's own main stays in the link without running. A test that links tests/support/fakepfs.cpp
replaces the pfsd client (its scepfs* names are weak) with a disk in memory.
tools/test_ps2.py runs the programs on the console.
"""
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from build import (CRT_HEAD, CRT_TAIL, GCCLIB, LD, LIBS, LIBS_L, LD_QUIET, STRIP, compile_unit, fail,  # noqa: E402
                   libs, read_units, unit_dir)
from fetch import UNITY  # noqa: E402

OUT = 'build/test'
# test program -> its own sources; the order is the order the runner runs them in
TESTS = {
    'import_test': ['tests/import_test.cpp', 'tests/support/fakepfs.cpp'],
    'import_disk_test': ['tests/import_disk_test.cpp'],
    'gettext_test': ['tests/gettext_test.cpp'],
    'amx_loader_test': ['tests/amx_loader_test.cpp'],
    'dnas_ar_test': ['tests/dnas_ar_test.cpp', 'tests/support/fakepfs.cpp'],
    'input_test': ['tests/input_test.cpp'],
    'sysconf_test': ['tests/sysconf_test.cpp', 'tests/support/fakepfs.cpp'],
    'xml_entity_test': ['tests/xml_entity_test.cpp'],
}
COMMON = ['tests/support/testutil.cpp', f'{UNITY}/unity.c']
# -DSN_TARGET_PS2: the integer types of ref/small-fmt6/amx.h, as bn's units that include it
CFLAGS = f'-Itests -I{UNITY} -DSN_TARGET_PS2'


def main():
    args = sys.argv[1:]
    jobs = next((int(a[2:]) for a in args if a.startswith('-j') and a[2:]), os.cpu_count() or 1)
    if not os.path.exists(f'{UNITY}/unity.c'):
        fail(f'{UNITY}/unity.c missing: run python3 tools/fetch.py')
    units = read_units()
    missing = [u[0] for u in units if not os.path.exists(unit_dir(u[0]) + '/out.o')]
    if missing:
        fail(f'{len(missing)} units of bn not compiled: run make build first')
    srcs = sorted(set(COMMON + [s for v in TESTS.values() for s in v]))
    with ThreadPoolExecutor(jobs) as ex:
        res = list(ex.map(lambda s: compile_unit(s, CFLAGS, ''), srcs))
    errs = [l for _, e in res for l in e]
    if errs:
        print('\n'.join(errs), file=sys.stderr)
        sys.exit(1)
    libs()
    os.makedirs(OUT, exist_ok=True)
    bn = [unit_dir(u[0]) + '/out.o' for u in units]
    for name, own in TESTS.items():
        objs = CRT_HEAD + bn + [unit_dir(s) + '/out.o' for s in own + COMMON] + CRT_TAIL
        unstripped = f'{OUT}/{name}.unstripped'
        r = subprocess.run(f'{LD} -m elf32lsmip -dynamic-linker /lib/ld.so.1 --wrap main -o {unstripped} '
                           f'{" ".join(objs)} -L{LIBS} -rpath-link {LIBS} {LIBS_L} {GCCLIB}/libgcc.a',
                           shell=True, capture_output=True, text=True)
        msgs = [l for l in r.stderr.splitlines() if not any(q in l for q in LD_QUIET)]
        if r.returncode:
            fail(f'link of {name} failed\n' + '\n'.join(msgs))
        for l in msgs:
            print(f'warning: ld: {l}', file=sys.stderr)
        r = subprocess.run([STRIP, '-o', f'{OUT}/{name}', unstripped], capture_output=True, text=True)
        if r.returncode:
            fail(f'strip: {r.stderr.strip()}')
    print(f'tests      {len(TESTS)} programs in {OUT}/: {", ".join(TESTS)}')


if __name__ == '__main__':
    main()
