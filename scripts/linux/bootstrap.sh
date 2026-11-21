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
  if [[ -z "${VCPKG_ROOT:-}" ]]; then
    for candidate in "${HOME}/vcpkg" "/opt/vcpkg"; do
      if [[ -f "${candidate}/scripts/buildsystems/vcpkg.cmake" ]]; then
        VCPKG_ROOT="${candidate}"
        break
      fi
    done
  fi
else
  bash "${setup_script}"
fi

case "$(uname -s)" in
  Darwin) configure_preset="macos-clang" ;;
  *) configure_preset="linux-clang" ;;
esac

configure_args=()
if [[ "${use_vcpkg}" -eq 1 ]]; then
  configure_args+=("-DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
fi

if [[ -z "${build_dir}" ]]; then
  build_dir="build/bootstrap/${configure_preset}"
fi

log "Selected configure preset: ${configure_preset}"
log "Selected build directory: ${build_dir}"

cmake --preset "${configure_preset}" -B "${build_dir}" ${configure_args[@]+"${configure_args[@]}"}

if [[ "${skip_build}" -eq 0 ]]; then
  cmake --build "${build_dir}"
fi

log "Done"
