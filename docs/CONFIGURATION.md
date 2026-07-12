# Configuration

OmniStats stores runtime configuration in `%APPDATA%\omnistats\config.json`. Prefer the Settings UI for normal changes.

## Rocket League Stats API

Live data comes from Rocket League's local Stats API. OmniStats defaults to loopback only:

```text
Host: 127.0.0.1
Port: 49123
```

The relevant Rocket League values are:

```ini
[StatsAPI]
PacketSendRate=30
Port=49123
```

OmniStats can verify and repair this configuration. Restart Rocket League after changing it.

## Required startup diagnostics

Startup diagnostics are sent after privacy acceptance and are not configurable. They contain the app version, a pseudonymous installation ID, and feature-toggle status. Match data and player names are not included. See [PRIVACY.md](PRIVACY.md).

## Optional integrations

- **Discord Rich Presence** communicates with the local Discord client.
- **Ballchasing replay uploads** require a user-supplied token protected with Windows DPAPI.
- **Crash reports** upload pending minidumps only when enabled.
- **Update checks** use the external updater. Downloaded executables must match the published SHA-256 value before replacement.

**Tracker Network rank lookup** is controlled by `enable_mmr_tracking`. When enabled, OmniStats sends lobby player names and platform identifiers to Tracker Network to retrieve public rank information. It remains disabled by default.

Do not commit real configuration, databases, history exports, logs, crash dumps, or tokens.
