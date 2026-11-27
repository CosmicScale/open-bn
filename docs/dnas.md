# The protections: DNAS and Chloe

Two parts of `bn` are stored encrypted in the shipped binary. The DNAS code (Sony's network
authentication library) decrypts its own functions in place while they run; the Chloe stages are
encrypted programs that `bn` decrypts and runs once, to reboot the console out of Linux.

## DNAS

**166 regions in 163 functions** of the DNAS code carry a region that is encrypted in the shipped
file. Two families differ in the shim that decrypts them:

| family | regions | shim | units |
|---|---:|---|---|
| DNAS2 | 125 | `__sce_dnas2_symbol11002` (module 11) and `__sce_dnas2_symbol12002` (module 12) | 13 units of `src/dnas/dnas2m11_*` and `dnas2m12_*` |
| DNAS1 | 41 | `_sce_dnas_pwrap_main` | `scednas1_state.c`, `dnas1msg.c`, `dnas1recv.c`, `scednas2.c` |

### In the shipped program

Around each region the function holds a fixed **scaffold**: before the region, a test of a flag word
(already decrypted?), a call to the shim with the region start, and a branch over 11 descriptor words;
after the region, a branch over 10 more descriptor words and a second call to the shim with the region
end. The shim is called as `shim(start_or_end, len ^ code, code, (id << 1) | dir)`:

- `dir = 0` (start): `mprotect` the pages RWX, set the flag word to `start`, decrypt the region,
  flush the caches, `mprotect` RX;
- `dir = 1` (end): the same, clearing the flag and encrypting the region again.

The cipher: `code` selects a recipe of up to seven
steps (count from the top nibble, 5 when it is 0), each one of 8 word primitives with two fixed
constants; decryption uses one bank of primitives, encryption the other. Words that the dynamic loader
rewrites (relocations inside the region) are skipped: their offsets are in tables in `.text`
(`_sce_dnas_pwrap_info_tbl` for DNAS1, `__sce_dnas2_symbol11001`/`12001` for the two DNAS2 shims).

Inside some DNAS1-family regions the protection also hides strings in the instruction stream
(31 sites: server host names and similar): a branch skips a `nop`, two descriptor words and the string,
and a `la` right after it points into the skipped block.

### In this build

The region boundaries are the macros of `src/dnas/dnas_prot.h` (`SCE_DNAS_PROT_BEGIN()`,
`SCE_DNAS_PROT_END()`, `SCE_DNAS1_PROT_*` and `SCE_DNAS_STR("text")` for a string hidden inside a
region). They are empty: the regions are compiled and stored in plaintext, with no scaffold and no
call to the shims, and the hidden strings are ordinary literals. The shims and their exclusion
tables are not part of this build; the scrambling functions still are, but nothing calls them.

The PSBBN Definitive Patch disables the DNAS checks (`default_isp.dat` removed from
`__contents/bn.conf`), so the authentication code is not reached either. The package reader below
still is: installing a download package goes through the DNAS2 code.

### Download packages

The archive inside a download package (`.pkg`, read by `src/install/`) goes through
`src/dnas/dnas_ar.c`. After its 16-byte header, Sony's packages hold an encrypted stream that starts
with the word `0x20005`: `bn` then loads the DNAS2 profile and reads the members through the DNAS2
record reader (`src/dnas/dnas2read.c`). A package whose members are stored in plaintext starts
with its first member instead: each member is a 24-byte header (type 1 directory or 2 file,
attribute, name length, reserved, 64-bit size), the name and the body, read straight from the file.
`bn` reads the archive as plaintext when the stream starts with a valid member header, and hands
anything else to the DNAS2 reader. Zero bytes after the last member end the archive.
A plaintext member cannot be personalized (attribute other than 0), since that needs the DNAS2
session.

## Chloe

