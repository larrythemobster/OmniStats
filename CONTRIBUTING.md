# Contributing

Thanks for improving OmniStats.

## Get started

This is a Windows C++20 project using CMake, vcpkg, D3D11, ImGui, ImPlot, SQLite, SDL2, curl, asio, discord-rpc, Google Test, and Google Benchmark.

Follow [docs/BUILDING.md](docs/BUILDING.md) to install vcpkg, configure the project, build it, and run tests.

## Pull requests

Keep pull requests focused. A small, reviewable behavior change is better than a mixed cleanup.

Before opening a pull request:

- Run the relevant tests.
- Explain the user-visible behavior change and validation.
- Do not commit generated binaries, local configuration, logs, crash dumps, databases, private paths, deploy keys, or API tokens.
- Do not add temporary scratch artifacts, generated planning files, or private notes.
- Avoid broad formatting-only changes unless formatting is the explicit purpose.

## Code style

- Use 4 spaces for indentation.
- Keep opening braces on the same line as the statement.
- Prefer `#pragma once` for headers.
- Use `PascalCase` for types and functions.
- Use `camelCase` for regular members.
- Use `m_` for private members.
- Use `snake_case` for serialized configuration fields.
- Add comments only when they explain a non-obvious constraint, decision, or edge case.

## Tests

Tests live under `tests/` and use Google Test. Prefer deterministic tests that run offline. Keep network-dependent tests isolated and clearly named.

## License and branding

OmniStats is source-available under the PolyForm Internal Use License 1.0.0, with the narrow contribution exception in `CONTRIBUTION_EXCEPTION.md`. Personal/internal use, local modification, and contributions to the official OmniStats project are allowed. Redistribution, unofficial releases, public rebrands, and competing releases require written permission.

The OmniStats name, logo, official services, update server, API endpoints, Discord application, and support invite are not licensed for reuse. See `LICENSE`, `CONTRIBUTION_EXCEPTION.md`, and `TRADEMARKS.md`.
