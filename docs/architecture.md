# Architecture Overview

## High-Level Modules

- `include/volt`
  - Public headers grouped by subsystem (`core`, `platform`, `render`, `io`).

- `src/core`
  - Core implementation sources.
- `src/math`
  - Foundational math library sources (vectors, matrices, quaternions, transforms, projection, coordinate conversion, and typed unit conversions).
- `src/physics`
  - Physics/domain equation helpers built on top of typed math quantities.
  - SPICE-oriented modeling scaffolding (netlist/model/stamping/simulation contracts).
- `src/platform`
  - Platform implementation sources (currently GLFW-backed).
- `src/render`
  - Rendering implementation sources and Vulkan backend.
- `src/ui`
  - Custom retained-mode UI scaffolding integrated with the render loop.
  - Includes widget submission, basic interaction routing scaffolds, and style-token loading.
  - Includes panel/flow layout helpers, UTF-8 text-run scaffolding, and UI resource registry interfaces.
- `src/io`
  - Import pipeline implementation and format adapters.
- `src/app`
  - Executable composition root.

## Data/Control Direction

- `app` orchestrates startup and frame loop.
- `platform` owns input and window lifecycle.
- `render` consumes platform handles and owns GPU resources.
- `ui` consumes platform input snapshots and emits renderer-consumable UI pass commands.
  - UI pass includes a mesh-build stage (`UiMeshData`) that converts command streams into batched quad geometry.
  - Renderer consumes UI mesh batches, uploads scaffold vertex/index buffers, and records indexed draw calls per batch.
  - Basic widgets: text, button, slider, icon, image.
  - Complex scaffolds: chart and schematic placeholders for later specialization.
  - Layout helpers: panel scopes plus flow-column/flow-row auto placement.
  - Text/resource contracts: UTF-8 text runs and custom loader/atlas interfaces.
- `io` translates external formats into internal model representations.
- `core` defines shared domain types used across modules.
- `math` defines reusable numeric and unit conversion foundations consumed by render/io first.
- `physics` defines physical/electrical equations using math quantities.
  - `volt::physics::electrical` currently includes typed Ohm and power relationships.
  - `volt::physics::spice` provides extension points for future SPICE implementation:
    - netlist and subcircuit representations
    - model parameter catalogs
    - MNA stamping interfaces
    - analysis request and simulation result contracts

## Logging Layer

- `core` provides a centralized logging facade backed by `spdlog`.
- Runtime feature toggles (event/tick tracing) are debug-only controls.
- In non-debug configurations, logging macros compile to no-op so optional diagnostics do not add runtime overhead.

## Frame Stage Scaffold

- App loop stages are currently: `poll input -> run UI layout/paint scaffold -> renderer tick`.
- Renderer tick stages are currently scaffolded as: `begin frame -> scene pass hook -> UI pass hook -> end frame`.
- Scene submission and UI pass recording are wired as placeholders to preserve deterministic frame ownership before full Vulkan pass implementation.

## Input Scaffold

- Platform window now captures keyboard, mouse position/button, and scroll snapshots.
- Input snapshots are reset for transient deltas each frame and exposed to UI as a platform-agnostic contract.

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

## Known Deferred Work

- Vulkan command buffers, render passes, framebuffers, and synchronization are still placeholder-level hooks.
- UI layout tree, retained widgets, and real draw command generation are not implemented yet.
- Model import data is not yet uploaded into GPU buffers for scene rendering.
