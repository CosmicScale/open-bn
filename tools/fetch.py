#!/usr/bin/env python3
"""Download and extract the third-party files the link needs, into build/dl/.

Usage: python3 tools/fetch.py        (from the repo root, on the host: it needs the network;
       `make build` calls it)

They are third-party binaries: they live only in build/ (gitignored), never in the repo.
Every file is checked against a known sha1; a cached file with the right sha1 is not
downloaded again. Two sources:

  1. GCC-29_1.RPM of the PS2 Linux Kit (disc 2, /SCEI/RPMS/): crtbegin.o, crtend.o and
     libgcc.a, from the disc already downloaded by `make kit` (docker/kit/tmp/disc2.iso; if
     missing tools/fetch_kit.py downloads it), extracted with rpm2cpio + cpio in the
     open-bn-env container into build/dl/gcc29/x/.
  2. PSBBN Definitive Patch v4.1.0 (archive.org, 143 MB): PSBBN's libc and ld.so, libstdc++,
     libocm and libfreetype, the libraries bn runs with. Extracted into build/dl/dp41/ under
     the real file name (the tar has symlinks).
"""
import hashlib, os, struct, subprocess, sys, tarfile, urllib.request

VERBOSE = bool(os.environ.get('VERBOSE')) or '-v' in sys.argv[1:]

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

DL = 'build/dl'                                      # everything downloaded, relative to the root
GCC29 = f'{DL}/gcc29/x'                              # GCC-29_1.RPM extracted
GCCLIB = f'{GCC29}/usr/lib/gcc-lib/mipsEEel-linux/2.95.2'   # crtbegin.o, crtend.o, libgcc.a
DP = f'{DL}/dp41'                                    # files of the Definitive Patch v4.1.0
KIT_DISC2 = 'docker/kit/tmp/disc2.iso'               # disc 2 of the kit (tools/fetch_kit.py)

GCC_RPM = f'{DL}/gcc29/GCC-29_1.RPM'
GCC_RPM_ISO_PATH = '/SCEI/RPMS/GCC-29_1.RPM'
GCC_RPM_SHA1 = '327f42f209dbb1967e8b14591dca6d467143c9e4'
GCC_FILES = {  # files extracted from the RPM -> sha1
    'usr/lib/gcc-lib/mipsEEel-linux/2.95.2/crtbegin.o': 'a20c88e0d19e777f20aee0ae94a3a40d7e996cb6',
    'usr/lib/gcc-lib/mipsEEel-linux/2.95.2/crtend.o': 'b2e8c0dfd30f8bfcd592e38541e0369528ca7a17',
    'usr/lib/gcc-lib/mipsEEel-linux/2.95.2/libgcc.a': 'd39ec0cb438e5cca7244c3799afedf0ec884086d',
}

DP_URL = ('https://archive.org/download/psbbn-definitive-patch-v4.1/'
          'psbbn-definitive-patch-v4.1.0.tar.gz')
DP_TAR = f'{DL}/psbbn-definitive-patch-v4.1.0.tar.gz'
DP_TAR_SHA1 = '21501ac7e2b77807dddfecb83dd98e3f9eceb2cb'
DP_FILES = {  # tar member (the real file, not the symlink) -> sha1
    './__linux.1/lib/libc-2.2.2.so': '744284e54196c4a97948c7498689d0525ec4232b',
    './__linux.1/lib/ld-2.2.2.so': 'e2827e1661db147f3024cf80213ab17adf5fe390',
    './__linux.1/usr/lib/libstdc++-3-libc6.2-2-2.10.0.so': 'cb6dba42929cf2fadf8505961ae1b934e454bcb4',
    './__linux.4/bn/openmg/libocm.so.1.3': 'b594348548288815bfd054fb531b136ab55db067',
    './__linux.4/bn/usr/lib/libfreetype.so.6.1.0': '9be5fd94ac92f70a41393c7be831ceb13ec2f2e7',
}


def sha1(p):
    h = hashlib.sha1()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def ok(p, want):
    return os.path.exists(p) and (want is None or sha1(p) == want)


# ---- 1. GCC-29_1.RPM -----------------------------------------------------------------

