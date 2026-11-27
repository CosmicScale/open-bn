#!/usr/bin/env python3
"""Build bn and its modules: from the sources to build/bn.elf and build/modules/<name>.so.

Usage: python3 tools/build.py [-jN] [--compile-only] [--no-assets] [TARGET ...]
       (in the open-bn-env container, from the repository root: `make build [T=<target>]`)

The targets are bn, the modules it loads with dlopen from /opt0/bn/modules/ (every directory
config/modules/<name>/ with a units.txt is one) and script, the scripts of the pages. Without
TARGET all of them are built.

1. Assets: every row of config/assets.txt is cut from orig/bn-jpn.elf into assets/<file>, as the
   body of a C initializer that a source #includes. They are never committed.
2. Units: every unit of config/units.txt is compiled with Sony gcc 2.95.2 (-O2 plus the flags of
   its row) and assembled with gas 2.9EE into build/obj/<unit>/out.o. A unit is rebuilt only if its
   command changed or a file in gcc's dependency list is newer than its object. An implicit function
   declaration is an error in src/ (gcc 2.95 only warns: the call silently returns int).
3. Link: the shared libraries go into build/libs/ (from build/dl/, tools/fetch.py, and the kit);
   ld 2.9EE links the crt files and the units in the order of config/units.txt; the result is
   stripped into build/bn.elf.
A module is built the same way from config/modules/<name>/units.txt, without assets: its objects
go into build/modules/<name>/obj/, and ld -shared links them with the crt files, without libraries
(the undefined symbols of a module are resolved by bn and the libraries it has loaded), into
build/modules/<name>.so; every symbol it imports must be defined by build/bn.elf or one of those
libraries, as bn's dlopen requires.

4. Scripts: every src/bnx/<path>.sma is compiled by the Small compiler (docs/scripts.md) into
   build/script/<path>.amx, a plain AMX program that bn loads without decrypting it when a page
   names it, and all of them are packed into build/script.tar.gz, to unpack in /opt0/bn/script/
   on the console.

--compile-only stops after step 2; --no-assets skips step 1 (tools/compare.py, for the tree of another
commit, which uses the assets of this checkout). Sources, configuration and outputs are relative to
the current directory; the toolchain and the kit are those of this checkout.
"""
import hashlib
import os
import re
import shutil
import struct
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
MODULES = 'config/modules'                              # config/modules/<name>/units.txt
MODULES_OUT = 'build/modules'
SCRIPT_SRC = 'src/bnx'                                  # src/bnx/<path>.sma
SCRIPT_WORK = 'build/bnx'                               # the compiler runs in build/bnx/<path>/
SCRIPT_OUT = 'build/script'                             # build/script/<path>.amx
SCRIPT_TAR = 'build/script.tar.gz'

TOOL = f'{ROOT}/docker/toolchain/sony'                  # Sony gcc 2.95.2 (make sony-gcc)
INC = f'{ROOT}/docker/toolchain/include'                # its headers (make headers)
BIN = f'{ROOT}/docker/toolchain/sonybinutils'           # Sony binutils 2.9EE (make sony-binutils)
AS, LD, STRIP = (f'{BIN}/mipsEEel-linux-{t}' for t in ('as', 'ld', 'strip'))
# the pins of docker/Dockerfile.sonybinutils: an as built without
# docker/patches/gas-overlapping-memcpy.patch can turn the lui of a NO_PIC macro into a
# load/store, an ld built without docker/patches/ld-compact-rel-relvaddr.patch links, but
# writes different bytes at every link
TOOL_SHA1 = {AS: 'a7cd60b7f240ac2c387dba861eac6d61ccaab175', LD: 'ccaa3d26986fab487abaf619cfde7c33c7fa6bda'}
KIT = f'{ROOT}/docker/kit/rootfs'                       # the PS2 Linux Kit (make kit)
SC = f'{ROOT}/docker/toolchain/small/sc'                # Small 2.1.0 (make small)
# the options of every script (docs/scripts.md); -i and the paths are added per script
SC_FLAGS = "-d0 -t0 '-\\' -C+ -P+ '-;+'"

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


class Target:
    """bn or a module: its units file and the directory of its objects."""

    def __init__(self, name):
        self.name = name
        self.is_bn = name == 'bn'
        self.units = UNITS if self.is_bn else f'{MODULES}/{name}/units.txt'
        self.obj = OBJ if self.is_bn else f'{MODULES_OUT}/{name}/obj'


def targets(names=(), root='.'):
    """The targets named (all of them if none): bn first, then the modules in name order. root: the
    tree whose config/modules/ is listed (tools/compare.py)."""
    m = os.path.join(root, MODULES)
    mods = sorted(d for d in os.listdir(m) if os.path.exists(f'{m}/{d}/units.txt')) if os.path.isdir(m) else []
    for reserved in ('bn', 'script'):
        if reserved in mods:
            fail(f'{m}/{reserved}/: a module cannot be named {reserved}')
    known = ['bn'] + mods
    bad = [n for n in names if n not in known + ['script']]
    if bad:
        fail(f'unknown target {", ".join(bad)} (targets: {", ".join(known + ["script"])})')
    return [Target(n) for n in known if not names or n in names]


