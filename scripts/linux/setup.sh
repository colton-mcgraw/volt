#!/usr/bin/env bash
set -euo pipefail

require_vcpkg=0
if [[ "${1:-}" == "--require-vcpkg" ]]; then
  require_vcpkg=1
fi

log() {
  printf '[setup] %s\n' "$1"
}

workspace_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
log "Workspace: ${workspace_root}"

if ! command -v cmake >/dev/null 2>&1; then
  printf '[setup] ERROR: CMake is not installed or not on PATH.\n' >&2
  exit 1
fi
log "Found CMake"

if command -v c++ >/dev/null 2>&1; then
  log "Found C++ compiler: $(c++ --version | head -n 1)"
elif command -v g++ >/dev/null 2>&1; then
  log "Found C++ compiler: $(g++ --version | head -n 1)"
elif command -v clang++ >/dev/null 2>&1; then
  log "Found C++ compiler: $(clang++ --version | head -n 1)"
else
  printf '[setup] ERROR: No C++ compiler found (c++, g++, or clang++).\n' >&2
  exit 1
fi

if [[ -n "${VULKAN_SDK:-}" ]]; then
  log "VULKAN_SDK detected: ${VULKAN_SDK}"
else
  log "VULKAN_SDK is not set. CMake may still find Vulkan from system packages."
fi

resolve_vcpkg_root() {
  if [[ -n "${VCPKG_ROOT:-}" ]] && [[ -f "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" ]]; then
    printf '%s' "${VCPKG_ROOT}"
    return
  fi

  local candidates=(
    "${HOME}/vcpkg"
    "/opt/vcpkg"
  )

  local candidate
  for candidate in "${candidates[@]}"; do
    if [[ -f "${candidate}/scripts/buildsystems/vcpkg.cmake" ]]; then
      export VCPKG_ROOT="${candidate}"
      printf '%s' "${candidate}"
      return
    fi
  done

  printf ''
}

vcpkg_root="$(resolve_vcpkg_root)"
if [[ -n "${vcpkg_root}" ]]; then
  log "Using VCPKG_ROOT=${vcpkg_root}"
elif [[ "${require_vcpkg}" -eq 1 ]]; then
  printf '[setup] ERROR: VCPKG_ROOT is required but no valid vcpkg toolchain path was found.\n' >&2
  exit 1
else
  log "vcpkg not detected. Will use non-vcpkg preset."
fi