def iso_extract(iso, path, dest):
    """Extract the file `path` (absolute, ISO 9660 level 1 names) from the ISO."""
    from fetch_kit import iso_walk
    with open(iso, 'rb') as fh:
        fh.seek(16 * 2048)
        pvd = fh.read(2048)
        if pvd[1:6] != b'CD001':
            sys.exit(f'error: {iso} is not an ISO9660 image')
        root = pvd[156:190]
        lba, size = struct.unpack('<I', root[2:6])[0], struct.unpack('<I', root[10:14])[0]
        cur = ''
        for part in path.strip('/').split('/'):
            want = f'{cur}/{part}'
            hit = next((x for x in iso_walk(fh, lba, size, cur) if x[0] == want), None)
            if hit is None:
                sys.exit(f'error: {want} not found in {iso}')
            cur, lba, size = want, hit[1], hit[2]
        fh.seek(lba * 2048)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, 'wb') as out:
            out.write(fh.read(size))


def fetch_gcc():
    if not all(ok(f'{GCC29}/{p}', w) for p, w in GCC_FILES.items()):
        if not ok(GCC_RPM, GCC_RPM_SHA1):
            if not os.path.exists(KIT_DISC2):
                print(f'fetch: {KIT_DISC2} missing, downloading it with tools/fetch_kit.py (disc only)')
                import fetch_kit
                off, csize, usize = fetch_kit.zip_member(fetch_kit.ITEM_URL, fetch_kit.DISC)
                fetch_kit.download_iso(fetch_kit.ITEM_URL, off, csize, usize, KIT_DISC2)
            iso_extract(KIT_DISC2, GCC_RPM_ISO_PATH, GCC_RPM)
            if sha1(GCC_RPM) != GCC_RPM_SHA1:
                sys.exit(f'error: unexpected sha1 of {GCC_RPM}')
        os.makedirs(GCC29, exist_ok=True)
        # rpm2cpio/cpio in the container (the host may lack them), with the host's user
        # so the files are not owned by root
        script = (f'set -e; cd /work/{GCC29}; rpm2cpio /work/{GCC_RPM} | cpio -idmu --quiet '
                  + ' '.join(GCC_FILES))
        subprocess.run(['docker', 'run', '--rm', '--platform', 'linux/amd64', '--network', 'none',
                        '--user', f'{os.getuid()}:{os.getgid()}', '-v', f'{os.getcwd()}:/work',
                        'open-bn-env', 'sh', '-c', script], check=True)
        for p, w in GCC_FILES.items():
            if not ok(f'{GCC29}/{p}', w):
                sys.exit(f'error: {p} not extracted from {GCC_RPM} or unexpected sha1')
    if VERBOSE:
        print(f'ok  crtbegin.o crtend.o libgcc.a  -> {GCCLIB}')


# ---- 2. Definitive Patch v4.1.0 --------------------------------------------------------

def fetch_dp():
    dst = {m: f'{DP}/{os.path.basename(m)}' for m in DP_FILES}
    if all(ok(dst[m], w) for m, w in DP_FILES.items()):
        if VERBOSE:
            print(f'ok  {len(DP_FILES)} Definitive Patch files -> {DP} (cached)')
        return
    if not ok(DP_TAR, DP_TAR_SHA1):
        os.makedirs(DL, exist_ok=True)
        print(f'fetch: downloading {DP_URL} (143 MB)')
        with urllib.request.urlopen(DP_URL, timeout=60) as r, open(DP_TAR + '.part', 'wb') as f:
            for b in iter(lambda: r.read(1 << 20), b''):
                f.write(b)
        os.rename(DP_TAR + '.part', DP_TAR)
        if sha1(DP_TAR) != DP_TAR_SHA1:
            sys.exit(f'error: unexpected sha1 of {DP_TAR}')
    os.makedirs(DP, exist_ok=True)
    with tarfile.open(DP_TAR, 'r:gz') as t:
        for m in DP_FILES:
            ti = t.getmember(m)
            if not ti.isfile():
                sys.exit(f'error: {m} in the tar is not a regular file')
            with t.extractfile(ti) as src, open(dst[m], 'wb') as out:
                out.write(src.read())
    for m, w in DP_FILES.items():
        if sha1(dst[m]) != w:
            sys.exit(f'error: unexpected sha1 of {dst[m]}')
    if VERBOSE:
        print(f'ok  {len(DP_FILES)} Definitive Patch files -> {DP}')


def main():
    fetch_gcc()
    fetch_dp()


if __name__ == '__main__':
    main()
