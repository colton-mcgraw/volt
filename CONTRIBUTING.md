# Contributing

Thank you for contributing to Volt.

## Getting Started

1. Fork and clone the repository.
2. Create a topic branch from master.
3. Install prerequisites listed in README.md.
4. Run the bootstrap script for your platform.

Windows:

```powershell
.\scripts\windows\bootstrap.ps1
```

Linux:

```bash
bash ./scripts/linux/bootstrap.sh
```

## Development Workflow

1. Keep changes focused and scoped to one concern.
2. Prefer small commits with clear messages.
3. Update or add docs when behavior changes.
4. Add tests when test targets are introduced.

## Coding Guidelines

- Follow existing C++20 style and naming in include/ and src/.
- Keep public interfaces in include/ and implementation in src/.
- Avoid unrelated refactors in the same change.

## Pull Requests

1. Open a pull request against `development`.
2. Describe the problem, the change, and validation steps.
3. Ensure CI is passing before requesting review.
4. Address review feedback in follow-up commits.

## Reporting Issues

When filing a bug, include:

- Operating system and compiler details
- Repro steps
- Expected vs. actual behavior
- Logs or screenshots when applicable
