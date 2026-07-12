# Architecture

OmniStats is a native Windows companion application. It does not inject into Rocket League. The main process reads the local Stats API, reduces incoming events into a synchronized session model, stores selected history locally, and renders the overlay or dashboard.

## Runtime flow

1. `TelemetryParser` connects to the configured loopback Stats API endpoint.
2. `StatsClient` passes complete events to `TelemetryReducer`.
3. `TelemetryReducer` updates `SessionState` and emits explicit side effects.
4. `SideEffectExecutor` performs database writes, replay actions, and optional integrations outside the parser path.
5. `MMRFetcher` performs optional Tracker Network rank requests on its own bounded worker queue.
6. `DatabaseManager` serializes SQLite work and publishes completed queries back to the session model.
7. `Overlay` owns the Windows/D3D11/ImGui lifecycle and renders immutable snapshots of shared state.

## Ownership boundaries

- `src/core`: configuration, session state, pure reduction logic, input, storage paths, and shared value types.
- `src/network`: local telemetry input, required startup diagnostics, Tracker rank lookup, updater support, and other external services.
- `src/database`: SQLite ownership and asynchronous persistence.
- `src/ui`: Win32, D3D11, overlay layout, panels, and widgets.
- `src/updater`: the separate updater process, dependency repair, download, checksum verification, and process replacement.
- `installer/wix`: the supported public installer.

## External network requests

The core Rocket League telemetry connection is loopback-only. Startup diagnostics are required after privacy acceptance; the remaining internet-facing features are optional:

| Destination | Purpose | Trigger |
| --- | --- | --- |
| Tracker Network Rocket League profile service | Public rank and MMR lookup for lobby players | User enables MMR tracking |
| `api.omnistats.org/api/v1/telemetry` | App version, pseudonymous installation ID, and feature-toggle status | Every startup after privacy acceptance |
| `api.omnistats.org/api/v1/crash` | Native crash dump upload | User enables crash reports and a pending dump exists |
| `omnistats.org/version.txt` and release files | Update checks and updates | User enables update checks or starts an update |
| Discord local RPC | Rich Presence | User enables Discord Rich Presence |
| ballchasing.com | Replay upload | User supplies a token and enables uploads |
| GitHub curl-impersonate release archive | Repair the Tracker compatibility runtime | User runs updater repair and the runtime is missing or invalid |

The Tracker integration sends the platform identifier and public player name needed for rank lookup. It uses a compatibility runtime because the third-party service expects browser-compatible transport behavior. The integration is isolated behind an opt-in setting, rate limited, and expected to fail safely when the service changes.

## Update trust boundary

The updater downloads files over HTTPS and validates the application executable against its published SHA-256 value before replacement. The curl-impersonate repair archive and extracted DLL also use pinned SHA-256 values. This protects against transfer corruption and unexpected content when the HTTPS origin remains trusted, but it is not equivalent to paid Authenticode publisher identity.
