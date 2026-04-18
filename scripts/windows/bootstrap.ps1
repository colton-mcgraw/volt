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

function Resolve-VisualStudioGenerator {
  $helpOutput = & cmake --help
  if ($LASTEXITCODE -ne 0) {
    throw "Unable to query CMake generator list."
  }

  $generatorCandidates = @()
  foreach ($line in $helpOutput) {
    $match = [regex]::Match($line, '^\s*\*?\s*Visual Studio (\d+)\s+(\d{4})\s*=')
    if (-not $match.Success) {
      continue
    }

    $major = [int]$match.Groups[1].Value
    if ($major -lt 17) {
      continue
    }

    $year = [int]$match.Groups[2].Value
    $generatorCandidates += [PSCustomObject]@{
      Major = $major
      Year = $year
      Name = "Visual Studio $major $year"
    }
  }

  if ($generatorCandidates.Count -eq 0) {
    return $null
  }

  return ($generatorCandidates |
      Sort-Object -Property @{Expression = "Major"; Descending = $true}, @{Expression = "Year"; Descending = $true} |
      Select-Object -First 1).Name
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

$visualStudioGenerator = Resolve-VisualStudioGenerator
if ([string]::IsNullOrWhiteSpace($visualStudioGenerator)) {
  throw "No supported Visual Studio generator (17+) was found in CMake generator list."
}

Write-Info "Detected Visual Studio generator: $visualStudioGenerator"

Invoke-Checked -Command "cmake" -CommandArgs @("--preset", $configurePreset, "-B", $BuildDir, "--fresh")

if (-not $SkipBuild) {
  Invoke-Checked -Command "cmake" -CommandArgs @("--build", $BuildDir, "--config", $Configuration)
}

Write-Info "Done"
