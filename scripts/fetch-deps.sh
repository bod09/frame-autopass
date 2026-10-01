#!/bin/sh
# Fetch the build dependencies into gitignored directories. Nothing is
# installed system-wide.
set -eu
cd "$(dirname "$0")/.."

ZIG_VERSION=0.16.0
ZIG_SHA256=70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00
OPENVR_COMMIT=0924064316de3effbcd1acf1e309182a2deb1c05  # OpenVR SDK 2.15.6

if [ ! -x toolchain/zig-x86_64-linux-$ZIG_VERSION/zig ]; then
    mkdir -p toolchain
    tarball=zig-x86_64-linux-$ZIG_VERSION.tar.xz
    curl -sSfL -o toolchain/$tarball https://ziglang.org/download/$ZIG_VERSION/$tarball
    echo "$ZIG_SHA256  toolchain/$tarball" | sha256sum -c
    tar -C toolchain -xf toolchain/$tarball
    rm toolchain/$tarball
fi

if [ ! -f third_party/openvr/headers/openvr.h ]; then
    mkdir -p third_party/openvr
    git -C third_party/openvr init -q
    git -C third_party/openvr fetch -q --depth 1 https://github.com/ValveSoftware/openvr $OPENVR_COMMIT
    git -C third_party/openvr checkout -q FETCH_HEAD
    # The SDK repo ships prebuilt binaries for every platform; only the
    # headers and the loader source are needed.
    rm -rf third_party/openvr/bin third_party/openvr/lib third_party/openvr/samples \
        third_party/openvr/unity_package third_party/openvr/controller_callouts third_party/openvr/.git
fi
