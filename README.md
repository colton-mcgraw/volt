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
  - `spdlog`

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

## Math and Units Foundation

- `volt_math` now provides vectors, matrices, quaternions, transforms, projections, and coordinate-space helpers.
- Units include broad physical domains plus EDA/3D-oriented concepts:
  - length, angle, mass, time, area, volume, temperature
  - voltage, current, resistance, power, capacitance, inductance
  - magnetic flux density, magnetic flux, frequency, charge, conductance
  - energy, force, pressure
- `volt_math` units provide strongly typed quantities and conversion helpers.
- Physical equations are implemented in `volt_physics`:
  - `volt::physics::electrical` currently provides typed Ohm/power helpers (`V = I * R`, `I = V / R`, `R = V / I`, `P = V * I`, `P = I^2 * R`, `P = V^2 / R`).
- `volt::physics::spice` now includes SPICE-oriented scaffolding for future solver implementation:
  - netlist and element structures
  - model library and parameter definitions
  - MNA stamping interfaces
  - simulation options/request/result contracts

## Continuous Integration

GitHub Actions CI is configured in [.github/workflows/ci.yml](.github/workflows/ci.yml).

- Windows job runs [scripts/windows/bootstrap.ps1](scripts/windows/bootstrap.ps1)
- Linux job runs [scripts/linux/bootstrap.sh](scripts/linux/bootstrap.sh)

This keeps CI behavior aligned with local developer bootstrap commands.

## Logging

- Logging is routed through a core wrapper on top of `spdlog`.
- `Debug` builds keep runtime-adjustable logging and feature toggles enabled.
- Non-`Debug` builds compile logging macros to no-op so debug-depth toggles are optimized away.
- In `Debug`, logs are written to `logs/volt-debug.log` even when no console is attached.
- On Windows `Debug`, logs are also sent to the Visual Studio debugger output window.
- In `Debug`, defaults are `VOLT_LOG_LEVEL=trace`, `VOLT_EVENT_TRACE=on`, and `VOLT_TICK_TRACE=on` when these env vars are not set.

Debug environment toggles:

- `VOLT_LOG_LEVEL`: `trace|debug|info|warn|error|critical` (default `info`)
- `VOLT_EVENT_TRACE`: `1|true|yes` to emit per-event trace lines
- `VOLT_TICK_TRACE`: `1|true|yes` to emit frame tick trace lines
- Set `VOLT_EVENT_TRACE=0` or `VOLT_TICK_TRACE=0` to disable those debug defaults.
- `VOLT_LOG_CATEGORIES`: category filter in Debug builds. Supported values: `core,app,platform,render,ui,io,event,all,none`.
  - Separators accepted: comma `,`, semicolon `;`, or whitespace.

Example:

- `VOLT_LOG_LEVEL=debug`
- `VOLT_LOG_CATEGORIES=app,render,event`
- `VOLT_LOG_CATEGORIES=app render event`

## UI Foundation

- `volt_ui` now includes a retained-mode widget submission foundation for:
  - text, button, slider, icon, and image elements
- Complex UI extension scaffolds are in place for:
  - chart widgets (line/bar placeholder path)
  - schematic canvas widgets (symbol/net placeholder path)
- UI rendering currently builds a typed draw-command stream and forwards it through the renderer UI pass callback.
- UI now also builds per-frame mesh data (vertices/indices/batches) from render commands and feeds it to renderer-side scaffold buffers.
- Renderer UI pass now uploads scaffold UI mesh buffers and emits indexed draws per batch.
- External style scaffolding is available via `assets/ui/default.style` and loaded through `StyleSheet`.
- Layout scaffolding now includes container helpers:
  - panel scopes (`beginPanel`/`endPanel`) with clipped child bounds
  - flow-column and flow-row placement helpers for deterministic auto layout
- Text scaffolding now includes UTF-8 text-run generation with per-command glyph-count summaries.
- Resource scaffolding includes interfaces for custom font/image loaders and font atlas builders.
