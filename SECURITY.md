# Security Policy

Please do not open a public issue for vulnerabilities, exposed credentials, private logs, crash dumps, or privacy-sensitive bugs.

Use GitHub's private vulnerability reporting feature if it is enabled for this repository. Otherwise, contact the project maintainer privately through the official support channel.

## Before publishing a release

Check that the commit, tag, and release archive do not include:

- Deploy keys or private SSH keys.
- API tokens or test secrets.
- Local `.env` files.
- Local `config.json` files.
- Logs.
- Crash dumps or minidumps.
- SQLite databases.
- `matches.jsonl` or `mmr_history.jsonl` files from real users.
- Personal machine paths.
- Server IPs, usernames, or deployment paths.

## GitHub security settings

Enable GitHub secret scanning and push protection in the repository settings. Keep the included Gitleaks workflow enabled so pull requests receive an additional scan with read-only repository access.
