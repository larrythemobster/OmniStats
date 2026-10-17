# Building

OmniStats targets Windows 10 or newer and requires Visual Studio 2022 with the Desktop development with C++ workload, a Windows SDK, CMake 3.22 or newer, Git, vcpkg, and the standalone Discord Social SDK archive described below.

There's no native Linux build. To run OmniStats on Linux, see [LINUX.md](LINUX.md).

## Set up vcpkg

Use a current bootstrapped vcpkg checkout. Local builds intentionally do not force the repository to contain a particular historical vcpkg commit, so existing developer installations continue to work.

```powershell
git clone https://github.com/microsoft/vcpkg.git "$HOME\src\vcpkg"
& "$HOME\src\vcpkg\bootstrap-vcpkg.bat" -disableMetrics
$env:VCPKG_ROOT = "$HOME\src\vcpkg"
```

CI checks out the official `2026.03.18` vcpkg release before configuring the project.

## Discord Social SDK

The SDK files can be placed under `Client/third_party/discord-social-sdk/discord_social_sdk`, or provided as `DiscordSocialSdk-1.10.19337.zip`. CMake looks in `third_party/discord-social-sdk` first, then `%USERPROFILE%\Downloads` by default. For another location, set the `OMNISTATS_DISCORD_SDK_ARCHIVE` environment variable or pass `-DOMNISTATS_DISCORD_SDK_ARCHIVE=<path>` when configuring. If using the archive, CMake verifies SHA-256 `d784097504685953849cc2842561d8a8013326fe0b8df65a1de63ddefde62045` before extracting the Windows x64 release headers, library, DLL, and notices.

The application uses desktop Rich Presence through local Discord IPC without calling `Client::Connect`, requesting OAuth scopes, or handling Discord tokens. The SDK runtime `discord_partner_sdk.dll` is copied beside the application and included in the MSI. Voice libraries and noise-suppression models are not used or packaged.

GitHub Actions does not build the client because the SDK cannot be downloaded there. Build and test locally; `deploy.ps1` also runs the full suite before every release. Do not commit the SDK archive or credentials to the repository.

## Configure, build, and test

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release --output-on-failure
```

The normal test suite is offline and deterministic. Tracker parsing and worker behavior remain covered without making live third-party requests during tests.

### Optional live network tests

The `CurlRealNetworkTest` cases contact `httpbin.org` and are skipped by default. Run them explicitly when validating the HTTP stack:

```powershell
$env:OMNISTATS_RUN_NETWORK_TESTS = "1"
ctest --preset windows-release --output-on-failure
Remove-Item Env:\OMNISTATS_RUN_NETWORK_TESTS
```

## Editing the UI without rebuilding

Release builds embed `resources/rml/*.rml` and `omnistats.rcss`. To iterate on them, point OmniStats at a folder of editable copies and press **Ctrl+Shift+R** in the OmniStats window to reload:

```powershell
$env:OMNISTATS_RML_DIR = "$PWD\resources\rml"
.\out\windows-release\Release\OmniStats.exe
```

Debug builds read from the source tree automatically. The hotkey reloads the stylesheet, reapplies the theme colors, and reloads the Insights and first-run documents. Widget markup built in C++ still needs a rebuild.

## Useful CMake options

| Option | Default | Purpose |
| --- | --- | --- |
| `BUILD_TESTING` | `ON` | Builds the unit and integration test executable. |
| `OMNISTATS_BUILD_BENCHMARKS` | `OFF` | Builds the Google Benchmark executable. |
| `OMNISTATS_BUILD_FUZZER` | `OFF` | Builds the fuzzing executable. Requires the clang-cl toolset on Windows (`-T ClangCL`); plain `cl.exe` cannot link libFuzzer. |
| `OMNISTATS_BUILD_UPDATER` | `ON` | Builds `OmniStatsUpdater.exe`. |
| `OMNISTATS_BUILD_INSTALLER` | `OFF` | Deprecated compatibility switch. Enabling it fails with instructions to use the WiX MSI. |
| `OMNISTATS_ENABLE_LOW_LEVEL_HOOK` | `OFF` | Enables the retired low-level keyboard-hook path for local testing only. |
| `CMAKE_DISABLE_PRECOMPILE_HEADERS` | `OFF` | Disables the RmlUi PCH. RmlUi sources explicitly include the shared header, so both configurations are supported. |
| `ENABLE_SANITIZERS` | `OFF` | Enables supported sanitizer instrumentation. |
| `ENABLE_TSAN` | `OFF` | Enables ThreadSanitizer on supported non-MSVC toolchains. |
| `ENABLE_COVERAGE` | `OFF` | Enables coverage instrumentation on supported non-MSVC toolchains. |

The supported installer definition is under `installer/wix`. Release binaries should be built and tested from a clean commit before packaging.
