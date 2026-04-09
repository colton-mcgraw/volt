# Architecture Overview

## High-Level Modules

- `include/volt`
  - Public headers grouped by subsystem (`core`, `platform`, `render`, `io`).

- `src/core`
  - Core implementation sources.
- `src/platform`
  - Platform implementation sources (currently GLFW-backed).
- `src/render`
  - Rendering implementation sources and Vulkan backend.
- `src/io`
  - Import pipeline implementation and format adapters.
- `src/app`
  - Executable composition root.

## Data/Control Direction

- `app` orchestrates startup and frame loop.
- `platform` owns input and window lifecycle.
- `render` consumes platform handles and owns GPU resources.
- `io` translates external formats into internal model representations.
- `core` defines shared domain types used across modules.

## File Import Strategy

- Importers implement `IModelImporter`.
- `ImporterRegistry` maps file extensions to importers.
- Import execution uses `ImportRequest` (path, format hint, options).
- Results flow through `ImportResult` (normalized scene + structured diagnostics).
- `ImportPipeline` runs staged flow: `probe -> parse -> normalize -> validate`.
- STEP and 3MF importers are scaffolded and currently return `not implemented yet` diagnostics.

## Platform Targets

- Windows and Linux first-class.
- Rendering API: Vulkan.
- UI system: custom, integrated into native rendering loop.