`bn` reboots the PS2 out of Linux through the `Chloe*` library (`src/chloe/`, 26 units; the names are
obfuscated, sweets and fruit). The only entry point is `bn_do_reboot()` in `src/app/bnexec.cpp`
(`bnReboot()`, armed by `bn_arm_reboot()`), and it always ends with `execl("/sbin/reboot")`.

- `ChloeBanana("/dev/hda")` reads a 512-byte header (block count and first block at offsets 308/304)
  and loads that program from the disk.
- If it is present, a chain of `Chloe*` calls fills a 112-byte `ChloeObj` (`src/chloe/chloe.h`): the
  loaded program, the download slots, the address of the kernel symbol `sbios` from `/proc/ksyms`, a
  table of libc functions, and a checksum that the steps update and `ChloeTart` checks. Defences on
  the way: `setrlimit(RLIMIT_CORE)`, the `MIPS_TASKFLAGS` `sysmips` call, an `lstat` of a path built
  encrypted, the `/dev/akmem` probe. Then `ChloeMaple` decrypts **stage 1** into a fresh page-aligned
  RWX buffer (XOR with a running key, initial value 35, that adds each encrypted byte rotated left by
  3), calls it with the context and wipes it.
- The other chain (`ChloeCake` .. `ChloePekoe`) writes a buffer to physical memory through
  `/dev/akmem` with the same two requests stage 1 uses (`_IOW('W', 0, int[2])` for the window,
  `_IOW('W', 1, int[3])` to start), then sets `"BootNative"`, `0xBFC00000` and `"DVD Player"` into
  the context.

**Stage 1** (`ChloeDoushite`, 1,152 B of `.data`) is user-space PIC MIPS code, 288
instructions. It opens `/dev/akmem`, decrypts **stage 2** with the same cipher and writes it to
physical memory at `0x81000000`, builds a parameter block from the context (download slots included),
writes the signature `"PS2REBOO"` and sends the start request.

**Stage 2** (`ChloeKnife`, 42,848 B of `.data`) is bare-metal EE code: it loads its
arguments from immediates patched at run time, sets `sp = 0xA0100000`, jumps to `0x01000C00`, and then
programs the INTC (`0xB000F000`) and DMAC (`0xB000E000`) registers, taking the machine back from Linux.

### `/dev/akmem`

`/dev/akmem` ("another kernel memory") is a device of the PSBBN kernel that loads a program into
physical memory and runs it in place of Linux at the next reboot. It comes from a NetBSD driver written
to boot NetBSD from Windows CE on MIPS handhelds; Sony included it in the 2.2.19 kernel of PSBBN. Its
user-space loader `akload.c` and the NetBSD driver patch (2001) are published by the PS2 Linux
community project "akmem boot loader" (https://ps2-linux.tensioncore.com/projects/akmem.html). The
interface, as `akload` and `bn` use it:

1. `_IOW('W', 0, ...)` (`MEMIOCTL_AKALLOC`) takes `{nsegments, segments}` with one segment
   `{start, end}` of physical addresses; the driver reserves kernel pages for that range. In
   `ChloeCake` the range starts at 1 MB.
2. `write()` at a file offset equal to a physical address stores the program into those pages.
3. `_IOW('W', 1, ...)` (`MEMIOCTL_AKCOMMIT`) records the entry point and the arguments `argc`/`argv`
   for the program. The NetBSD header has a fourth word, `env`; `bn` passes three. In `ChloeCake` the
   entry is the load address and the arguments are the download slots.
4. `/sbin/reboot`: the reboot path of the kernel copies the pages to their addresses, flushes the
   caches and jumps to the entry with `a0 = argc`, `a1 = argv`. `akload -r` runs `/sbin/reboot` right
   after the commit, as `bn_do_reboot()` does.

The `Chloe*` functions are source like the rest of the program; the two stages are not: they are the
assets `chloe/stage1.inc` and `chloe/stage2.inc` (`config/assets.txt`), included by
`src/chloe/chloe_stages.c`.
