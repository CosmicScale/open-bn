# open-bn. Most targets run inside the open-bn-env Docker image (make env).
#
#   make build             compile and link build/bn.elf and the modules, build/modules/<name>.so
#                          (needs orig/bn-jpn.elf, see README.md); T=<target> builds only bn or one module
#   make compare REF=main  compare the objects with those built from another commit
#   make test PS2=<ip>     build the tests and run them on the PS2 at <ip> (docs/testing.md)
#   make format            format src/ and tests/ with the pinned clang-format
DOCKER_USER = --user $(shell id -u):$(shell id -g) -e HOME=/tmp
DOCKER = docker run --rm --platform linux/amd64 $(DOCKER_USER) -v "$(CURDIR):/work" open-bn-env

.PHONY: build compare test format format-check env venv need-buildx sony-gcc sony-binutils small kit headers bootstrap

## Build

# Check the formatting of src/ and tests/, then compile every unit and link build/bn.elf and the
# modules of config/modules/ (tools/build.py); T="<target> ..." builds only those (bn, top, ...).
# fetch.py runs on the host because it needs the network (third-party libraries into build/dl/).
build: format-check
	@python3 tools/fetch.py
	@$(DOCKER) python3 tools/build.py $(T)

# Objects that differ from those built from the commit REF (default: main): the units a change touched.
REF ?= main
COMPARE_TREE = build/compare/$(shell echo '$(REF)' | tr -c 'A-Za-z0-9_.\n-' '_')
compare:
	@rm -rf $(COMPARE_TREE) && mkdir -p $(COMPARE_TREE)
	@git archive $(REF) | tar -x -C $(COMPARE_TREE)
	@$(DOCKER) python3 tools/compare.py $(REF)

# The tests in tests/, built against the objects of build (tools/test.py), run on the PS2 at PS2=<address>
# over telnet (tools/test_ps2.py, on the host: it needs the console and .venv from make venv).
test: build
	@[ -n "$(PS2)" ] || { echo "error: PS2=<address of the console> missing (see docs/testing.md)" >&2; exit 2; }
	@.venv/bin/python -c 'import telnetlib3' 2>/dev/null || { echo "error: no telnetlib3 in .venv: run make venv" >&2; exit 2; }
	@$(DOCKER) python3 tools/test.py
	@.venv/bin/python tools/test_ps2.py $(PS2)

# Format src/ and tests/ with the pinned clang-format (tools/format.py); build verifies it first.
format:
	@$(DOCKER) python3 tools/format.py

format-check:
	@$(DOCKER) python3 tools/format.py --check

## Toolchain setup

# Everything a new machine needs: the Docker image, Sony gcc and binutils, the Small compiler, the PS2 Linux Kit headers.
bootstrap: need-buildx env sony-gcc sony-binutils small kit headers
	@echo "bootstrap  done; build with: make build"

env:
	cd docker && docker build --platform linux/amd64 -t open-bn-env .

# Host-side tools in .venv (gitignored): the pinned clang-format, used by tools/format.py on the host
# (pre-commit hook, editor), whose version comes from tools/format.py; telnetlib3 for tools/test_ps2.py.
venv:
	@[ -x .venv/bin/python ] || python3 -m venv .venv
	@.venv/bin/pip install -q --disable-pip-version-check clang-format==$$(python3 tools/format.py --version) telnetlib3==5.0.3
	@echo "venv       $$(.venv/bin/clang-format --version)"

# docker build --output (sony-gcc, sony-binutils, small) needs BuildKit: the buildx plugin.
need-buildx:
	@docker buildx version >/dev/null 2>&1 || { echo "error: docker buildx not found; install it (Ubuntu/Mint: sudo apt install docker-buildx)" >&2; exit 2; }

sony-gcc: need-buildx
	cd docker && docker build --platform linux/amd64 -f Dockerfile.sonygcc --output=toolchain/sony .

sony-binutils: need-buildx
	cd docker && docker build --platform linux/amd64 -f Dockerfile.sonybinutils --output=toolchain/sonybinutils .

# The Small 2.1.0 compiler of the scripts (docker/small/README.md) into docker/toolchain/small/sc.
small: need-buildx
	cd docker && docker build --platform linux/amd64 -f Dockerfile.small --output=toolchain/small .

# glibc and kernel headers from the PS2 Linux Kit disc 2 (archive.org) into docker/kit/rootfs.
kit:
	@python3 tools/fetch_kit.py

headers:
	@$(DOCKER) sh tools/setup_cxx_headers.sh
