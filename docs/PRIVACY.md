# Privacy

OmniStats is a local Windows companion. It reads Rocket League telemetry from the loopback Stats API and stores configuration and history under `%APPDATA%\omnistats`.

## Required startup diagnostics

After the user accepts the Privacy Policy and Terms of Use, OmniStats sends one startup diagnostic request to `api.omnistats.org/api/v1/telemetry`. The request contains the app version, a persistent pseudonymous installation ID, and the enabled/disabled status of Ballchasing uploads, Discord Rich Presence, and automatic updates, plus the always-enabled update-check status. Match data and player names are not included.

The startup diagnostic is required to use OmniStats and cannot be disabled in Settings. Users who do not agree can exit from the privacy notice before the application starts.

## Network data flows

| Feature | Destination and fields | Default |
| --- | --- | --- |
| OmniStats account sign-in and rank lookup | Signing in sends the device public key, derived device ID, computer name, `windows` platform string, and app version to `api.omnistats.org/v1/auth/device/*`, then opens the approval page in the default browser. When signed in and enabled, rank lookups send lobby platform and account IDs with a short-lived device bearer token and device ID to `api.omnistats.org/v1/ranks`. | Off until signed in |
| Tracker rank lookup | Tracker Network receives the lobby player's public name and platform identifier needed to request public Rocket League rank information. | Off |
| Discord Rich Presence | Match/session presence is sent to the user's local Discord client. | Off |
| Ballchasing replay upload | Selected replay files are uploaded to ballchasing.com using the user's token. | Off |
| Crash reports | A pending Windows minidump, app version, and pseudonymous installation ID are uploaded to `api.omnistats.org/api/v1/crash`. Minidumps may contain sensitive process memory. | Off |
| Update checks | Version metadata is requested from `omnistats.org` at startup, periodically while OmniStats is running, and when Settings is opened. Release files are only downloaded when an update is installed and are checked against published SHA-256 values. | Always on |
| Service announcements | `api.omnistats.org/api/v1/client/config` is requested after privacy acceptance and about every 15 minutes with only the app version. It returns service announcements and the minimum recommended version, which shows an update reminder on older versions. The last response is cached in `%APPDATA%\omnistats\remote_config_cache.json`, and dismissed announcements in `remote_config_dismissed.json`. | Always on |
| Player profile links | The user's default browser opens the selected public profile page. | User action |

The installation ID is persistent and pseudonymous, not anonymous. It is created during startup after privacy acceptance and is stored in local configuration/database state.

Tracker Network, Discord, Ballchasing, GitHub, and other third-party services apply their own privacy practices to requests sent to them. The Tracker integration may stop working if its service or access requirements change.

## Locally stored match data

OmniStats stores match and session history on your PC in `%APPDATA%\omnistats\omnistats.db` (`Matches`, `MatchPlayers`, `MatchPlayerStats`, `MatchLocalStats`, and `Sessions`) and `%APPDATA%\omnistats\matches.jsonl`:

- `Matches` stores the timestamp, arena, team scores, win/loss outcome, match GUID, playlist ID, gamemode, player count, and linked session ID.
- `MatchPlayers` stores each participant's platform primary ID, display name, team number, opponent flag, and MMR at match time.
- `MatchPlayerStats` stores each participant's scoreboard score, goals, assists, saves, shots, demolitions, ball touches, car touches, max goal speed, and fastest goal time.
- `MatchLocalStats` stores the local player's boost collected, times demolished, crossbars hit, hardest crossbar impact, max ball speed, own goals, match duration, overtime duration, and stats schema version.
- `Sessions` stores the local account ID, start and end timestamps, win/loss counts, per-playlist MMR change JSON, session totals JSON, and whether the row came from a live reset or historical backfill.

These tables remain on your machine and are never uploaded by OmniStats. **Export Local Data** writes them to `%APPDATA%\omnistats\exports`, and **Delete History & Identity** clears all five tables along with the local JSONL files.

## Local controls

In Settings, **Integrations** controls OmniStats account sign-in, the OmniStats rank API toggle, and optional third-party services. Signing in generates an Ed25519 device keypair and stores the private key and rotating refresh token in `config.json` encrypted with Windows DPAPI `CryptProtectData` bound to the current Windows user, alongside the signed-in display name and derived device public ID. Signing out revokes the device session and clears the stored refresh token and display name. **Data** contains crash report sharing, history exports, and **Delete History & Identity**. Deletion requires confirmation and keeps settings and tokens. Ballchasing tokens are hidden unless **Show** is selected.

Deleting `%APPDATA%\omnistats` while OmniStats is closed removes local configuration, history, logs, crash dumps, and the installation ID. Back up anything you want to keep first.

Normal HTTPS infrastructure may process the connecting IP address. Current retention details and the contact method for privacy requests are published at <https://omnistats.org/privacy>.

Treat configuration, replay tokens, history, logs, and minidumps as private. Redact them before sharing diagnostics.
