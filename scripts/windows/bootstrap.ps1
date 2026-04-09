param(
  [switch]$UseVcpkg,
  [switch]$SkipBuild,
  [string]$BuildDir,
  [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
  [string]$Configuration = "Debug"
)

$ErrorActionPreference = "Stop"

function Write-Info {
  param([string]$Message)
  Write-Host "[bootstrap] $Message" -ForegroundColor Green
}

function Invoke-Checked {
  param([string]$Command, [string[]]$CommandArgs)

  & $Command @CommandArgs
  if ($LASTEXITCODE -ne 0) {
    throw "Command failed with exit code ${LASTEXITCODE}: $Command $($CommandArgs -join ' ')"
  }
}

$workspaceRoot = Resolve-Path (Join-Path $PSScriptRoot "../..")
Set-Location $workspaceRoot

$setupScript = Join-Path $PSScriptRoot "setup.ps1"
if (-not (Test-Path $setupScript)) {
  throw "Missing setup script at $setupScript"
}

& $setupScript -RequireVcpkg:$UseVcpkg

$configurePreset = if ($UseVcpkg) { "windows-msvc-vcpkg" } else { "windows-msvc" }
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
  $BuildDir = if ($UseVcpkg) { "build/bootstrap/windows-msvc-vcpkg" } else { "build/bootstrap/windows-msvc" }
}

Write-Info "Selected configure preset: $configurePreset"
Write-Info "Selected build directory: $BuildDir"

Invoke-Checked -Command "cmake" -CommandArgs @("--preset", $configurePreset, "-B", $BuildDir)

if (-not $SkipBuild) {
  Invoke-Checked -Command "cmake" -CommandArgs @("--build", $BuildDir, "--config", $Configuration)
}

Write-Info "Done"
