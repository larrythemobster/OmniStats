# Building OmniStats

OmniStats targets Windows 10 or newer and requires a C++20-capable MSVC toolchain, CMake 3.22 or newer, Git, and vcpkg. The repository contains a vcpkg manifest (`vcpkg.json`); it does not vendor a vcpkg checkout.

## Set up vcpkg

Install vcpkg outside this repository, then set `VCPKG_ROOT` for the current PowerShell session:

```powershell
git clone https://github.com/microsoft/vcpkg.git "$HOME\src\vcpkg"
& "$HOME\src\vcpkg\bootstrap-vcpkg.bat"
$env:VCPKG_ROOT = "$HOME\src\vcpkg"
```

Alternatively, set `VCPKG_TOOLCHAIN_FILE` directly to vcpkg's `scripts\buildsystems\vcpkg.cmake` file.

## Configure and build

```powershell
cmake -S . -B out `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build out --config Release
```

vcpkg installs the manifest dependencies during CMake configuration. Build output stays under `out/`.

## Run

```powershell
.\out\Release\OmniStats.exe
```

## Test

```powershell
ctest --test-dir out -C Release --output-on-failure
```

Some curl-impersonate tests intentionally contact an external test service. Keep those tests separate from an offline verification run when necessary.

## Useful CMake options

| Option | Default | Purpose |
| --- | --- | --- |
| `BUILD_TESTING` | `ON` | Builds tests, benchmarks, and fuzz targets. |
| `OMNISTATS_BUILD_UPDATER` | `ON` | Builds `OmniStatsUpdater.exe`. |
| `OMNISTATS_BUILD_INSTALLER` | `ON` | Builds `OmniStatsInstaller.exe`. |
| `OMNISTATS_ENABLE_IN_APP_UPDATE_LOGIC` | `OFF` | Enables the legacy in-app update path. |
| `OMNISTATS_ENABLE_LOW_LEVEL_HOOK` | `OFF` | Enables the legacy low-level keyboard hook. |
| `ENABLE_SANITIZERS` | `OFF` | Enables address/undefined behavior sanitizers where supported. |
| `ENABLE_TSAN` | `OFF` | Enables ThreadSanitizer on supported non-MSVC toolchains. |
| `ENABLE_COVERAGE` | `OFF` | Enables coverage instrumentation on supported non-MSVC toolchains. |
