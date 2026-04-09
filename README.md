# Volt

[![CI](https://github.com/<OWNER>/<REPO>/actions/workflows/ci.yml/badge.svg)](https://github.com/<OWNER>/<REPO>/actions/workflows/ci.yml)

Modern cross-platform electronics design platform scaffold with C++20 and Vulkan.

Project domain: [volteda.net](https://volteda.net)

## Current Scope

- Native C++ core and platform layers.
- Vulkan instance bootstrap for the renderer.
- Importer abstraction with STEP and 3MF extension points.
- CMake + vcpkg manifest setup for Windows and Linux.

## Project Layout

- `include/volt/*`: public headers
- `src/*`: implementation sources by subsystem
- `src/app/main.cpp`: application entry point
- `cmake/*`: CMake helper modules
- `scripts/*`: local bootstrap/setup scripts

## Requirements

- CMake 3.25+
- C++20 compiler
- Vulkan SDK (or system Vulkan loader + headers)
- vcpkg (optional, recommended for third-party packages)

## Dependency Strategy

- Required:
  - `glfw3`
  - `Vulkan`

STEP and 3MF importers are scaffolded as internal placeholders and are intended to be implemented with project-native parsing/import pipelines.

## Configure and Build

### Bootstrap Scripts

Use the platform bootstrap scripts to auto-check prerequisites and select the right preset.
By default, bootstrap uses non-vcpkg presets; pass the vcpkg flag to opt in.
Bootstrap uses isolated build directories under build/bootstrap to avoid cache conflicts.

Windows:

```powershell
.\scripts\windows\bootstrap.ps1
```

Custom build directory:

```powershell
.\scripts\windows\bootstrap.ps1 -BuildDir build/custom/windows-dev
```

Windows with vcpkg required:

```powershell
$env:VCPKG_ROOT = "C:\src\vcpkg"
.\scripts\windows\bootstrap.ps1 -UseVcpkg
```

Linux:

```bash
bash ./scripts/linux/bootstrap.sh
```

Custom build directory:

```bash
bash ./scripts/linux/bootstrap.sh --build-dir build/custom/linux-dev
```

Linux with vcpkg required:

```bash
export VCPKG_ROOT=$HOME/vcpkg
bash ./scripts/linux/bootstrap.sh --use-vcpkg
```

Setup-only scripts (environment checks without configure/build):

Windows:

```powershell
.\scripts\windows\setup.ps1
```

Linux:

```bash
bash ./scripts/linux/setup.sh
```

### Windows (PowerShell)

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-build
```

### Windows with vcpkg (PowerShell)

```powershell
$env:VCPKG_ROOT = "C:\src\vcpkg"
cmake --preset windows-msvc-vcpkg
cmake --build --preset windows-build-vcpkg
```

### Linux (bash)

```bash
cmake --preset linux-gcc
cmake --build --preset linux-build
```

### Linux with vcpkg (bash)

```bash
export VCPKG_ROOT=$HOME/vcpkg
cmake --preset linux-gcc-vcpkg
cmake --build --preset linux-build-vcpkg
```

## Format Import Status

- `.step`/`.stp`: importer interface present, implementation pending
- `.3mf`: importer interface present, implementation pending

Current stubs return a clear `not implemented yet` result so custom importer work can be added incrementally.

Importer contract highlights:

- `ImportRequest`: path, format hint, and import options
- `ImportResult`: success flag, message, detected format, normalized scene payload, and diagnostic issues
- `ImportIssue`: structured severity/code/message diagnostics

## Suggested Next Milestones

1. Implement Vulkan device, swapchain, render pass, and frame graph.
2. Introduce ECS or scene graph for board and assembly visualization.
3. Implement project-native STEP and 3MF import pipelines.
4. Build deterministic command stack for editing and undo/redo.
5. Add CI for Windows and Linux with sanitizer and static-analysis jobs.

## Continuous Integration

GitHub Actions CI is configured in [.github/workflows/ci.yml](.github/workflows/ci.yml).

- Windows job runs [scripts/windows/bootstrap.ps1](scripts/windows/bootstrap.ps1)
- Linux job runs [scripts/linux/bootstrap.sh](scripts/linux/bootstrap.sh)

This keeps CI behavior aligned with local developer bootstrap commands.
