# Privacy

OmniStats is a local Windows companion. It reads Rocket League telemetry from the loopback Stats API and stores configuration and history under `%APPDATA%\omnistats`.

## Required startup diagnostics

After the user accepts the Privacy Policy and Terms of Use, OmniStats sends one startup diagnostic request to `api.omnistats.org/api/v1/telemetry`. The request contains the app version, a persistent pseudonymous installation ID, and the enabled/disabled status of Ballchasing uploads, Discord Rich Presence, update checks, and automatic updates. Match data and player names are not included.

The startup diagnostic is required to use OmniStats and cannot be disabled in Settings. Users who do not agree can exit from the privacy notice before the application starts.

## Optional data flows

| Feature | Destination and fields | Default |
| --- | --- | --- |
| Tracker rank lookup | Tracker Network receives the lobby player's public name and platform identifier needed to request public Rocket League rank information. | Off |
| Discord Rich Presence | Match/session presence is sent to the user's local Discord client. | Off |
| Ballchasing replay upload | Selected replay files are uploaded to ballchasing.com using the user's token. | Off |
| Crash reports | A pending Windows minidump, app version, and pseudonymous installation ID are uploaded to `api.omnistats.org/api/v1/crash`. Minidumps may contain sensitive process memory. | Off |
| Update checks | Version metadata and release files are requested from `omnistats.org`. Downloads are checked against published SHA-256 values. | Off |
| Player profile links | The user's default browser opens the selected public profile page. | User action |

The installation ID is persistent and pseudonymous, not anonymous. It is created during startup after privacy acceptance and is stored in local configuration/database state.

Tracker Network, Discord, Ballchasing, GitHub, and other third-party services apply their own privacy practices to requests sent to them. The Tracker integration may stop working if its service or access requirements change.

## Local controls

The Settings UI can disable optional services and delete saved match history. Deleting `%APPDATA%\omnistats` while OmniStats is closed removes local configuration, history, logs, crash dumps, and the installation ID. Back up anything you want to keep first.

Normal HTTPS infrastructure may process the connecting IP address. The official website must publish the current server retention period and deletion-request process; see [WEBSITE_RELEASE_CHECKLIST.md](WEBSITE_RELEASE_CHECKLIST.md).

Treat configuration, replay tokens, history, logs, and minidumps as private. Redact them before sharing diagnostics.
