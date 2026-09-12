# Architecture

OmniStats is a native Windows companion application. It does not inject into Rocket League. The main process reads the local Stats API, reduces incoming events into a synchronized session model, stores selected history locally, and presents the overlay/dashboard through a native Win32 + DirectX 11 + RmlUi stack.

## Runtime flow

1. `TelemetryParser` connects to the configured loopback Stats API endpoint.
2. `StatsClient` passes complete events to `TelemetryReducer`.
3. `TelemetryReducer` updates `SessionState` and emits explicit side effects.
4. `SideEffectExecutor` performs database writes, replay actions, and optional integrations outside the parser path.
5. `MMRFetcher` performs optional Tracker Network rank requests on its own bounded worker queue.
6. `DatabaseManager` serializes SQLite work and publishes completed queries back to the session model.
7. `Overlay` owns the native Win32 window, tray, DirectX 11 device/swap chain, focus/click-through behavior, and the frame loop.
8. `RmlUiController` snapshots only the shared state needed by the presentation layer, updates persistent RmlUi documents when relevant state changes, and binds UI actions back to the existing configuration/database/application systems.
9. `RmlRenderInterfaceD3D11` translates RmlUi geometry, textures, clipping, and transforms onto OmniStats' existing D3D11 device/context.

## UI architecture

The UI remains native C++ and keeps the existing DirectX 11 renderer/window infrastructure. There is no browser runtime and no JavaScript UI layer.

- `src/ui/Overlay.*`: host lifecycle, frame pacing, focus gating, transparent/click-through overlay behavior, second-monitor desktop-window behavior, DPI/resizing, device-loss recovery, tray integration, and Win32 message routing.
- `src/ui/D3D11Device.*`: DirectX 11 device, context, swap chain, render target, resize, and Present handling.
- `src/ui/OverlayWindow.*`: native window creation, persisted bounds, monitor placement, and style changes.
- `src/ui/rml/RmlUiController.*`: persistent RmlUi document/controller, application-state snapshots, settings bindings, dashboard editing, overlay editing, graph markup, notifications, and native actions.
- `src/ui/rml/RmlRenderInterfaceD3D11.*`: compact native RmlUi 6 render interface using the existing D3D11 device/context. It supports compiled geometry, per-draw textures, premultiplied-alpha blending, scissoring, and transforms without the legacy compatibility adapter.
- `src/ui/rml/RmlSystemInterfaceWin32.*`: timers, cursors, clipboard, path handling, and IME positioning.
- `src/ui/rml/RmlInputWin32.*`: Win32 mouse, wheel, keyboard, and Unicode text input translation.
- `src/ui/rml/RmlFileInterface.*`: embedded RML/RCSS resources with a disk fallback for unpacked runs.
- `resources/rml/`: the reusable RML/RCSS design system and persistent document shell.
- `resources/fonts/`: the bundled Inter (UI), JetBrains Mono (numeric), and Russo One (display) faces. They are embedded as RCDATA and registered by `RmlUiController::LoadBundledFonts` with an explicit family and weight; Segoe UI and MS Gothic are registered as fallback faces for glyphs the bundled faces lack.
- `src/core/DashboardLayoutConfig.*` and `src/core/OverlayLayoutConfig.*`: renderer-independent persisted layout formats. Existing user layouts remain the source of truth.

Core telemetry, storage, networking, updater, replay, and integration code does not depend on RmlUi. The UI consumes snapshots and calls the existing APIs rather than duplicating application logic.

## Ownership boundaries

- `src/core`: configuration, session state, pure reduction logic, input, storage paths, persisted layout configuration, and shared value types.
- `src/network`: local telemetry input, required startup diagnostics, Tracker rank lookup, updater support, and other external services.
- `src/database`: SQLite ownership and asynchronous persistence.
- `src/ui`: Win32/D3D11 host plus RmlUi presentation and native UI bindings.
- `src/updater`: the separate updater process, dependency repair, download, checksum verification, and process replacement.
- `installer/wix`: the supported public installer.

## UI update model

RmlUi documents are kept alive instead of being recreated each frame. `RmlUiController` compares session/history versions and relevant UI/config state, refreshes only the affected roots, and caches RmlUi/D3D resources through the normal document/render-interface lifetime. Fast telemetry remains in `SessionState`; it is not serialized into an intermediate browser-style state blob.

The editable dashboard and transparent overlay keep separate persisted layouts. Dashboard drag/drop updates `DashboardLayoutConfig`; overlay move/resize/widget docking updates `OverlayLayoutConfig`. Settings controls write through `Config::Update` and continue using the existing persistence and side-effect paths.

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
