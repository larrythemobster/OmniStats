# Privacy

OmniStats is a local-first Windows companion application for Rocket League. Telemetry is read from Rocket League's local loopback socket, and match history remains stored on your computer in `%APPDATA%\omnistats`.

## Network data flows

| Feature | Destination | Transmitted data | Default |
|---|---|---|---|
| **Startup diagnostics** | `api.omnistats.org` | App version, pseudonymous installation ID, and integration toggle states. No match data or player names. | Required (startup) |
| **Update checks** | `omnistats.org` | App version. Receives version metadata and announcements. Binaries verified via SHA-256 before staging. | Always on |
| **OmniStats rank lookup** | `api.omnistats.org` | Up to 8 player platform and account IDs per batch, sent with the signed-in access token or an API key. | Opt-in |
| **Tracker rank lookup** | Tracker Network | Lobby player public display names and platform identifiers. | Opt-in |
| **Replay uploads** | `ballchasing.com` | Rocket League `.replay` files containing match participant identities and gameplay data. | Opt-in |
| **Crash reports** | `api.omnistats.org` | Windows minidump, app version, and installation ID. May contain process memory. | Opt-in |
| **Discord Rich Presence** | Discord (Local IPC) | Current match score and playlist status sent to the local Discord client. | Opt-in |

The installation ID is a persistent pseudonymous identifier used to correlate diagnostic pings. It is not linked to personal accounts.

## Local data storage

All match and user data is stored locally in `%APPDATA%\omnistats\omnistats.db` (SQLite 3 in WAL mode):

- **Match history:** Timestamps, arenas, scores, match outcomes, player platform IDs, names, MMR, and scoreboard statistics.
- **Player notes:** Private scouting notes keyed by normalized platform account identity.
- **Tracked accounts:** Curated list of local and alt accounts with nicknames and cached playlist ranks.
- **Replay queue:** Local replay paths, upload status, and Ballchasing URLs.
- **Configuration:** Stored in `config.json`. Account signing keys and tokens are encrypted using Windows DPAPI (`CryptProtectData`).

OmniStats does not upload your local database. Settings provides controls to export data (`%APPDATA%\omnistats\exports`), merge external databases, or delete history.

## Diagnostic logs and support bundles

- **Local logs:** Rotated on application startup. Up to 5 historical sessions (`omnistats_log.1.txt` through `omnistats_log.5.txt`) are retained. Player IDs, tokens, and credentials are redacted by default.
- **Support bundles:** Created only upon explicit user request via **Settings > Troubleshooting**. Bundles contain sanitized logs, configuration with secrets stripped, database row counts (no player data), and system environment details. Bundles are never transmitted automatically.

## Contact

For privacy questions or data deletion requests regarding optional accounts, visit the [OmniStats Discord](https://discord.gg/4KBW35ApvF) or see [https://omnistats.org/privacy](https://omnistats.org/privacy).
