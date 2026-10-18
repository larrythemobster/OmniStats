# Privacy

OmniStats is a local-first Windows companion application for Rocket League. Telemetry is read from Rocket League's local loopback socket, and match history remains stored on your computer in `%APPDATA%\omnistats`.

## Network data flows

| Feature | Destination | Transmitted data | Default |
|---|---|---|---|
| **Startup diagnostics** | OmniStats service | App version, pseudonymous installation ID, and integration toggle states. Official builds only. No match data or player names. | Required in official builds (startup); absent in source builds |
| **Update checks** | `omnistats.org` | App version. Receives version metadata and announcements. The MSI is checked against its SHA-256 checksum and pinned Authenticode signature before installation. | Always on |
| **OmniStats rank lookup** | OmniStats service | Up to 8 player platform and account IDs per batch, sent with the signed-in access token. Official builds only. | Opt-in |
| **Tournament schedules** | OmniStats service | Region code, device ID, and signed-in access token. Local ping-cache and match-log contents are not uploaded. Official builds only. | Enabled while signed in; can be disabled |
| **Tracker rank lookup** | Tracker Network | Lobby player public display names and platform identifiers. | Opt-in |
| **Replay uploads** | `ballchasing.com` | Rocket League `.replay` files containing match participant identities and gameplay data. | Opt-in |
| **Crash reports** | OmniStats service | Windows minidump, app version, and installation ID. May contain process memory. Official builds only. | Opt-in in official builds; absent in source builds |
| **Discord Rich Presence** | Discord (Local IPC) | Current match score and playlist status sent to the local Discord client. | Opt-in |

Discord Rich Presence uses the Social SDK's local desktop IPC mode. OmniStats does not request Discord OAuth authorization or store Discord access tokens for this integration. The setting remains optional; disabling it clears the application's presence.

The installation ID is a persistent pseudonymous identifier used to correlate diagnostic pings. It is not linked to personal accounts. The server keeps each ID and its first recorded startup date for long-term installation counts, even after individual startup records expire. This counts installations, not people; clearing local identity data can generate a new ID.

## Local data storage

All match and user data is stored locally in `%APPDATA%\omnistats\omnistats.db` (SQLite 3 in WAL mode):

- **Match history:** Timestamps, arenas, scores, match outcomes, player platform IDs, names, MMR, and scoreboard statistics.
- **Match analytics:** Per-match gameplay data from the Stats API: ball-hit locations and speeds, goal locations, kickoff and possession summaries, stat-feed events per player, and your own car's boost and movement aggregates.
- **Player notes:** Private scouting notes keyed by normalized platform account identity.
- **Tracked accounts:** Curated list of local and alt accounts with nicknames and cached playlist ranks.
- **Replay queue:** Local replay paths, upload status, and Ballchasing URLs.
- **Configuration:** Stored in `config.json`. Account signing keys and tokens are encrypted using Windows DPAPI (`CryptProtectData`).

OmniStats does not upload your local database. Settings provides controls to export data (`%APPDATA%\omnistats\exports`), merge external databases, or delete history.

Automatic tournament-region detection reads Rocket League's `RegionCache.cache` and, if necessary, recent reservation-region fields in `Launch.log` locally. Only the selected region code is sent to the schedule API.

## Diagnostic logs and support bundles

- **Local logs:** Rotated on application startup. Up to 5 historical sessions (`omnistats_log.1.txt` through `omnistats_log.5.txt`) are retained. Player IDs, tokens, and credentials are redacted by default.
- **Support bundles:** Created only upon explicit user request via **Settings > Troubleshooting**. Bundles contain sanitized logs, configuration with secrets stripped, database row counts (no player data), and system environment details. Bundles are never transmitted automatically.

## Contact

For privacy questions or data deletion requests regarding optional accounts, visit the [OmniStats Discord](https://discord.gg/4KBW35ApvF) or see [https://omnistats.org/privacy](https://omnistats.org/privacy).
