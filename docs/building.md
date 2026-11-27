# Building

`make build` checks the formatting of `src/` and `tests/`, downloads the third-party libraries (on the host,
`tools/fetch.py`) and runs `tools/build.py` in the `open-bn-env` container. Everything it writes is
under `build/` and `assets/`, both gitignored.

It builds `bn`, its modules, the shared objects `bn` loads from `/opt0/bn/modules/` (every directory
`config/modules/<name>/` with a `units.txt` is one), and the scripts of the pages. `make build
T=<target>` builds only `bn`, one module or the scripts (`script`); `T="bn top"` for several.

## Steps of `tools/build.py`

1. **Assets.** The rows of `config/assets.txt` are cut from `orig/bn-jpn.elf` into `assets/`, as the
   body of a C initializer that a source includes (`#include "../assets/chloe/stage1.inc"`): sound
   banks, VU1 microcode, the encrypted Chloe stages, the ATOK tables, DNAS keys and tables, CFD keys.
   The SHA1 of each piece is checked.
2. **Units.** Every unit of `config/units.txt` is compiled with Sony gcc 2.95.2 at `-O2` plus the
   flags of its row, and assembled with gas 2.9EE into `build/obj/<unit>/out.o`. A unit is rebuilt
   only if its command changed or a file in gcc's dependency list is newer than its object. An
   implicit function declaration is an error in `src/` (gcc 2.95 only warns about it).
3. **Link.** The shared libraries are copied into `build/libs/`; ld 2.9EE links, with its
   built-in script, the kit's `crt1.o`/`crti.o`/`crtbegin.o`, the units in the order of
   `config/units.txt`, then `crtend.o`/`crtn.o`, libgcc and the libraries; the result is stripped
   into `build/bn.elf` (`build/bn.unstripped.elf` and `build/bn.map` stay next to it).
4. **Scripts.** Every `src/bnx/<path>.sma` is compiled by the Small compiler (`make small`, options
   in `docs/scripts.md`) into `build/script/<path>.amx`, a plain AMX module, in a work directory of
   its own under `build/bnx/`. A script is rebuilt only if its command changed or its source, an
   include file or the compiler is newer than its module. All of them are packed again at every
   build into `build/script.tar.gz`, with fixed owner, mode and times. `make compare` does not
   compare the scripts.

The libraries are the ones `bn` runs with: libc, ld.so, libstdc++, libocm and libfreetype of the
Definitive Patch, and libm, libpthread, libdl and libcrypt of the PS2 Linux Kit (the same glibc
2.2.2). `-E` exports every global: the modules of `bn/modules/` resolve symbols against `bn`.
`__DATE__` and `__TIME__` are fixed, so a build does not depend on when it runs, and in the container
(where the tree is `/work`: the paths of the headers end up in `bn`'s assert strings) the same sources
give the same `bn.elf` and modules byte for byte. ld 2.9EE left a field of the `.compact_rel` entries
uninitialized, which `docker/patches/ld-compact-rel-relvaddr.patch` sets to 0. gas 2.9EE moved two
blocks with `memcpy` where source and destination can overlap, so its output depended on the libc it
runs on (the NO_PIC expansion of a load/store from `sym(reg)` could lose its `lui`);
`docker/patches/gas-overlapping-memcpy.patch` uses `memmove` there. `tools/build.py` checks that the
as and the ld in use are the patched ones.

## Modules

A module is built like `bn` from `config/modules/<name>/units.txt`, without assets: its objects go
into `build/modules/<name>/obj/`, and ld links them with `-shared -soname <name>.so` between the kit's
`crti.o`/`crtbegin.o` and `crtend.o`/`crtn.o`, as `g++ -shared` does, into `build/modules/<name>.so`.
No library is linked: the undefined symbols of a module (functions of `bn` such as `amx_GetAddr`, and
of libc, libpthread and libstdc++) are resolved by `bn` and the libraries it has already loaded.
`bn` opens a module with `RTLD_NOW`, so a symbol nobody defines would make it fail to load: after the
link, the build checks every symbol the module imports against `build/bn.elf` and `build/libs/` (it
warns and skips the check when `bn` has not been built yet).

Each module carries its own copy of the libgcc members it uses (exception handling, RTTI,
`operator delete`): they are built from `src/modules/libgcc/` and `ref/gcc-2.95.2/` with the flags of
gcc's libgcc build. A new module gets its sources in `src/modules/<name>/` and its `units.txt`,
which lists those shared members too.

## Adding a unit

A new `.c`/`.cpp` gets a row in `config/units.txt` (`source | cflags | asflags`), at the place in
the link order where its code belongs: usually right after the unit it works with. Code that the
`src/` units call goes in `src/`, a header next to its `.cpp`.

## Checking a change: `make compare`

`make compare REF=<commit>` (default `main`) extracts the tree of that commit into `build/compare/`,
compiles it, and compares its objects with the current ones, section by section. It lists the units
whose object changed, and those added or removed. The list should hold the units the change meant
to touch and nothing else. Both trees are compiled by the current `tools/build.py`: a change to the
build itself (base flags, includes) and anything only the link sees (libraries, crt files, order of
the units) do not show up.

## Trying a build on the console

`bn` is `/opt0/bn/bn` on the PS2 (the `__linux.4` partition), started once by
`/etc/rc.d/rc.bn` at boot. With the Definitive Patch, telnet is open (user `root`, see the
Definitive Patch documentation for the password):

```sh
cp /opt0/bn/bn /opt0/bn/bn.orig       # once: keep the installed one
wget -O /opt0/bn/bn.new http://<pc>:8000/bn.elf && chmod 755 /opt0/bn/bn.new && mv /opt0/bn/bn.new /opt0/bn/bn
```

(`python3 -m http.server -d build` on the PC serves `build/bn.elf`.) A module goes next to it, in
`/opt0/bn/modules/`:

```sh
cd /opt0/bn/modules
[ -e top.so.orig ] || cp -p top.so top.so.orig      # once: keep the installed one
wget -O top.so.new http://<pc>:8000/modules/top.so && chmod 700 top.so.new && mv top.so.new top.so
```

`top.so` is loaded by the Top Menu (`/opt0/bn/script/top.xml`) at every boot.

The scripts go next to the encrypted modules, in `/opt0/bn/script/`, and run where a page names
them: `<SCRIPT src="../parts/album_item.amx" ...>` (`docs/scripts.md`). The page XMLs come with
the language pack of the Definitive Patch and name the encrypted modules; to try the scripts, the
second line below keeps the XMLs once and the last one makes every `<SCRIPT>` name its `.amx`:

```sh
cd /opt0/bn/script
[ -e ../script-xml.orig.tar.gz ] || find . -name '*.xml' | tar czf ../script-xml.orig.tar.gz -T -   # once
wget -O /tmp/script.tar.gz http://<pc>:8000/script.tar.gz && tar xzf /tmp/script.tar.gz && rm /tmp/script.tar.gz
find . -name '*.xml' | xargs perl -pi -e 's{(<SCRIPT\b[^>]*\bsrc=")([^"]+?)(?:\.amx)?"}{$1$2.amx"}g'
```

Only a `bn` that loads plain modules opens these pages, so install it first, and open no page
until the console has restarted: the `bn` still running cannot load them. Then restart the
console. To go back, put the XMLs back first (`tar xzf ../script-xml.orig.tar.gz` in
`/opt0/bn/script`), then copy the `.orig` files back; the `.amx` files can stay.

What a change touches is tried on the console, together with what is always on the path: the
main menu, the Game Collection, launching a game.
