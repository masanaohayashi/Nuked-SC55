# Building

Requirements:

- cmake
- SDL2
- rtmidi (Linux, Mac only)

Tested compilers:

- msvc 19.39.33523
- clang 19.1.7
- gcc 14.2.0

Full build

```bash
git clone git@github.com:jcmoyer/Nuked-SC55.git
cd Nuked-SC55
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

If you're building a binary to only run on your local machine, consider adding
`-DCMAKE_CXX_FLAGS="-march=native -mtune=native"
-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` to the first cmake command above to
enable more optimizations.

After building, you can create a self-contained install with any required files
in their correct locations under `<path>`:

```bash
cmake --install . --prefix=<path>
```

### Linux release archive from macOS

Docker Desktop and Docker Buildx can build the Linux binaries on macOS for both
supported architectures. From the repository root, run:

```bash
./scripts/linux/package-release.sh
```

This creates `dist/SC-55-Linux-x64-<version>.tar.gz` and
`dist/SC-55-Linux-arm64-<version>.tar.gz`. To build only one architecture, pass
`--architecture x64` or `--architecture arm64`. Each archive contains the JUCE
standalone app at `bin/SC-55` and the VST3 plug-in at
`lib/vst3/SC-55.vst3`; runtime libraries are not bundled. The Linux build uses
the Projucer-generated `Plugins/Builds/LinuxMakefile` exporter inside Docker.
The script reads `<version>` from `Plugins/Nuked-SC55.jucer` and disables the
Docker Buildx cache by default so every invocation rebuilds the binaries; pass
`--version` to override the release version or `--cache` to opt into cached
development builds.

### Windows

For builds using msvc you will most likely need to pass
`-DCMAKE_PREFIX_PATH=<path>` where `<path>` points to a directory containing
SDL2, and optionally rtmidi (only when `-DUSE_RTMIDI=ON`).

cmake is expecting to find `<path>/SDL2-X.YY.Z/cmake/sdl2-config.cmake`.

For builds in an msys2 environment, installing SDL2 via pacman should be
enough.

#### ASIO (optional)

To enable ASIO support, pass `-DNUKED_ENABLE_ASIO=ON` and
`-DNUKED_ASIO_SDK_DIR=<path>` where `<path>` points to the extracted ASIO SDK
obtained from [here](https://www.steinberg.net/developers/).

#### Windows release package from macOS with Parallels

When Windows 11 is running in Parallels Desktop, the Windows packaging script
can be invoked from macOS through the Parallels shared-folder mapping:

```bash
./scripts/windows/package-release.sh --architecture x64
./scripts/windows/package-release.sh --architecture arm64
```

The wrapper calls `scripts/windows/package-release.ps1` inside the running VM.
It assumes the repository is mapped to
`C:\Mac\Home\Documents\src\Nuked-SC55-jcmoyer`, and writes the installers to
the shared `dist/` directory. Use `--architecture all` to create both
architectures, `--vm` to select another VM, or `--windows-repo` to override the
Windows-side repository path. Parallels Tools must be installed in the guest
and the Windows VM must already be running.

### All desktop packages and GitHub draft release

To build all desktop packages and create one draft GitHub Release with every
artifact attached, configure `scripts/macos/config.env`, authenticate with
`gh`, start Docker Desktop and the Windows 11 Parallels VM, and run from a
clean, pushed `master` checkout:

```bash
./scripts/package-release.sh
```

This builds a universal macOS DMG, Windows x64 and ARM64 installers, and Linux
x64 and arm64 archives. The script checks that `HEAD` exactly matches the
configured remote branch before building and again before creating the tag.
It never publishes the release automatically; the resulting GitHub Release is
always a draft for review.

# Development

Requirements:

- Python 3
- [Catch2 v3.7.0](https://github.com/catchorg/Catch2) installed in
  `CMAKE_PREFIX_PATH`

There is a test suite that makes sure new commits don't change existing
behavior. It is expected that all tests pass for every commit on master.

You can run the test suite by configuring with the following cmake variables:

- `-DNUKED_ENABLE_TESTS=ON`: when set, the following variables must all be set
  as well.
- `-DNUKED_TEST_ROMDIR=<path>`: `<path>` should point to a directory containing
  the romsets listed below.
- `-DNUKED_TEST_JV880_NVRAM=<path>`: `<path>` should point to a file containing
  nvram dumped from the JV-880 immediately after it has been reset to the
  factory preset. This can be obtained by launching nuked-sc55 with `--romset
  jv880 --nvram <path>`. Once the emulator has started, press `T` to enter the
  utility menu, then press `.` until `Util:Factory preset` appears. Press `G`
  twice and close the emulator. The file `<path>` should contain a 32K nvram
  dump. Note that the actual filename will have a number appended to it. This
  is the emulator instance number and should **not** be included in the
  filename passed to cmake.

Currently these tests require SC-55 (v1.21), SC-55mk2, and JV-880 romsets with
these SHA-256 hashes:

```
7e1bacd1d7c62ed66e465ba05597dcd60dfc13fc23de0287fdbce6cf906c6544 *sc55_rom1.bin
effc6132d68f7e300aaef915ccdd08aba93606c22d23e580daf9ea6617913af1 *sc55_rom2.bin
5655509a531804f97ea2d7ef05b8fec20ebf46216b389a84c44169257a4d2007 *sc55_waverom1.bin
c655b159792d999b90df9e4fa782cf56411ba1eaa0bb3ac2bdaf09e1391006b1 *sc55_waverom2.bin
334b2d16be3c2362210fdbec1c866ad58badeb0f84fd9bf5d0ac599baf077cc2 *sc55_waverom3.bin

8a1eb33c7599b746c0c50283e4349a1bb1773b5c0ec0e9661219bf6c067d2042 *rom1.bin
a4c9fd821059054c7e7681d61f49ce6f42ed2fe407a7ec1ba0dfdc9722582ce0 *rom2.bin
b0b5f865a403f7308b4be8d0ed3ba2ed1c22db881b8a8326769dea222f6431d8 *rom_sm.bin
c6429e21b9b3a02fbd68ef0b2053668433bee0bccd537a71841bc70b8874243b *waverom1.bin
5b753f6cef4cfc7fcafe1430fecbb94a739b874e55356246a46abe24097ee491 *waverom2.bin

aabfcf883b29060198566440205f2fae1ce689043ea0fc7074842aaa4fd4823e *jv880_rom1.bin
ed437f1bc75cc558f174707bcfeb45d5e03483efd9bfd0a382ca57c0edb2a40c *jv880_rom2.bin
aa3101a76d57992246efeda282a2cb0c0f8fdb441c2eed2aa0b0fad4d81f3ad4 *jv880_waverom1.bin
a7b50bb47734ee9117fa16df1f257990a9a1a0b5ed420337ae4310eb80df75c8 *jv880_waverom2.bin
```

Additionally, the file containing the JV-880 factory preset nvram dump should
have the following SHA-256 hash:

```
d5da784546f9fd482c82beb366c527f313e8ea81bc9039dbb8c531197aa6d207 *jv880/nvram0
```

After cmake has configured the build, you can run the test suite:

```
$ cmake --build . --config Release && ctest . -C Release
```

Note that these tests take a long time to finish individually, so you may want
to pass `-j` to run them in parallel.
