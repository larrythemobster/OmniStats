# Privacy

OmniStats is a local Windows companion. It reads Rocket League telemetry from the loopback Stats API and stores configuration and history under `%APPDATA%\omnistats`.

## Required startup diagnostics

After the user accepts the Privacy Policy and Terms of Use, OmniStats sends one startup diagnostic request to `api.omnistats.org/api/v1/telemetry`. The request contains the app version, a persistent pseudonymous installation ID, and the enabled/disabled status of Ballchasing uploads, Discord Rich Presence, and automatic updates, plus the always-enabled update-check status. Match data and player names are not included.

The startup diagnostic is required to use OmniStats and cannot be disabled in Settings. Users who do not agree can exit from the privacy notice before the application starts.

## Network data flows

| Feature | Destination and fields | Default |
| --- | --- | --- |
| Tracker rank lookup | Tracker Network receives the lobby player's public name and platform identifier needed to request public Rocket League rank information. | Off |
| Discord Rich Presence | Match/session presence is sent to the user's local Discord client. | Off |
| Ballchasing replay upload | Selected replay files are uploaded to ballchasing.com using the user's token. | Off |
| Crash reports | A pending Windows minidump, app version, and pseudonymous installation ID are uploaded to `api.omnistats.org/api/v1/crash`. Minidumps may contain sensitive process memory. | Off |
| Update checks | Version metadata is requested from `omnistats.org` at startup, periodically while OmniStats is running, and when Settings is opened. Release files are only downloaded when an update is installed and are checked against published SHA-256 values. | Always on |
| Player profile links | The user's default browser opens the selected public profile page. | User action |

The installation ID is persistent and pseudonymous, not anonymous. It is created during startup after privacy acceptance and is stored in local configuration/database state.

Tracker Network, Discord, Ballchasing, GitHub, and other third-party services apply their own privacy practices to requests sent to them. The Tracker integration may stop working if its service or access requirements change.

## Local controls

In Settings, **Replays & services** controls optional integrations. **Data & privacy** contains crash report sharing, history exports, and **Delete History & Identity**. Deletion requires confirmation and keeps settings and tokens. Ballchasing tokens are hidden unless **Show token** is selected.

Deleting `%APPDATA%\omnistats` while OmniStats is closed removes local configuration, history, logs, crash dumps, and the installation ID. Back up anything you want to keep first.

Normal HTTPS infrastructure may process the connecting IP address. Current retention details and the contact method for privacy requests are published at <https://omnistats.org/privacy>.

Treat configuration, replay tokens, history, logs, and minidumps as private. Redact them before sharing diagnostics.
