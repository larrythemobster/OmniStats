# Contributing to OmniStats

Thank you for helping improve OmniStats. Bug fixes, tests, documentation, performance work, accessibility improvements, and focused features are all welcome.

OmniStats is a native Windows application written in C++20. Contributions should be focused, understandable, tested, and consistent with the project's privacy, security, architecture, and source-available license.

## Table of contents

- [Code of conduct](#code-of-conduct)
- [Before you begin](#before-you-begin)
- [Getting started](#getting-started)
- [Project structure](#project-structure)
- [Development workflow](#development-workflow)
- [Coding standards](#coding-standards)
- [Testing](#testing)
- [Network, privacy, and security](#network-privacy-and-security)
- [Commit conventions](#commit-conventions)
- [Pull request process](#pull-request-process)
- [Reporting issues](#reporting-issues)
- [License and contribution terms](#license-and-contribution-terms)

---

## Code of conduct

Be respectful, constructive, and professional. Review the work, not the person. Harassment, personal attacks, and hostile behavior are not acceptable.

Contributors must understand and be able to explain the changes they submit. Do not submit copied code, unreviewed bulk rewrites, or work you cannot maintain during review.

## Before you begin

Small bug fixes and documentation corrections can be submitted directly.

Open an issue before starting a large feature, architectural change, new dependency, new network integration, data-format change, or major UI redesign. Early discussion helps avoid work that conflicts with the roadmap or existing design.

Report vulnerabilities through GitHub private vulnerability reporting when available. Otherwise, use the private security contact method listed on <https://omnistats.org/>. Never post credentials, tokens, crash dumps, private endpoints, personal data, or exploitable security details in a public issue.

The following will not be accepted:

- Malware, hidden data collection, credential theft, or spyware
- Cheats, anti-cheat bypasses, or game-memory manipulation
- Disabled TLS verification or weakened download validation
- Code copied without permission
- Undisclosed changes to telemetry or external data flows
- Unofficial branding, release infrastructure, mirrors, or public rebrands
- Large unrelated rewrites mixed into a feature or bug fix
- Tests that depend on live third-party services

## Getting started

### Prerequisites

| Tool | Requirement | Purpose |
| --- | --- | --- |
| Windows | Windows 10 or newer | Supported development platform |
| Visual Studio | Visual Studio 2022 | MSVC compiler and Windows tooling |
| Workload | Desktop development with C++ | Compiler, linker, and Windows SDK |
| CMake | 3.22 or newer | Project configuration and builds |
| Git | Current version | Source control |
| vcpkg | Bootstrapped checkout | C++ dependency management |

### Fork and clone

```powershell
git clone https://github.com/<your-account>/<your-fork>.git
cd OmniStats
```

### Set up vcpkg

Use an existing vcpkg checkout or create one:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\src\vcpkg
C:\src\vcpkg\bootstrap-vcpkg.bat -disableMetrics
$env:VCPKG_ROOT = "C:\src\vcpkg"
```

### Configure, build, and test

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

The Release executable is normally produced at:

```text
out/windows-release/Release/OmniStats.exe
```

See [docs/BUILDING.md](docs/BUILDING.md) for additional build options and troubleshooting.

## Project structure

```text
OmniStats/
├── src/
│   ├── core/              # Configuration, session state, input, reducers, and side effects
│   ├── database/          # SQLite persistence and database worker ownership
│   ├── network/           # Stats API, Tracker, telemetry, Discord, uploads, and updates
│   ├── ui/                # Win32, Direct3D 11, ImGui, ImPlot, tray, and overlay rendering
│   ├── updater/           # External updater executable
│   └── main.cpp           # Main application entry point
├── tests/
│   ├── core/              # Configuration, input, state, and reducer tests
│   ├── database/          # Persistence tests
│   ├── integration/       # Internal pipeline tests
│   ├── network/           # Network component tests
│   ├── ui/                # Headless UI and layout tests
│   └── fuzz/              # Parser fuzz targets
├── benchmarks/            # Optional Google Benchmark targets
├── installer/wix/         # Supported WiX MSI package
├── resources/             # Icons, images, and resource scripts
├── docs/                  # Architecture, privacy, and build documentation
├── CMakeLists.txt         # Build targets and dependency wiring
├── CMakePresets.json      # Supported build and test presets
└── vcpkg.json             # Dependency manifest and project version
```

Read [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) before changing responsibilities across modules.

## Development workflow

1. Update `main` and create a focused branch:

   ```powershell
   git checkout main
   git pull --ff-only
   git checkout -b feat/short-description
   # or: git checkout -b fix/short-description
   ```

2. Make one logical change. Avoid unrelated formatting, renaming, or cleanup.

3. Add or update tests for changed behavior.

4. Build and run the Release test preset:

   ```powershell
   cmake --preset windows-release
   cmake --build --preset windows-release
   ctest --preset windows-release
   ```

5. Update documentation when behavior, configuration, privacy, installation, external services, or release steps change.

6. Review the diff before committing:

   ```powershell
   git diff --check
   git diff
   ```

7. Push the branch and open a pull request.

## Coding standards

### C++ and architecture

- Use C++20 and follow the surrounding code style.
- Follow `.editorconfig`: four-space indentation, UTF-8 text, and no trailing whitespace.
- Prefer RAII and standard-library ownership types over manual lifetime management.
- Keep thread ownership and shutdown behavior explicit.
- Do not block the render thread with network, database, file, or long-running work.
- Keep transport and third-party request logic out of UI code.
- Keep business rules out of rendering functions when they belong in `src/core`.
- Route persistent data through `DatabaseManager` rather than opening unrelated SQLite connections.
- Use the existing configuration system for persisted settings.
- Prefer small functions and classes with one clear responsibility.
- Comment intent and constraints, not code that already explains itself.
- Avoid formatting-only changes in functional pull requests.

### UI

- Preserve the in-game overlay, dashboard, edit mode, and second-monitor behavior where applicable.
- Check practical Windows scaling and resolution configurations.
- Follow the existing theme and layout systems instead of adding isolated styling rules.
- Include before-and-after screenshots for meaningful visual changes.
- Verify relevant mouse, keyboard, and controller interactions.

### Persisted data

- Preserve existing user data whenever possible.
- Treat database schemas and configuration formats as compatibility-sensitive.
- Add migration or fallback behavior when persisted formats change.
- Test missing, empty, malformed, older, and partially written data where relevant.
- Never commit a real user database or configuration file.

### Dependencies

- Discuss new runtime dependencies before adding them.
- Update `vcpkg.json`, CMake, and documentation together.

## Testing

Every behavior change should include appropriate automated coverage unless the pull request clearly explains why automation is impractical.

### Full test suite

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

### Focused tests

```powershell
ctest --preset windows-release -R MMRFetcher
ctest --preset windows-release -R DatabaseManager
ctest --preset windows-release -R Overlay
```

Tests must:

- Be deterministic and safe to run repeatedly
- Avoid live Tracker, Discord, Ballchasing, update, and telemetry endpoints
- Use local servers, fixtures, test doubles, or stored test data for network behavior
- Clean up temporary files, sockets, environment changes, and worker threads
- Cover failure, shutdown, malformed-input, and retry paths where relevant
- Preserve the intent of existing tests rather than weakening them to pass

Benchmarks and fuzz targets are useful, but they do not replace correctness tests.

## Network, privacy, and security

OmniStats communicates with external services, so these changes receive additional review.

### Required startup diagnostics

After the privacy notice is accepted, OmniStats sends the documented startup diagnostic payload. Do not add fields, identifiers, destinations, or new required requests without maintainer approval and matching updates to:

- `docs/PRIVACY.md`
- `docs/ARCHITECTURE.md`
- The in-app privacy notice
- Installer and website privacy wording where applicable

Startup diagnostics must not include match data, lobby player names, replay contents, access tokens, or raw service responses.

### Tracker Network

Tracker rank lookup is an existing optional feature. Changes to its requests, parsing, queue, rate limits, retries, identifiers, or runtime dependencies must:

- Preserve the user-facing opt-in setting
- Keep requests bounded and rate limited
- Avoid logging full player identifiers or raw responses in normal logs
- Include deterministic tests without live requests
- Update privacy and troubleshooting documentation when behavior changes
- Avoid presenting the integration as an official Tracker API without authorization

### Updates and downloads

- Use HTTPS.
- Keep TLS certificate and hostname verification enabled.
- Preserve SHA-256 validation for downloaded files.
- Fail safely when validation, download, repair, or replacement fails.
- Do not claim an unsigned artifact is signed.
- Keep application, updater, and MSI versions consistent.

Never commit tokens, credentials, real user data, installation identifiers, crash dumps, or logs containing personal paths. Use redacted or synthetic fixtures.

## Commit conventions

Use a clear Conventional Commit-style subject:

```text
<type>(<scope>): <short description>
```

### Types

| Type | Use for |
| --- | --- |
| `feat` | New user-facing behavior |
| `fix` | Bug fixes |
| `refactor` | Structural changes without intended behavior changes |
| `perf` | Performance or resource-usage improvements |
| `test` | Test additions or corrections |
| `docs` | Documentation-only changes |
| `build` | CMake, vcpkg, packaging, or dependency changes |
| `ci` | GitHub Actions or automated validation |
| `security` | Security hardening or vulnerability fixes |
| `chore` | Focused maintenance |

### Suggested scopes

`overlay`, `dashboard`, `ui`, `stats-api`, `telemetry`, `mmr`, `database`, `replays`, `discord`, `updater`, `installer`, `config`, `privacy`, and `deps`.

### Examples

```text
feat(overlay): add compact session summary layout
fix(mmr): handle profiles with missing playlist segments
fix(database): preserve history after interrupted shutdown
refactor(telemetry): separate payload creation from transport
test(updater): cover checksum mismatch during repair
build(installer): include Tracker runtime dependencies
docs(privacy): document startup diagnostic fields
```

## Pull request process

Use the commit format for the pull request title:

```text
fix(overlay): prevent roster clipping at high UI scale
```

The description should include:

- What changed and why
- Affected modules
- User-visible behavior
- Tests performed
- Screenshots for UI changes
- Configuration, database, privacy, installer, or compatibility impact
- Known limitations or follow-up work

Before requesting review, confirm:

- [ ] The change is focused and contains no unrelated cleanup.
- [ ] Release configuration builds successfully.
- [ ] The full Release test preset passes.
- [ ] Changed behavior has appropriate tests.
- [ ] Threads, temporary files, and resources are cleaned up correctly.
- [ ] UI changes include screenshots and practical scale checks.
- [ ] Network behavior remains bounded, secure, and documented.
- [ ] Privacy documentation matches any data-flow changes.
- [ ] No secrets, private data, local paths, binaries, databases, or crash dumps are included.
- [ ] New dependencies are documented and included in the supported build configuration.

Maintainers may ask for a pull request to be split, simplified, tested further, or aligned with the existing architecture. Approval is not guaranteed; changes may be declined when they are out of scope, too broad, insufficiently tested, legally unclear, privacy-invasive, or difficult to maintain.

## Reporting issues

Use the provided GitHub issue forms.

A useful bug report includes:

- OmniStats version and whether it is an official or local build
- Windows version
- Rocket League platform when relevant
- Affected feature or screen
- Exact reproduction steps
- Expected and actual behavior
- Whether the problem is consistent or intermittent
- A minimal, sanitized log excerpt only when necessary

Do not upload complete logs, configuration files, databases, crash dumps, tokens, installation identifiers, or player data to a public issue.

For feature requests, describe the problem, expected user-visible behavior, existing workarounds, and important privacy, performance, compatibility, or maintenance constraints.

## License and contribution terms

OmniStats is source available under the [PolyForm Internal Use License 1.0.0](LICENSE). It is not an open-source license and does not permit general redistribution.

The [OmniStats Contribution Exception](CONTRIBUTION_EXCEPTION.md) grants narrow permission to create a fork, branch, patch, or pull request only as reasonably necessary to propose changes back to the official OmniStats repository. It does not permit unofficial releases, public rebrands, mirrors, competing distributions, or use of OmniStats branding and infrastructure for another project.

By submitting code, documentation, tests, assets, issue content, or another contribution, you agree to the contribution license in `CONTRIBUTION_EXCEPTION.md` and confirm that you have the right to submit the material.

Thank you for helping make OmniStats more reliable, useful, and maintainable.
