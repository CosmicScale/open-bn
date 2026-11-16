# Build environment for open-bn (image open-bn-env): python, the i386 libc the Sony
# toolchain needs, and the pinned clang-format. The Sony toolchain is mounted at runtime (see below).
# Run it as linux/amd64 (emulated on Apple Silicon).
FROM debian:bookworm

# libc6:i386: the Sony gcc and binutils are i386 binaries.
# patch: make headers (tools/setup_cxx_headers.sh). rpm2cpio, cpio: tools/fetch.py and tools/fetch_kit.py.
# python3-pyelftools: tools/build.py reads orig/bn-jpn.elf (assets).
RUN dpkg --add-architecture i386 && apt-get update && apt-get install -y --no-install-recommends \
    make python3 patch rpm2cpio cpio libc6:i386 python3-pyelftools \
    && rm -rf /var/lib/apt/lists/*

# Source formatter, pinned: tools/format.py accepts only this version (make format, make build); keep in sync with it.
RUN apt-get update && apt-get install -y --no-install-recommends python3-pip \
    && pip3 install --no-cache-dir --break-system-packages clang-format==23.1.5 \
    && rm -rf /var/lib/apt/lists/*

# The Sony toolchain (rebuilt gcc 2.95.2-3a and binutils 2.9EE) is not baked into
# the image: it is mounted at runtime from docker/toolchain/.
WORKDIR /work
