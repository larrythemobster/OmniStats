# Security Policy

OmniStats takes security and user privacy seriously. Please report suspected vulnerabilities privately so they can be investigated before details are made public.

## Reporting a vulnerability

Use GitHub private vulnerability reporting when it is available for this repository. Otherwise, use the security contact method published at <https://omnistats.org/>.

Please include:

- the affected OmniStats version;
- a clear description of the issue;
- steps to reproduce it;
- the potential impact; and
- relevant logs, screenshots, or proof-of-concept material.

Do not publish vulnerabilities, credentials, access tokens, personal information, crash dumps, or working exploits in a public GitHub issue.

## Supported versions

Security fixes are provided for the latest public release of OmniStats. Users running older versions may be asked to update or reinstall using the current official MSI before support can be provided.

Only downloads published through the official OmniStats website or this GitHub repository should be considered official releases.

## Release integrity

Official releases should:

- be built from reviewed source code;
- pass the Windows build and test workflow;
- be distributed over HTTPS;
- publish the MSI with its SHA-256 checksum; and
- clearly state whether the binaries are digitally signed or unsigned.

The updater checks the downloaded MSI against its published SHA-256 checksum and requires a valid Authenticode signature from the pinned OmniStats certificate before running Windows Installer. Unsigned builds may display an **Unknown publisher** or Microsoft Defender SmartScreen warning.

## External services

Required startup diagnostics are sent only after the user accepts the in-app privacy notice. The transmitted fields are documented in [docs/PRIVACY.md](docs/PRIVACY.md).

The following features are optional:

- OmniStats and Tracker Network rank lookup;
- Discord Rich Presence;
- Ballchasing replay uploads;
- automatic update installation; and
- crash-report uploads.

Update checks always run at startup, periodically and when Settings opens.

Changes to an external endpoint, transmitted field, stored identifier, consent flow, download process, or update behavior require security and privacy review before release.

## Sensitive data

Do not commit or publish:

- API keys, access tokens, or passwords;
- private certificates or signing credentials;
- personal user information;
- unredacted crash dumps; or
- production configuration secrets.

If a credential is exposed, revoke or rotate it immediately and report the exposure privately.

## Responsible disclosure

Please allow reasonable time for investigation and remediation before publicly disclosing a vulnerability. Good-faith research that avoids privacy violations, service disruption, data destruction, and unauthorized access is appreciated.
