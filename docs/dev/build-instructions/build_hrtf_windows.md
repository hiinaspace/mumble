# Building HRTF + Spatial Room on Windows

Quick guide for building the `spatial-room` branch with HRTF support on Windows.

## Prerequisites

- Visual Studio 2022 with C++ desktop workload and MFC
- CMake 3.23+ and Ninja (both available via VS installer or winget)
- Git

## 1. Set up the Mumble vcpkg environment

Follow the standard [static build instructions](build_static.md). Clone the Mumble
vcpkg fork and run its install script, or download the pre-built environment from
<https://github.com/mumble-voip/vcpkg/releases/tag/2025-11>.

The triplet is `x64-windows-static-md`.

## 2. Install libmysofa into vcpkg

libmysofa is not in Mumble's pre-built environment, so install it manually:

```cmd
cd <vcpkg_dir>
vcpkg install libmysofa:x64-windows-static-md
```

This pulls libmysofa 1.3.2 + its zlib dependency (zlib is likely already present).

## 3. Clone and checkout

```cmd
git clone --recurse-submodules https://github.com/hiinaspace/mumble.git
cd mumble
git checkout spatial-room
git submodule update --init --recursive
```

## 4. Configure and build

Open an **x64 Native Tools Command Prompt** (search "x64" in Start menu —
must say `Environment initialized for: 'x64'`).

```cmd
cmake -S . -B build -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -Dstatic=ON ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md ^
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg_dir>/scripts/buildsystems/vcpkg.cmake ^
  -DIce_HOME=<vcpkg_dir>/installed/x64-windows-static-md ^
  -Dserver=OFF ^
  -Doverlay=OFF ^
  -Dplugins=OFF

ninja -C build
```

Replace `<vcpkg_dir>` with the actual path to your vcpkg directory.

Check cmake output for:
- `HRTF binaural spatialization: ON` (confirms libmysofa was found)
- `Spatial Room plugin: ON`

If HRTF shows OFF, cmake will print a warning about libmysofa not being found.
Use `-Ddebug-dependency-search=ON` to debug.

## 5. Run

The built `mumble.exe` will be in `build/`. The default SOFA file
(`data/hrtf/default.sofa`) is referenced by absolute path at compile time, so it
works from the build tree without installing.

## 6. Test with a friend

1. Both connect to the same Mumble server + channel
2. Both open Configure > Plugins, find "Spatial Room", open its config
3. Check "Activated" and pick different seats
4. In Audio Output settings, enable positional audio (check the box, set
   minimum distance ~0.5m, maximum ~5m)
5. If HRTF is built in, check "HRTF binaural rendering" in Audio Output settings
6. Talk — you should hear each other from the direction of their seat
