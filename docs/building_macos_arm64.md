# Build on Apple Silicon

Use a native arm64 terminal and a clone of `extended-functionality` in a
path without spaces or parentheses. The dependency script uses unquoted
build paths.

## One-time setup

Install full Xcode and complete its first-launch setup. Select that Xcode
with `xcode-select`. If `xcrun metal -v` fails, install its Metal toolchain
with `xcodebuild -downloadComponent MetalToolchain`.

Install CMake, NASM, and pkg-config. With Homebrew, run
`brew install cmake nasm pkgconf`.

From the root of the clone, build the pinned dependencies into `deps-arm64`
once. Repeat this step when the dependency recipe changes.

```sh
bash .github/workflows/scripts/macos/build-dependencies-universal.sh "$PWD/deps-arm64"
```

## Build

From the root of the clone, run:

```sh
cmake -P tools/build-macos-arm64.cmake
```

This builds and packages `bin/PCSX2.app` with the executable at
`bin/PCSX2.app/Contents/MacOS/PCSX2`. Run `ws pcsx2 deploy` to copy the app
from `@pcsx2_fork` to `@pcsx2_dev`. Workshop writes `portable.txt` alongside
the deployed app so PCSX2 uses that directory for its data.

Use the [fork's launch options](launch_options.md) with the bundled executable.
The arm64 build uses interpreters for the EE, IOP, and VUs, which can limit
emulation speed.
