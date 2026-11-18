#!/usr/bin/env python3
"""Which units does a change touch? Compares the objects of the tree with those of a commit.

Usage: python3 tools/compare.py REF   (in the container, after `make compare` extracted REF into
       build/compare/<REF>/ with git archive)

Compiles the current tree (tools/build.py --compile-only, incremental) and the tree of REF
(objects in build/compare/<REF>/build/obj/), then compares the two objects of every unit section by
section, debug information aside. Prints the units whose object changed, with the sections that
differ, and the units added or removed. A unit you did not mean to touch in the list is a
regression to look at: a header, a define or a flag changed more than you thought.

Both trees are compiled by the tools/build.py of this checkout, with its toolchain and the assets
of assets/: a change to build.py itself (base flags, includes) does not show up, and neither does
anything that only the link sees (libraries, crt files, order of the units).
Exit 1 if building fails, 0 otherwise.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from build import UNITS, unit_dir  # noqa: E402

# debug information: it records the directory of the build
IGNORE = {'.mdebug', '.rel.mdebug'}


def units(path):
    out = []
    for line in open(path):
        line = line.strip()
        if line and not line.startswith('#'):
            out.append(line.split('|')[0].strip())
    return out


def sections(path):
    from elftools.elf.elffile import ELFFile
    e = ELFFile(open(path, 'rb'))
    return {s.name: s.data() for s in e.iter_sections() if s.header.sh_type != 'SHT_NOBITS'}


def main():
    if len(sys.argv) != 2:
        sys.exit('usage: python3 tools/compare.py REF')
    ref = sys.argv[1]
    tree = 'build/compare/' + re.sub(r'[^A-Za-z0-9_.-]', '_', ref)
    if not os.path.isdir(tree):
        sys.exit(f'error: {tree} missing: run make compare REF={ref}')
    build = [sys.executable, f'{ROOT}/tools/build.py', '--compile-only']
    if subprocess.run(build).returncode:
        sys.exit('error: the current tree does not build')
    if not os.path.lexists(f'{tree}/assets'):
        os.symlink(os.path.relpath('assets', tree), f'{tree}/assets')
    print(f'{ref}:')
    if subprocess.run(build + ['--no-assets'], cwd=tree).returncode:
        sys.exit(f'error: {ref} does not build')
    cur, old = units(UNITS), units(f'{tree}/{UNITS}')
    changed = []
    for u in cur:
        if u not in old:
            continue
        a, b = f'{unit_dir(u)}/out.o', f'{tree}/{unit_dir(u)}/out.o'
        if open(a, 'rb').read() != open(b, 'rb').read():
            sa, sb = sections(a), sections(b)
            diff = sorted(n for n in set(sa) | set(sb) if sa.get(n) != sb.get(n) and n not in IGNORE)
            if diff:
                changed.append((u, diff))
    added = [u for u in cur if u not in old]
    removed = [u for u in old if u not in cur]
    print(f'compare    {len(cur)} units against {ref}: {len(changed)} changed, {len(added)} added, '
          f'{len(removed)} removed')
    for u, diff in changed:
        print(f'  changed  {u}  ({", ".join(diff)})')
    for u in added:
        print(f'  added    {u}')
    for u in removed:
        print(f'  removed  {u}')


if __name__ == '__main__':
    main()
