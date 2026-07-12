# Security policy

Report suspected vulnerabilities privately through the contact method published on <https://omnistats.org/>. Do not open a public issue containing credentials, tokens, personal data, or an exploitable proof of concept.

## Supported releases

Security fixes are provided for the latest public release. Older builds may be asked to upgrade or reinstall through the official MSI.

## Release protections

The release process must:

- build from a clean, reviewed commit;
- run the Windows CI test suite;
- publish SHA-256 checksums for the application, updater, and MSI;
- download updates only over HTTPS;
- verify the downloaded executable against the expected SHA-256 value before replacement; and
- document whether a release is signed or unsigned.

Code signing is optional because it has a direct cost. When a certificate becomes available, `scripts/build-release.ps1` can sign and timestamp all release artifacts. Until then, users should expect Windows to show an **Unknown publisher** or SmartScreen warning and should download only from the official OmniStats website or release page.

A checksum hosted beside a binary detects accidental corruption and unexpected replacement when the checksum is obtained through the same trusted HTTPS channel. It is not equivalent to independent publisher authentication.

## Third-party integrations

Startup diagnostics are required after privacy acceptance. Tracker rank lookup, Discord Rich Presence, Ballchasing uploads, update checks, and crash reports remain optional. Changes to any external endpoint, request behavior, stored field, or consent flow require security and privacy review.
