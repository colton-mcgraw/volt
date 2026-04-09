#!/usr/bin/env bash
set -euo pipefail

use_vcpkg=0
skip_build=0
build_dir=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --use-vcpkg)
      use_vcpkg=1
      shift
      ;;
    --skip-build)
      skip_build=1
      shift
      ;;
    --build-dir)
      build_dir="${2:-}"
      if [[ -z "${build_dir}" ]]; then
        printf 'Missing value for --build-dir\n' >&2
        exit 1
      fi
      shift 2
      ;;
    *)
      printf 'Unknown argument: %s\n' "$1" >&2
      exit 1
      ;;
  esac
done

log() {
  printf '[bootstrap] %s\n' "$1"
}

workspace_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${workspace_root}"

setup_script="${workspace_root}/scripts/linux/setup.sh"
if [[ ! -f "${setup_script}" ]]; then
  printf '[bootstrap] ERROR: Missing setup script at %s\n' "${setup_script}" >&2
  exit 1
fi

if [[ "${use_vcpkg}" -eq 1 ]]; then
  bash "${setup_script}" --require-vcpkg
else
  bash "${setup_script}"
fi

if [[ "${use_vcpkg}" -eq 1 ]]; then
  configure_preset="linux-gcc-vcpkg"
  if [[ -z "${build_dir}" ]]; then
    build_dir="build/bootstrap/linux-gcc-vcpkg"
  fi
else
  configure_preset="linux-gcc"
  if [[ -z "${build_dir}" ]]; then
    build_dir="build/bootstrap/linux-gcc"
  fi
fi

log "Selected configure preset: ${configure_preset}"
log "Selected build directory: ${build_dir}"

cmake --preset "${configure_preset}" -B "${build_dir}"

if [[ "${skip_build}" -eq 0 ]]; then
  cmake --build "${build_dir}"
fi

log "Done"
