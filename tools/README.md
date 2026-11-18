# tools/

All tools run from the repository root. Those marked [container] run inside the `open-bn-env` image
(`docker run --rm --platform linux/amd64 -v "$PWD:/work" open-bn-env python3 ...`) or through the
Makefile target shown.

## Build (`make build`, `make compare`; docs/building.md)

- **`build.py`** [container] `make build`: assets, compilation of every unit (incremental), link and
  strip into `build/bn.elf`.
  - `python3 tools/build.py [-jN] [--compile-only] [--no-assets]`
- **`compare.py`** [container] `make compare REF=<commit>`: compiles the tree of another commit and lists the
  units whose object differs from the current one.
- **`fetch.py`** `make build`: downloads the third-party files of the link into `build/dl/` (on the host:
  it needs the network): crtbegin.o, crtend.o and libgcc.a of the kit, the libraries of the Definitive Patch,
  the sources of Unity for the tests.

## Tests (`make test PS2=<address>`; docs/testing.md)

- **`test.py`** [container] `make test`: compiles the tests of `tests/` and links each with the objects of `bn` into a
  program for the console, in `build/test/`.
  - `python3 tools/test.py [-jN]`
- **`test_ps2.py`** (host, `.venv/bin/python`): copies the programs of `build/test/` to a PS2 over telnet,
  runs them and reports which passed.
  - `.venv/bin/python tools/test_ps2.py HOST [--keep]`

## Formatter (`make format`)

- **`format.py`** [container] `make format`: Format the sources of src/ and tests/ with clang-format 23.1.5,
  the only version accepted.
  - `python3 tools/format.py [--check] [--version] [FILE ...]`
- **`hooks/pre-commit`**: Optional pre-commit hook: refuses a commit whose staged src/ and tests/ files are
  not formatted.

The container has the pinned clang-format (docker/Dockerfile), so `make format` needs nothing else.
On the host (editor format-on-save, the hook), `make venv` installs it into `.venv` (gitignored, together with
the telnetlib3 of `tools/test_ps2.py`), like a
lockfile-pinned dev dependency; `tools/format.py` picks the first of `CLANG_FORMAT`, `.venv/bin/clang-format`
and `clang-format` in PATH that has the pinned version, so the system clang-format does not matter:

```sh
make venv                                             # once, and again after a version bump
python3 tools/format.py --check                       # what make build does first, on the host
.venv/bin/clang-format                                # the binary to give the editor
```

To bump the version: `VERSION` in `tools/format.py` and the pin in `docker/Dockerfile`, then `make venv`,
`make env`, `make format`.

Another version of clang-format formats differently: `tools/format.py` refuses it (exit 2).

## Generators of tracked sources

- **`gen_cloud_noise.py`**: Generate src/scene/cloud_noise.inc: the initializer of `s_noise` (src/scene/cloud.cpp).
  - `python3 tools/gen_cloud_noise.py            # rewrite src/scene/cloud_noise.inc`
- **`gen_sqlite.sh`**: Regenerates the generated source files of SQLite 2.7.2 in ref/sqlite-2.7.2/gen/

## Environment

- **`fetch_kit.py`**: Fetches the pieces of the PS2 Linux Kit (disc 2) the build needs (`make kit`).
  - `Usage: python3 tools/fetch_kit.py [--rpm-glob GLOB ...] [--purge-iso]`
- **`setup_cxx_headers.sh`** [container] `make headers`: Prepares docker/toolchain/include/ to compile C++
  with the Sony g++.
