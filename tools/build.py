#!/usr/bin/env python3
"""Build bn: from the sources to build/bn.elf.

Usage: python3 tools/build.py [-jN] [--compile-only] [--no-assets]
       (in the open-bn-env container, from the repository root: `make build`)

1. Assets: every row of config/assets.txt is cut from orig/bn-jpn.elf into assets/<file>, as the
   body of a C initializer that a source #includes. They are never committed.
2. Units: every unit of config/units.txt is compiled with Sony gcc 2.95.2 (-O2 plus the flags of
   its row) and assembled with gas 2.9EE into build/obj/<unit>/out.o. A unit is rebuilt only if its
   command changed or a file in gcc's dependency list is newer than its object. An implicit function
   declaration is an error in src/ (gcc 2.95 only warns: the call silently returns int).
3. Link: the shared libraries go into build/libs/ (from build/dl/, tools/fetch.py, and the kit);
   ld 2.9EE links the crt files and the units in the order of config/units.txt; the result is
   stripped into build/bn.elf.

--compile-only stops after step 2; --no-assets skips step 1 (tools/test.py, for the tree of another
commit, which uses the assets of this checkout). Sources, configuration and outputs are relative to
the current directory; the toolchain and the kit are those of this checkout.
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from fetch import DP, GCCLIB  # noqa: E402

ORIG = 'orig/bn-jpn.elf'
ORIG_SHA1 = '0bb9908a1d423edc77d116514003866898e5cf6b'
UNITS = 'config/units.txt'
ASSETS = 'config/assets.txt'
OBJ = 'build/obj'
LIBS = 'build/libs'
ELF = 'build/bn.elf'

TOOL = f'{ROOT}/docker/toolchain/sony'                  # Sony gcc 2.95.2 (make sony-gcc)
INC = f'{ROOT}/docker/toolchain/include'                # its headers (make headers)
BIN = f'{ROOT}/docker/toolchain/sonybinutils'           # Sony binutils 2.9EE (make sony-binutils)
AS, LD, STRIP = (f'{BIN}/mipsEEel-linux-{t}' for t in ('as', 'ld', 'strip'))
KIT = f'{ROOT}/docker/kit/rootfs'                       # the PS2 Linux Kit (make kit)

INC_C = f'-nostdinc -Isrc -I{INC}/fixinc -I{INC}/ginclude'
INC_CXX = (f'-nostdinc -Isrc -I{INC}/fixinc -I{INC}/libstdc++ -I{INC}/libstdc++/stl -I{INC}/libstdc++/std '
           f'-I{INC}/cpinc -I{INC}/libio -I{INC}/ginclude')
# __DATE__ and __TIME__ (only in the version strings of giflib) are fixed, so that a build does not
# depend on when it runs (cpp warns about redefining them)
FIXED_TIME = "-D__DATE__='\"Aug 28 2002\"' -D__TIME__='\"00:10:34\"'"

# The libraries bn runs with: PSBBN's (Definitive Patch) and the kit's glibc 2.2.2 ones, with sha1.
# libocm.so has no DT_SONAME: bn's DT_NEEDED "libocm.so" is the file name -locm finds.
LIB_FILES = {
    'libc.so.6': (f'{DP}/libc-2.2.2.so', '744284e54196c4a97948c7498689d0525ec4232b'),
    'ld.so.1': (f'{DP}/ld-2.2.2.so', 'e2827e1661db147f3024cf80213ab17adf5fe390'),
    'libstdc++-libc6.2-2.so.3': (f'{DP}/libstdc++-3-libc6.2-2-2.10.0.so', 'cb6dba42929cf2fadf8505961ae1b934e454bcb4'),
    'libocm.so': (f'{DP}/libocm.so.1.3', 'b594348548288815bfd054fb531b136ab55db067'),
    'libfreetype.so.6': (f'{DP}/libfreetype.so.6.1.0', '9be5fd94ac92f70a41393c7be831ceb13ec2f2e7'),
    'libm.so.6': (f'{KIT}/lib/libm.so.6', '34bbc64f50c36b55d3c115999dcff8c1e95e5776'),
    'libpthread.so.0': (f'{KIT}/lib/libpthread.so.0', '3cdda9d1c3d3b2010cdfdfd7d789c4884ae6e8f1'),
    'libdl.so.2': (f'{KIT}/lib/libdl.so.2', '36df0e2cb1b5d4c714a0906eb01ef8970ef096bf'),
    'libcrypt.so.1': (f'{KIT}/lib/libcrypt.so.1', 'bddbfbb51d76cbef3a960740ac3ebb2fa79e5ee3'),
}
LIB_LINKS = {'libm.so': 'libm.so.6', 'libpthread.so': 'libpthread.so.0', 'libdl.so': 'libdl.so.2',
             'libcrypt.so': 'libcrypt.so.1', 'libstdc++-libc6.2-2.so': 'libstdc++-libc6.2-2.so.3',
             'libfreetype.so': 'libfreetype.so.6'}
LIBS_L = '-lfreetype -lpthread -lcrypt -lm -ldl -lstdc++-libc6.2-2 -locm -lc'
CRT_HEAD = [f'{KIT}/usr/lib/crt1.o', f'{KIT}/usr/lib/crti.o', f'{GCCLIB}/crtbegin.o']
CRT_TAIL = [f'{GCCLIB}/crtend.o', f'{KIT}/usr/lib/crtn.o']
# known and harmless: chloe_orange.c uses tmpnam
LD_QUIET = ("In function `ChloeWatermelon':", "the use of `tmpnam' is dangerous")


def fail(msg):
    sys.exit(f'error: {msg}')


def run(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)


def sha1(data):
    return hashlib.sha1(data).hexdigest()


def rows(path):
    for line in open(path):
        line = line.split('#', 1)[0].strip()
        if line:
            yield [x.strip() for x in line.split('|')]


def read_units():
    """[(source, cflags, asflags)] in link order."""
    units = []
    for f in rows(UNITS):
        f += ['', '']
        if not os.path.exists(f[0]):
            fail(f'{UNITS}: {f[0]} does not exist')
        units.append((f[0], f[1], f[2]))
    return units


def unit_dir(src):
    return f'{OBJ}/' + re.sub(r'[^A-Za-z0-9_]', '_', src)


def assets():
    """Writes the changed assets; returns how many."""
    if not os.path.exists(ORIG):
        fail(f'{ORIG} missing (see Building in README.md)')
    raw = open(ORIG, 'rb').read()
    if sha1(raw) != ORIG_SHA1:
        fail(f'SHA1 of {ORIG} is not {ORIG_SHA1}')
    from elftools.elf.elffile import ELFFile
    import io
    secs = [s.header for s in ELFFile(io.BytesIO(raw)).iter_sections()
            if s.header.sh_type != 'SHT_NOBITS' and s.header.sh_size]
    wrote = 0
    for name, addr, size, dest in rows(ASSETS):
        a, n = int(addr, 16), int(size)
        h = next((h for h in secs if h.sh_addr <= a and a + n <= h.sh_addr + h.sh_size), None)
        if h is None:
            fail(f'asset {name}: 0x{a:x}+{n} is outside the sections of {ORIG}')
        b = raw[h.sh_offset + a - h.sh_addr:][:n]
        text = '\n'.join([f'/* {name}: from {ORIG} (tools/build.py), do not edit. */']
                         + ['    ' + ', '.join(f'0x{x:02x}' for x in b[i:i + 12]) + ','
                            for i in range(0, n, 12)]) + '\n'
        path = os.path.join('assets', dest)
        if not os.path.exists(path) or open(path).read() != text:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, 'w').write(text)
            wrote += 1
    return wrote


def compile_cmds(src, cflags, asflags, d):
    is_cxx = src.endswith(('.cpp', '.cc'))
    flags = cflags.split()
    # the unit's -I before the kit's headers: a header of the unit is not hidden by a system header
    # with the same name (glibc has an err.h, SSL-C has its own)
    inc = ' '.join([INC_CXX if is_cxx else INC_C] + [f for f in flags if f.startswith('-I')] + [f'-I{KIT}/usr/include'])
    rest = ' '.join(f for f in flags if not f.startswith('-I'))
    cc = f'{TOOL}/g++' if is_cxx else f'{TOOL}/gcc -Wimplicit-function-declaration'
    return (f'{cc} -B{TOOL}/ -S -O2 {inc} {rest} {FIXED_TIME} -Wp,-MD,{d}/out.d -o {d}/out.s {src}',
            f'{AS} -mips2 -m5900 {asflags} -o {d}/out.o {d}/out.s')


def up_to_date(d, key):
    try:
        if open(f'{d}/out.cmd').read() != key:
            return False
        t = os.path.getmtime(f'{d}/out.o')
        deps = open(f'{d}/out.d').read().replace('\\\n', ' ').split(':', 1)[1].split()
    except (OSError, IndexError):
        return False
    return all(os.path.exists(p) and os.path.getmtime(p) <= t for p in deps)


def compile_unit(src, cflags, asflags):
    """Compiles one unit if needed; returns (compiled, error lines)."""
    d = unit_dir(src)
    cc, gas = compile_cmds(src, cflags, asflags, d)
    key = f'{cc}\n{gas}'
    if up_to_date(d, key):
        return False, []
    os.makedirs(d, exist_ok=True)
    for f in ('out.o', 'out.cmd'):
        if os.path.exists(f'{d}/{f}'):
            os.remove(f'{d}/{f}')
    r = run(cc)
    if r.returncode:
        err = [l for l in r.stderr.splitlines() if 'error' in l.lower()][:5] or r.stderr.splitlines()[-5:]
        return True, [f'{src}: compilation failed'] + [f'    {l}' for l in err]
    implicit = re.findall(r"implicit declaration of function [`'\"]?(\w+)", r.stderr)
    # an error in our code; the third-party libraries of ref/ have a few as shipped
    if implicit and src.startswith('src/'):
        return True, [f'{src}: implicit declaration of {", ".join(sorted(set(implicit)))}']
    r = run(gas)
    if r.returncode:
        return True, [f'{src}: assembly failed'] + [f'    {l}' for l in r.stderr.splitlines()[:5]]
    open(f'{d}/out.cmd', 'w').write(key)
    return True, []


def libs():
    """build/libs/: the libraries, the libc script without libc_nonshared.a (stat and lstat are units
    of bn, src/lib/libc_stat.c and libc_lstat.c) and the links -l looks for."""
    os.makedirs(LIBS, exist_ok=True)
    for dst, (src, want) in LIB_FILES.items():
        p = f'{LIBS}/{dst}'
        if not (os.path.exists(p) and sha1(open(p, 'rb').read()) == want):
            if not os.path.exists(src):
                fail(f'{src} missing: run python3 tools/fetch.py (or make kit)')
            shutil.copyfile(os.path.realpath(src), p)
            if sha1(open(p, 'rb').read()) != want:
                fail(f'unexpected sha1 of {src}')
    open(f'{LIBS}/libc.so', 'w').write('GROUP ( libc.so.6 )\n')
    for dev, real in LIB_LINKS.items():
        if os.path.lexists(f'{LIBS}/{dev}'):
            os.remove(f'{LIBS}/{dev}')
        os.symlink(real, f'{LIBS}/{dev}')


def link(units):
    libs()
    objs = CRT_HEAD + [unit_dir(u[0]) + '/out.o' for u in units] + CRT_TAIL
    unstripped = 'build/bn.unstripped.elf'
    for p in (unstripped, ELF):
        if os.path.exists(p):
            os.remove(p)
    # -E: every global goes into .dynsym, the modules of bn/modules/ resolve their imports against it
    r = run(f'{LD} -m elf32lsmip -dynamic-linker /lib/ld.so.1 -o {unstripped} -Map build/bn.map -E '
            f'{" ".join(objs)} -L{LIBS} -rpath-link {LIBS} {LIBS_L} {GCCLIB}/libgcc.a')
    msgs = [l for l in r.stderr.splitlines() if not any(q in l for q in LD_QUIET)]
    if r.returncode:
        fail('link failed\n' + '\n'.join(msgs))
    for l in msgs:
        print(f'warning: ld: {l}', file=sys.stderr)
    r = run(f'{STRIP} -o {ELF} {unstripped}')
    if r.returncode:
        fail(f'strip: {r.stderr.strip()}')


def main():
    args = sys.argv[1:]
    jobs = next((int(a[2:]) for a in args if a.startswith('-j') and a[2:]), os.cpu_count() or 1)
    units = read_units()
    if '--no-assets' not in args:
        n = assets()
        print(f'assets     {n} written' if n else 'assets     up to date')
    with ThreadPoolExecutor(jobs) as ex:
        res = list(ex.map(lambda u: compile_unit(*u), units))
    errs = [l for _, e in res for l in e]
    built = sum(1 for b, e in res if b and not e)
    print(f'units      {len(units)}, {built} compiled' + (f', {sum(1 for _, e in res if e)} failed' if errs else ''))
    if errs:
        print('\n'.join(errs), file=sys.stderr)
        sys.exit(1)
    if '--compile-only' not in args:
        link(units)
        print(f'linked     {ELF}')


if __name__ == '__main__':
    main()
