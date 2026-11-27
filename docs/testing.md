# Testing

`make test PS2=<address>` builds the tests in `tests/` and runs them on a PS2 with the Definitive
Patch. The tests run on the console because that is where `bn`'s code runs: the same compiler, the
same libraries, the same LinuxThreads and pfsd.

```sh
make venv                     # once: telnetlib3 for the runner, in .venv
make test PS2=192.168.1.82    # build, then build/test/ on the console
```

The console must be on, with `bn` idle on a menu, and telnet open (user and password from
`PS2_USER` and `PS2_PASSWORD`, by default those of the Definitive Patch). The console downloads the
programs from the PC on a random TCP port: a firewall on the PC must let it through.

## What happens

1. `make build`, as usual: the tests link the objects of `bn`.
2. `tools/test.py` (container) compiles each test, the helpers of `tests/support/` and
   [Unity](https://github.com/ThrowTheSwitch/Unity) (v2.7.0, downloaded by `tools/fetch.py` into
   `build/dl/unity/`), and links each test with every unit of `bn` into a program in `build/test/`.
   `ld --wrap main` points crt1 at the test's `__wrap_main`: `bn`'s own `main` stays in the link but
   never runs.
3. `tools/test_ps2.py` (host) serves `build/test/` over HTTP, logs in with telnet, downloads the
   programs into `/bntmp/test` with `wget` and runs them one by one, printing their output as it
   comes. A program passes when it exits 0 and Unity's summary ends with `OK`. `/bntmp/test` is
   removed at the end, whatever happened (`--keep` leaves it, with the databases of a program
   that failed). The runner sends short commands: the shell of the console drops long typed lines.

## The tests

- **`import_test`**: the Game Collection import (`bnGameImportStart`, `src/app/bnapp.cpp`) on a
  disk in memory. `tests/support/fakepfs.cpp` defines the `scepfs*` calls, which in `bn` are weak
  aliases of the pfsd client, so the import reads partitions, `SYSTEM.CNF`, `icon.sys` and
  `info.sys` that each test builds. The tests check the rows of `sce_game` and `sce_game_scan`, and
  which partitions an import reads again. A pfsd call the fake does not implement, an open for
  writing and a file left open fail the test; like pfsd, the fake refuses to open a mounted
  partition raw.
- **`import_disk_test`**: the same import on the console's own disk, through pfsd. It reads the
  disk as `bn`'s own import does, and writes only to databases of its own in `/bntmp/test`: an
  import into an empty database, an import of the console's `game.db` that must end with the same
  games, and a second import that must not write anything.

- **`gettext_test`**: the pad-help labels of text input (`GetText`, `src/app/gettext.cpp`) read from the
  `TEXT` elements of an `fg.xml` written in `/bntmp/test`, the built-in labels kept when the file
  or an element is missing, and long labels cut to the 31 bytes the system script `fg` has room for.
- **`amx_loader_test`**: the modules of the pages (`thXmlLoader`, `src/script/thxmlloader.cpp`),
  loaded as `bn` loads a page, from pages written in `/bntmp/test`. The encrypted `parts/focus.bnx`
  of the Definitive Patch is decrypted, and its program is the plain module of the other tests,
  written in `/opt0/bn/script/` under names starting with `.amx_loader_test` (removed at the end,
  and by the runner whatever happened). A module named `.amx` is loaded as it is, also through
  `file://opt0/...` and `/../`; it is refused outside `/opt0/bn/script/` (also through a symbolic
  link), when it is not a whole AMX program (signature, version, size, the blocks within its
  memory) and for a page from a CD-ROM or the network; a plain program named `.bnx` is decrypted,
  and fails.
- **`dnas_ar_test`**: the archive of a download package (`src/dnas/dnas_ar.c`) read from a file on
  the disk in memory: members stored in plaintext (headers, bodies, skips, zero padding at the end,
  truncated, personalized or overlong members), and the streams handed to the DNAS2 reader instead:
  encrypted, truncated, or not starting with a valid member.
- **`input_test`**: the confirm button on the inputs (`src/input/mypad.cpp`, `src/input/irrm.cpp`).
  The pad reads its packets from a pipe, as the driver returns them for a DualShock 2 and for a
  digital pad; the remote is given the keys it would decode. Circle and cross of the pad and of the
  remote follow the setting, the DVD player keys (ENTER, STOP, PREVIOUS, ...) give the same buttons
  with either setting, and the buttons of the remote reach the pages through the pad without being
  swapped again.
- **`sysconf_test`**: the confirm button in the system configuration (`src/app/syscnf.cpp`):
  `[pad] enter` of `system.ini` on a `__sysconf` partition in memory (`fakepfs`, writable there),
  read by `sysconf_read` together with the console's own configuration device, which is only read.
  The value reaches `SysOption` and `GetSystemParam(720)`; values other than 0 and 1, and `enter`
  outside `[pad]`, are ignored; the natives of the settings page refuse them; `systemini_write`
  writes the whole file, adding `[pad]` to a file without it.
- **`xml_entity_test`**: the entities of the buttons in the pages (`src/xml/xml_entitydeclmanager.cpp`),
  parsed as the loader of the pages parses them: `&btn_enter;` and `&btn_back;` give the glyphs of the
  confirm and back buttons and `&btn_enter_name;` `circle` or `cross`, in attribute values and in the
  text of an element (one text node, as the scripts read it), with the setting of the moment the page
  is parsed; the predefined entities of XML are unchanged.

pfsd serves several clients at once, each with its own mount points, so the tests run next to `bn`.
But pfsd refuses to open a partition raw while another client has it mounted, and `bn` keeps
mounted the partitions of the covers it showed last: `import_disk_test` lists those partitions
("mounted by another client, left out") and leaves them out of the comparisons, and skips the
third test.

## When a test goes wrong

The kernel of PSBBN runs without the out-of-memory killer: a program that takes all the memory
freezes the whole console, `bn` included, until it is switched off with a long press of the power
button (the file systems then need an `e2fsck`). So each program limits its address space to
`TEST_MEMORY_KB` and kills itself after `TEST_SECONDS` (`tests/support/testutil.h`, set by
`startTests()`). An import that does not end in a minute is cancelled and its test fails; if
it does not stop either, the program exits rather than run the next test with it still writing.
The runner interrupts a program that is still there a minute after `TEST_SECONDS`.

## Adding a test

A test of an existing program is a `static void test_...(void)` in its file plus a `RUN_TEST` line
in its `__wrap_main`. A new program is a file in `tests/` with its own `__wrap_main` (call
`startTests()` first) and a row in `TESTS` of `tools/test.py`, which lists its sources: add
`tests/support/fakepfs.cpp` to run it on the disk in memory. The functions a test calls are those
`bn` exports: a function that is `static` in its unit cannot be called from a test.
