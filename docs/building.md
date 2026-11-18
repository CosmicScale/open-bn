# Building

`make build` checks the formatting of `src/`, downloads the third-party libraries (on the host,
`tools/fetch.py`) and runs `tools/build.py` in the `open-bn-env` container. Everything it writes is
under `build/` and `assets/`, both gitignored.

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

The libraries are the ones `bn` runs with: libc, ld.so, libstdc++, libocm and libfreetype of the
Definitive Patch, and libm, libpthread, libdl and libcrypt of the PS2 Linux Kit (the same glibc
2.2.2). `-E` exports every global: the modules of `bn/modules/` resolve symbols against `bn`.
`__DATE__` and `__TIME__` are fixed, so a build does not depend on when it runs.

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

(`python3 -m http.server -d build` on the PC serves `build/bn.elf`.) Then restart the console.
To go back, copy `bn.orig` over `bn`. What a change touches is tried on the console, together with
what is always on the path: the main menu, the Game Collection, launching a game.
