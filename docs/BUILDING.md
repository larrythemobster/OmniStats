# Building

OmniStats targets Windows 10 or newer and requires Visual Studio 2022 with the Desktop development with C++ workload, a Windows SDK, CMake 3.22 or newer, Git, and vcpkg.

## Set up vcpkg

Use a current bootstrapped vcpkg checkout. Local builds intentionally do not force the repository to contain a particular historical vcpkg commit, so existing developer installations continue to work.

```powershell
git clone https://github.com/microsoft/vcpkg.git "$HOME\src\vcpkg"
& "$HOME\src\vcpkg\bootstrap-vcpkg.bat" -disableMetrics
$env:VCPKG_ROOT = "$HOME\src\vcpkg"
```

CI checks out the official `2026.03.18` vcpkg release before configuring the project.

## Configure, build, and test

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

The normal test suite is offline and deterministic. Tracker parsing and worker behavior remain covered without making live third-party requests during tests.

### Optional live network tests

The `CurlRealNetworkTest` cases contact `httpbin.org` and are skipped by default. Run them explicitly when validating the HTTP stack:

```powershell
$env:OMNISTATS_RUN_NETWORK_TESTS = "1"
ctest --preset windows-release --output-on-failure
Remove-Item Env:\OMNISTATS_RUN_NETWORK_TESTS
```

## Useful CMake options

| Option | Default | Purpose |
| --- | --- | --- |
| `BUILD_TESTING` | `ON` | Builds the unit and integration test executable. |
| `OMNISTATS_BUILD_BENCHMARKS` | `OFF` | Builds the Google Benchmark executable. |
| `OMNISTATS_BUILD_FUZZER` | `OFF` | Builds the fuzzing executable. |
| `OMNISTATS_BUILD_UPDATER` | `ON` | Builds `OmniStatsUpdater.exe`. |
| `OMNISTATS_BUILD_INSTALLER` | `OFF` | Deprecated compatibility switch. Enabling it fails with instructions to use the WiX MSI. |
| `OMNISTATS_ENABLE_LOW_LEVEL_HOOK` | `OFF` | Enables the retired low-level keyboard-hook path for local testing only. |
| `ENABLE_SANITIZERS` | `OFF` | Enables supported sanitizer instrumentation. |
| `ENABLE_TSAN` | `OFF` | Enables ThreadSanitizer on supported non-MSVC toolchains. |
| `ENABLE_COVERAGE` | `OFF` | Enables coverage instrumentation on supported non-MSVC toolchains. |

The supported installer definition is under `installer/wix`. Release binaries should be built and tested from a clean commit before packaging.
