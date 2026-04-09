param(
  [switch]$RequireVcpkg
)

$ErrorActionPreference = "Stop"

function Write-Info {
  param([string]$Message)
  Write-Host "[setup] $Message" -ForegroundColor Cyan
}

function Test-Command {
  param([string]$Name)
  return $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}

function Resolve-VcpkgRoot {
  if ($env:VCPKG_ROOT -and (Test-Path (Join-Path $env:VCPKG_ROOT "scripts/buildsystems/vcpkg.cmake"))) {
    return $env:VCPKG_ROOT
  }

  $commonLocations = @(
    "C:\src\vcpkg",
    "$env:USERPROFILE\vcpkg"
  )

  foreach ($candidate in $commonLocations) {
    if (Test-Path (Join-Path $candidate "scripts/buildsystems/vcpkg.cmake")) {
      $env:VCPKG_ROOT = $candidate
      return $candidate
    }
  }

  return $null
}

$workspaceRoot = Resolve-Path (Join-Path $PSScriptRoot "../..")
Write-Info "Workspace: $workspaceRoot"

if (-not (Test-Command "cmake")) {
  throw "CMake is not installed or not available on PATH."
}
Write-Info "Found CMake"

# We only require cl.exe for Visual Studio generator workflows.
if (-not (Test-Command "cl")) {
  Write-Info "MSVC compiler not currently on PATH. This can still work if CMake locates Visual Studio automatically."
} else {
  Write-Info "Found MSVC compiler"
}

if ($env:VULKAN_SDK) {
  Write-Info "VULKAN_SDK detected: $env:VULKAN_SDK"
} else {
  Write-Info "VULKAN_SDK is not set. CMake may still find Vulkan from system install."
}

$vcpkgRoot = Resolve-VcpkgRoot
if ($vcpkgRoot) {
  Write-Info "Using VCPKG_ROOT=$vcpkgRoot"
} elseif ($RequireVcpkg) {
  throw "VCPKG_ROOT is required but no valid vcpkg toolchain path was found."
} else {
  Write-Info "vcpkg not detected. Will use non-vcpkg preset."
}
