# Privacy

OmniStats is a local Windows companion app. It reads Rocket League telemetry from the local Stats API and stores match/session data in `%APPDATA%\omnistats`.

## Required startup diagnostic

After accepting the in-app privacy notice and terms, OmniStats sends its app version, an anonymous client UUID, and feature-toggle status to the official OmniStats telemetry service. The UUID is generated locally and stored in the local configuration.

## Optional data flows

| Feature | Data flow | Default |
| --- | --- | --- |
| Tracker.gg MMR tracking | Player name and platform identifier are sent to Tracker.gg for rank/MMR lookup. | Off |
| Discord Rich Presence | Match/session presence is sent to the user's local Discord client. | Off |
| Ballchasing replay upload | Replay files are uploaded to ballchasing.com using the user's token. | Off |
| Crash reports | Pending Windows minidumps are uploaded to the official crash endpoint on a later startup. Minidumps can contain sensitive process memory. | Off |
| Update checks | Version metadata and verified update files are requested from official OmniStats release services. | Off |

The replay-upload token is protected with Windows DPAPI in the local configuration. Treat all local configuration, history, logs, and crash dumps as private data.

## Local data controls

The Settings UI can disable optional integrations and delete saved match history. Before sharing diagnostics, remove player identifiers, tokens, logs, crash dumps, and personal filesystem paths.

For vulnerability reporting, see [SECURITY.md](../SECURITY.md).
