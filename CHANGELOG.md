# Changelog

All notable changes to this project will be documented in this file.

The format is based on Keep a Changelog, and this project follows Semantic Versioning.

## [Unreleased]

### Added

- Modular IO public API layout under `include/volt/io/{assets,compression,image,import,text}`.
- `volt::core::Text` Unicode text utility with UTF-8/UTF-16 conversion, normalization, case fold, and locale compare APIs.
- Optional ICU-backed Unicode behavior toggled via `VOLT_ENABLE_ICU` in CMake.
- In-tree codecs and utilities for:
  - zlib/raw DEFLATE decompression and zlib compression
  - custom canonical Huffman codec
  - PNG/BMP/JPEG image decode and encode
  - JSON parse/stringify and UTF helpers
- Asset services with manifest-driven path resolution, default font atlas generation, and centralized `AssetManager`.
- Expanded test targets under `tests/` for text, UTF, JSON, compression, Huffman codec, image codecs, and JPEG performance/benchmark coverage.
- Repository metadata files: `VERSION`, `CHANGELOG.md`, `LICENSE`, and `CONTRIBUTING.md`.

### Changed

- IO codebase reorganized from legacy flat paths to subsystem directories in both `include/` and `src/`.
- Runtime assets standardized around `assets/manifest.json` and image assets under `assets/images/`.
- Build staging now copies manifest, image, UI, and font asset directories to runtime output.
- CMake preset configuration no longer pins a specific Visual Studio generator version.
- `vcpkg.json` builtin baseline updated for current toolchain compatibility.

### Removed

- Legacy manifest text files: `assets/manifest.txt` and `assets/images/manifest.txt`.
- Legacy root asset files: `assets/volt.png` and `assets/volt.svg` (moved under `assets/images/`).
- Legacy flat IO headers and source files previously under `include/volt/io/` and `src/io/`.
- Vendored stb headers in `third_party/stb/`.

## [0.1.0] - 2026-04-08

### Initial Release

- Initial C++20 project scaffold for the Volt platform.
- Cross-platform CMake presets and platform bootstrap/setup scripts.
- Vulkan renderer bootstrap and platform/window abstractions.
- Importer pipeline scaffolding for STEP and 3MF formats.
