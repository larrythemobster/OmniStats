<h1><img src="resources/Logo.png" alt="" width="36" height="36" align="top"> OmniStats</h1>

[![Build](https://img.shields.io/github/actions/workflow/status/larrythemobster/OmniStats/build.yml?branch=main&label=build)](https://github.com/larrythemobster/OmniStats/actions/workflows/build.yml)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)](CMakeLists.txt)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%2B-0078D6)](docs/BUILDING.md)
[![License](https://img.shields.io/badge/license-PolyForm%20Internal%20Use-555)](LICENSE)
[![Discord](https://img.shields.io/badge/Discord-join-5865F2?logo=discord&logoColor=white)](https://discord.gg/4KBW35ApvF)

`OmniStats` is a native Windows companion for Rocket League. It connects to the game's loopback Stats API over TCP, tracks live match and session stats, and renders a click-through overlay or second-monitor dashboard using Direct3D 11 and RmlUi. Match history, notes, and tracked accounts stay local in SQLite.

**[Architecture](docs/ARCHITECTURE.md) · [Building](docs/BUILDING.md) · [Privacy](docs/PRIVACY.md) · [Overlay rules](docs/OVERLAY.md) · [Contributing](CONTRIBUTING.md) · [Security](SECURITY.md)**

![OmniStats second-monitor dashboard](docs/screenshots/dashboard.png)

## Features

- **Lobby ranks.** Rank, division, and MMR across 1v1, 2v2, 3v3, casual, tournaments, and extra modes.
- **Live telemetry.** Real-time saves, shots, assists, demos, and goal and ball speeds for the lobby and for you.
- **Rank progress and MMR graph.** Visual division bands, pre-session baseline, and match deltas. F9 toggles between session and lifetime history.
- **Player scouting.** Head-to-head records (with and vs) computed from local history, plus private player notes.
- **Session insights.** Win rates by teammate, time-of-day trends, and session recap cards exportable as PNG.
- **Account tracking.** Track MMR across all playlists for your main and alts without Rocket League running.
- **Modular layout.** Draggable, dockable cards with per-card visibility rules, or a separate second-monitor window.
- **Integrations.** Optional automatic replay saving, Ballchasing.com uploads, and Discord Rich Presence.

## Architecture and design

| Subsystem | Implementation | Design notes |
|---|---|---|
| **Game link** | Loopback TCP (Asio) | Connects to Rocket League's built-in Stats API on `127.0.0.1:49123`. Zero DLL injection, zero game memory reads. |
| **Renderer** | Direct3D 11 + RmlUi | Hardware-accelerated HTML/CSS-style DOM. Live telemetry updates text nodes directly via data bindings without rebuilding widgets. |
| **Storage** | SQLite 3 (WAL mode) | Local persistence for matches, player stats, notes, and tracked accounts. Uses versioned schema migrations. |
| **Rank lookup** | OmniStats API + Tracker Network | Device sign-in proves possession of an Ed25519 device key (libsodium); rank requests then carry the access token, or an API key. Secrets are protected with Windows DPAPI. Optional Tracker Network support. |
| **Packaging** | WiX Toolset v4 (MSI) | Authenticode-signed installer. The updater validates SHA-256 checksums over HTTPS before staging updates. |
| **Test suite** | GoogleTest | Unit and integration tests, including offscreen headless UI tests rendered through DirectX 11 WARP. |

## Supported surfaces

| Surface | Display mode | Description |
|---|---|---|
| **In-game overlay** | Borderless windowed | Click-through HUD rendered over Rocket League. Uses per-container visibility triggers (scoreboard hold, kickoff, goal, podium). |
| **Second monitor** | Exclusive fullscreen or borderless | Separate window with editable, dockable cards. Rocket League stays focused on your primary display. |

## Quick start

Download the signed installer from [omnistats.org](https://omnistats.org/) or build from source:

```powershell
$env:VCPKG_ROOT = 'C:\src\vcpkg'
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

First-run setup checks Rocket League's Stats API configuration, prompts for display mode, and walks through optional integrations. See [docs/BUILDING.md](docs/BUILDING.md) for full prerequisites and toolchain requirements.

## Screenshots

| In-game overlay | Match history | Accounts |
|:---:|:---:|:---:|
| ![Overlay roster with rank progress](docs/screenshots/overlay.png) | ![Match history window](docs/screenshots/history.png) | ![Tracked accounts window](docs/screenshots/accounts.png) |

## Documentation index

| Guide | Description |
|---|---|
| [Architecture](docs/ARCHITECTURE.md) | Data pipeline, reducer state machine, and RmlUi DOM update model |
| [Building](docs/BUILDING.md) | Visual Studio 2022, MSVC, vcpkg, and CMake configuration |
| [Overlay rules](docs/OVERLAY.md) | Container visibility rules, event timers, and layout editing |
| [Privacy](docs/PRIVACY.md) | Network data flows, local storage, diagnostic pings, and retention |
| [Contributing](CONTRIBUTING.md) | Code style, commit conventions, and testing requirements |
| [Security](SECURITY.md) | Vulnerability disclosure policy and release integrity verification |

## License and trademarks

Source available under the [PolyForm Internal Use License 1.0.0](LICENSE). Contributions accepted under [CONTRIBUTION_EXCEPTION.md](CONTRIBUTION_EXCEPTION.md). Redistribution, public mirrors, and competing distributions require written permission.

Rocket League, Psyonix, Epic Games, Tracker Network, Discord, and Ballchasing are trademarks of their respective owners. OmniStats is an independent project and is not affiliated with or endorsed by them.