def read_units(path=UNITS):
    """[(source, cflags, asflags)] in link order."""
    units = []
    for f in rows(path):
        f += ['', '']
        if not os.path.exists(f[0]):
            fail(f'{path}: {f[0]} does not exist')
        units.append((f[0], f[1], f[2]))
    return units


def unit_dir(src, obj=OBJ):
    return f'{obj}/' + re.sub(r'[^A-Za-z0-9_]', '_', src)


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


def check_toolchain():
    for path, want in TOOL_SHA1.items():
        if not os.path.exists(path):
            fail(f'{path} missing: run make sony-binutils')
        if sha1(open(path, 'rb').read()) != want:
            fail(f'{path} is not the pinned one (SHA1 {want}): run make sony-binutils')


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


def compile_unit(src, cflags, asflags, obj=OBJ):
    """Compiles one unit if needed; returns (compiled, error lines)."""
    d = unit_dir(src, obj)
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


def link_module(name, units, obj):
    """build/modules/<name>.so: the link line of `g++ -shared`, crt files around the units."""
    d = f'{MODULES_OUT}/{name}'
    so, unstripped = f'{MODULES_OUT}/{name}.so', f'{d}/{name}.unstripped.so'
    objs = [f'{KIT}/usr/lib/crti.o', f'{GCCLIB}/crtbegin.o'] + [unit_dir(u[0], obj) + '/out.o' for u in units] \
        + CRT_TAIL
    for p in (unstripped, so):
        if os.path.exists(p):
            os.remove(p)
    r = run(f'{LD} -m elf32lsmip -shared -soname {name}.so -o {unstripped} -Map {d}/{name}.map {" ".join(objs)}')
    if r.returncode:
        fail(f'link of {name}.so failed\n{r.stderr}')
    for l in r.stderr.splitlines():
        print(f'warning: ld: {l}', file=sys.stderr)
    r = run(f'{STRIP} -o {so} {unstripped}')
    if r.returncode:
        fail(f'strip: {r.stderr.strip()}')
    check_imports(so)
    return so


def scripts():
    """The sources of the scripts, src/bnx/<path>.sma, as <path>."""
    out = []
    for root, _, files in os.walk(SCRIPT_SRC):
        for f in files:
            if f.endswith('.sma'):
                out.append(os.path.relpath(os.path.join(root, f), SCRIPT_SRC)[:-4])
    return sorted(out)


def compile_script(path, includes_mtime):
    """Compiles src/bnx/<path>.sma into build/script/<path>.amx if needed; returns (compiled, error
    lines). The compiler writes its assembler file in the current directory and rebuilds its own
    path from PWD, so it runs in a directory of its own with PWD set. A script is rebuilt when its
    command changed or the source, one of the include files or the compiler is newer than its program."""
    src = os.path.abspath(f'{SCRIPT_SRC}/{path}.sma')
    out = os.path.abspath(f'{SCRIPT_OUT}/{path}.amx')
    work = os.path.abspath(f'{SCRIPT_WORK}/{path}')
    cmd = f'{SC} {SC_FLAGS} -i{os.path.abspath(SCRIPT_SRC)}/include {src} -o{out}'
    try:
        if open(f'{work}/out.cmd').read() == cmd and \
                os.path.getmtime(out) >= max(os.path.getmtime(src), includes_mtime):
            return False, []
    except OSError:
        pass
    os.makedirs(work, exist_ok=True)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    for f in (out, f'{work}/out.cmd'):
        if os.path.exists(f):
            os.remove(f)
    r = subprocess.run(cmd, shell=True, cwd=work, env=dict(os.environ, PWD=work), capture_output=True, text=True)
    # 0: no warnings, 1: warnings only (the scripts compile with the warnings they had), 2: errors,
    # 3: aborted; negative: killed by a signal
    if r.returncode not in (0, 1) or not os.path.exists(out):
        err = [l for l in (r.stdout + r.stderr).splitlines() if 'error' in l.lower()][:5]
        return True, [f'{SCRIPT_SRC}/{path}.sma: compilation failed'] + [f'    {l}' for l in err]
    data = open(out, 'rb').read()
    # what the loader of bn checks before loading a plain program (wholeAmx, src/script/thxmlloader.cpp)
    size, magic, version, cod, dat, hea, stp = (struct.unpack_from('<IHB5xiiii', data) if len(data) >= 56
                                                else (0, 0, 0, 0, 0, 0, 0))
    if not (magic == 0xf1e0 and version == 6 and size == len(data) and 56 <= cod <= dat <= hea <= stp
            and size <= stp):
        return True, [f'{out}: not a whole AMX program of file version 6']
    open(f'{work}/out.cmd', 'w').write(cmd)
    return True, []


