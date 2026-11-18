# open-bn

The source code of `bn`, the main program of the PlayStation BB Navigator
(PSBBN), the PS2 Linux menu, browser and installer that shipped with the
PlayStation 2 network adaptor and HDD in Japan.

The code comes from a matching decompilation, kept in
[MatteoTurri/open-bn](https://github.com/MatteoTurri/open-bn): there the C and
C++ sources, built with the original Sony toolchain, reproduce the shipped
executable byte for byte. This repository starts from that code, as installed by
the PSBBN Definitive Patch, and changes it: fixes and new features.

The repository contains no original binaries or assets. You need your own copy
of the executable (see below); sound banks, encrypted code and other data that
no compiler produced are extracted from it at build time.

## The target

- `bn` is a dynamically linked C++ executable for PS2 Linux (MIPS R5900), not a
  native PS2 program. It loads further modules from `bn/modules/` (music,
  movies, photos, mail, ...), which are not part of this repository.
- Compiler: Sony gcc 2.95.2 (`mipsEEel-linux`, 2.95.2-3a); assembler and linker:
  Sony binutils 2.9EE. Both are rebuilt from the PS2 Linux Kit sources.
- Linked libraries (SSLeay, SQLite, zlib, libpng, libjpeg, libtiff, libmng,
  giflib, the Small script interpreter) are built from period upstream sources
  in `ref/`.
- Original executable: `orig/bn-jpn.elf`, SHA1
  `0bb9908a1d423edc77d116514003866898e5cf6b`. The build takes from it only the
  bytes no compiler produces (see [`docs/building.md`](docs/building.md)).

## Building

Requirements, on Linux (tested on Arch and Ubuntu 22.04/24.04, which Linux Mint
21/22 are based on):

- `git`, `make`, `python3`, `curl` and Docker with the buildx plugin. On
  Ubuntu/Mint: `sudo apt install git make python3 curl docker.io docker-buildx`.
- Your user in the `docker` group (`sudo usermod -aG docker $USER`, then log out
  and back in).
- About 6 GB of disk. The first build needs network access: github.com for the
  clone, archive.org for the kit packages and the link inputs, plus ftp.gnu.org,
  Docker Hub and the Debian/Ubuntu package mirrors while the images build.

Clone the repository and run everything below from its root:

```sh
git clone https://github.com/CosmicScale/open-bn.git
cd open-bn
```

Get the executable (it is the `bn` of the Japanese language pack distributed
with the PSBBN Definitive Patch):

```sh
mkdir -p orig && cd orig
curl -LO 'https://archive.org/download/psbbn-definitive-patch-v4.1/language-pak-jpn-v1.1.0.tar.gz'
tar xzf language-pak-jpn-v1.1.0.tar.gz './__linux.4/bn/bn' && mv __linux.4/bn/bn bn-jpn.elf
rm -r language-pak-jpn-v1.1.0.tar.gz __linux.4
sha1sum bn-jpn.elf    # 0bb9908a1d423edc77d116514003866898e5cf6b
cd ..
```

Set up the toolchain and build:

```sh
make bootstrap      # Docker image, Sony gcc and binutils, PS2 Linux Kit headers
make build          # compile every unit and link build/bn.elf
```

`make bootstrap` takes a few minutes: it builds the toolchain into
`docker/toolchain/` and downloads about 1 GB of PS2 Linux Kit packages from
archive.org. The first `make build` also downloads the PSBBN Definitive Patch
(143 MB) for the libraries `bn` links against, and Unity for the tests, and
compiles all 1,130 units (under a minute on 16 cores); after that only what
changed is rebuilt. After a pull that changes `docker/patches/`, rebuild the
compiler with `make sony-gcc`.

## Repository layout

| Path | Contents |
|---|---|
| `src/` | the Sony code of `bn`, one folder per subsystem (see [`src/README.md`](src/README.md)) |
| `ref/` | third-party libraries, as upstream sources plus the changes Sony's build had |
| `config/` | the compilation units in link order with their flags, the assets taken from the original |
| `docker/` | Dockerfiles and patches that rebuild the Sony toolchain |
| `tests/` | tests of `bn`'s code that run on the console (`make test`, see [`docs/testing.md`](docs/testing.md)) |
| `tools/` | the build (`tools/build.py`), the object comparison (`tools/compare.py`), the tests (`tools/test.py`), the formatter and the setup scripts |
| `docs/` | how the build works and how to try a build on the console, the tests, DNAS, code conventions |
| `orig/`, `assets/`, `build/` | your executable, extracted data, build output (all gitignored) |

## Changing the code

Change the sources, then:

```sh
make build          # build/bn.elf
make compare        # the units whose object changed, against main (REF=<commit> for another)
make test PS2=<ip>  # the tests of tests/, on the console
```

`make compare` compiles the tree of another commit and compares the objects unit by
unit: a unit in the list that you did not mean to touch (a header, a define, a
flag) is the first thing to check. The result is tried on the console:
[`docs/building.md`](docs/building.md) explains how. A change to code that has tests
also runs `make test` ([`docs/testing.md`](docs/testing.md)).

How the code is organized and how to read it: [`src/README.md`](src/README.md).

## License

The tools, build files and documentation written for this project, and the changes and additions
made to `src/` here, are under the MIT license ([`LICENSE`](LICENSE)). The sources in `src/` reconstruct a
program by Sony Computer Entertainment, over which no rights are claimed or granted; the libraries in `ref/`
and the toolchain sources in `docker/` keep their own licenses.
