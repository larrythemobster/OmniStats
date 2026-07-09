# Configuration

OmniStats stores its runtime configuration in `%APPDATA%\omnistats\config.json`. The file is created and maintained by the application; prefer the Settings UI for normal changes.

## Rocket League Stats API

Live data comes from Rocket League's local Stats API. OmniStats defaults to:

```text
Host: 127.0.0.1
Port: 49123
```

The relevant Rocket League configuration values are:

```ini
[StatsAPI]
PacketSendRate=30
Port=49123
```

OmniStats checks this setup on startup when the setting is enabled and can offer to repair the configuration. Restart Rocket League after changing its Stats API configuration.

## Optional integrations

All optional integrations can remain disabled:

- **Tracker.gg MMR tracking** requests rank/MMR data for observed players.
- **Discord Rich Presence** updates the user's local Discord client.
- **Ballchasing replay uploads** require a user-supplied API token. OmniStats protects the stored token with Windows DPAPI.
- **Crash reports** upload pending minidumps only when enabled.
- **Update checks** can check for or automatically install official updates.

Use the Settings UI to review each option and its corresponding privacy notice. Do not commit a real `config.json`, database, history export, or replay-upload token.

For data handling details, see [PRIVACY.md](PRIVACY.md). For connection problems, see [TROUBLESHOOTING.md](TROUBLESHOOTING.md).