def pack_scripts(paths):
    """build/script.tar.gz: build/script/<path>.amx of every script, owned by root, 0644, with a fixed
    time, so that the same programs give the same archive. Written in full at every build (it takes
    well under a second), through a temporary file."""
    import gzip
    import io
    import tarfile
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w', format=tarfile.GNU_FORMAT) as t:
        for p in paths:
            data = open(f'{SCRIPT_OUT}/{p}.amx', 'rb').read()
            info = tarfile.TarInfo(f'{p}.amx')
            info.size, info.mode, info.mtime, info.uname, info.gname = len(data), 0o644, 0, 'root', 'root'
            t.addfile(info, io.BytesIO(data))
    with open(SCRIPT_TAR + '.tmp', 'wb') as f:
        f.write(gzip.compress(buf.getvalue(), mtime=0))
    os.replace(SCRIPT_TAR + '.tmp', SCRIPT_TAR)


def build_scripts(jobs):
    """Step 4; returns the error lines."""
    if not os.path.exists(SC):
        fail(f'{SC} missing: run make small')
    paths = scripts()
    inc = f'{SCRIPT_SRC}/include'
    includes_mtime = max([os.path.getmtime(f'{inc}/{f}') for f in os.listdir(inc)] + [os.path.getmtime(SC)])
    with ThreadPoolExecutor(jobs) as ex:
        res = list(ex.map(lambda p: compile_script(p, includes_mtime), paths))
    errs = [l for _, el in res for l in el]
    built = sum(1 for b, el in res if b and not el)
    failed = sum(1 for _, el in res if el)
    print(f'scripts    {len(paths)}, {built} compiled' + (f', {failed} failed' if failed else ''))
    if not errs:
        pack_scripts(paths)
        print(f'packed     {SCRIPT_TAR}')
    return errs


def dynsym(path):
    """{name: defined} of the .dynsym of an ELF file, without the weak undefined symbols."""
    from elftools.elf.elffile import ELFFile
    with open(path, 'rb') as f:
        s = ELFFile(f).get_section_by_name('.dynsym')
        return {x.name: x['st_shndx'] != 'SHN_UNDEF' for x in s.iter_symbols()
                if x.name and (x['st_shndx'] != 'SHN_UNDEF' or x['st_info']['bind'] != 'STB_WEAK')}


def check_imports(so):
    """ld -shared accepts any undefined symbol, but bn dlopens a module with RTLD_NOW: every symbol
    the module imports must be defined by build/bn.elf or by a library bn loads, or the module does
    not load (none of the original modules imports from another module)."""
    homes = [ELF] + [f'{LIBS}/{n}' for n in LIB_FILES]
    if not all(os.path.exists(p) for p in homes):
        print(f'warning: {so}: imports not checked, build bn first', file=sys.stderr)
        return
    defined = set()
    for p in homes:
        defined |= {n for n, d in dynsym(p).items() if d}
    missing = sorted(n for n, d in dynsym(so).items() if not d and n not in defined)
    if missing:
        fail(f'{so} imports symbols that {ELF} and its libraries do not define (rebuild bn if it is '
             f'out of date): {", ".join(missing)}')


def main():
    sys.stdout.reconfigure(line_buffering=True)  # in order with the warnings on stderr
    args = sys.argv[1:]
    jobs = next((int(a[2:]) for a in args if a.startswith('-j') and a[2:]), os.cpu_count() or 1)
    names = [a for a in args if not a.startswith('-')]
    want_scripts = not names or 'script' in names
    others = [n for n in names if n != 'script']
    tgts = targets(others) if others or not names else []
    check_toolchain()
    work = [(t, read_units(t.units)) for t in tgts]
    for t, units in work:
        if not units:
            fail(f'{t.units} has no units')
    if '--no-assets' not in args and any(t.is_bn for t in tgts):
        n = assets()
        print(f'assets     {n} written' if n else 'assets     up to date')
    with ThreadPoolExecutor(jobs) as ex:
        res = {t.name: list(ex.map(lambda u: compile_unit(*u, t.obj), units)) for t, units in work}
    errs = []
    for t, units in work:
        e = [l for _, el in res[t.name] for l in el]
        built = sum(1 for b, el in res[t.name] if b and not el)
        failed = sum(1 for _, el in res[t.name] if el)
        print(f'units      {t.name}: {len(units)}, {built} compiled' + (f', {failed} failed' if failed else ''))
        errs += e
    if errs:
        print('\n'.join(errs), file=sys.stderr)
        sys.exit(1)
    if '--compile-only' in args:
        return
    for t, units in work:
        if t.is_bn:
            link(units)
            print(f'linked     {ELF}')
        else:
            print(f'linked     {link_module(t.name, units, t.obj)}')
    if want_scripts:
        errs = build_scripts(jobs)
        if errs:
            print('\n'.join(errs), file=sys.stderr)
            sys.exit(1)


if __name__ == '__main__':
    main()
