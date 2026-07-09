# Troubleshooting

## No live match data

1. In Rocket League, verify that the local Stats API is enabled with `PacketSendRate=30` and `Port=49123`.
2. Confirm OmniStats is configured for `127.0.0.1:49123` unless you intentionally changed the local endpoint.
3. Restart Rocket League after changing `DefaultStatsAPI.ini`.
4. Use the startup repair prompt or the Settings UI to recheck the configuration.

## MMR data is missing

Tracker.gg lookups are optional. Enable MMR tracking in Settings, verify internet access, and expect external service rate limits or schema changes to affect the result. Do not retry aggressively.

## Update or installer failure

Update downloads are verified with SHA-256. If an update fails, keep automatic updates disabled until a later retry or reinstall from an official OmniStats release source. Do not substitute binaries from mirrors or unofficial builds.

## Build configuration fails

Verify that `VCPKG_ROOT` or `VCPKG_TOOLCHAIN_FILE` points to a bootstrapped vcpkg installation, then rerun CMake from a clean `out/` directory. See [BUILDING.md](BUILDING.md).

## Tests fail offline

Most tests should be local. The curl-impersonate tests intentionally perform network requests; run the reported failing test by name or exclude that test group for an offline check.

## Reporting a problem

Use the repository issue forms for reproducible bugs and feature requests. Remove secrets, local configuration, logs, minidumps, databases, player data, and machine paths before attaching anything. Follow [SECURITY.md](../SECURITY.md) instead of opening a public issue for a vulnerability or privacy-sensitive report.
