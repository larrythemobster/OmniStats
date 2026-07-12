# Troubleshooting

## Windows says “Unknown publisher” or shows SmartScreen

Current OmniStats releases may be unsigned because commercial code signing has a recurring cost. Download only from the official OmniStats website or official release page, compare the published SHA-256 value when available, and do not use mirrors or re-uploaded installers.

## An update is rejected

The updater rejects files whose SHA-256 value does not match the published checksum. Reinstall from the official MSI rather than bypassing the check.

## MMR tracking is unavailable

Confirm **Enable MMR Tracking (Tracker Network)** is enabled. Run the updater's repair mode if `libcurl-impersonate.dll` or `zlib.dll` is missing. The rank service is third-party and can temporarily rate limit requests or change behavior without notice; local match and session tracking should continue normally.

## Diagnostics do not send

Startup diagnostics are required after privacy acceptance and are sent once when OmniStats starts. Crash-report uploads remain optional. If startup diagnostics fail because the service is unavailable, OmniStats logs the failure and continues running.

## Build problems

Use a current bootstrapped vcpkg checkout, Visual Studio 2022, the x64 static triplet, and the commands in [BUILDING.md](BUILDING.md). Delete the build directory before retrying after toolchain or manifest changes.
