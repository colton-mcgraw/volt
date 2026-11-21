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

$configurePreset = "windows-clang"
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
  $BuildDir = "build/bootstrap/$configurePreset"
}

Write-Info "Selected configure preset: $configurePreset"
Write-Info "Selected build directory: $BuildDir"

$configureArgs = @("--preset", $configurePreset, "-B", $BuildDir, "--fresh")
if ($UseVcpkg) {
  $configureArgs += "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
}

Invoke-Checked -Command "cmake" -CommandArgs $configureArgs

if (-not $SkipBuild) {
  Invoke-Checked -Command "cmake" -CommandArgs @("--build", $BuildDir, "--config", $Configuration)
}

Write-Info "Done"
