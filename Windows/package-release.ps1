[CmdletBinding()]
param(
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$windowsRoot = $PSScriptRoot
$buildRoot = Join-Path $windowsRoot "build"
$packageRoot = Join-Path $buildRoot "package"
$packageName = "DelayLamaStandalone-v1.0-windows-x64"
$stageRoot = Join-Path $packageRoot $packageName
$zipPath = Join-Path $packageRoot ($packageName + ".zip")
$checksumPath = $zipPath + ".sha256"
$releaseOutput = Join-Path $buildRoot "Release"

function Invoke-RequiredCommand {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    Write-Host ("> {0} {1}" -f $Path, ($Arguments -join " "))
    & $Path @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code $($LASTEXITCODE): $Path"
    }
}

function Require-File {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required release file was not produced: $Path"
    }
}

$cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
if (-not $cmakeCommand) {
    throw "CMake was not found. Install CMake, the Windows SDK, and the Visual Studio C++ workload."
}

if ($Clean -and (Test-Path -LiteralPath $buildRoot)) {
    Remove-Item -LiteralPath $buildRoot -Recurse -Force
}

if (Test-Path -LiteralPath $packageRoot) {
    Remove-Item -LiteralPath $packageRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null

$configureArguments = @(
    "-S", $windowsRoot,
    "-B", $buildRoot,
    "-A", "x64",
    "-DCMAKE_BUILD_TYPE=Release"
)
Invoke-RequiredCommand -Path $cmakeCommand.Source -Arguments $configureArguments
Invoke-RequiredCommand -Path $cmakeCommand.Source -Arguments @(
    "--build", $buildRoot,
    "--config", "Release",
    "--parallel"
)

$executablePath = Join-Path $releaseOutput "DelayLamaStandalone.exe"
$resourceSource = Join-Path $releaseOutput "Resources"
Require-File -Path $executablePath
if (-not (Test-Path -LiteralPath $resourceSource -PathType Container)) {
    throw "The Release resource directory was not produced: $resourceSource"
}

$requiredResources = @(
    "background.png",
    "faces.png",
    "about.png",
    "DelayLama-Original-Manual.pdf"
)
foreach ($resource in $requiredResources) {
    Require-File -Path (Join-Path $resourceSource $resource)
}

New-Item -ItemType Directory -Path $stageRoot -Force | Out-Null
Copy-Item -LiteralPath $executablePath -Destination (Join-Path $stageRoot "DelayLamaStandalone.exe")
Copy-Item -LiteralPath $resourceSource -Destination (Join-Path $stageRoot "Resources") -Recurse
Copy-Item -LiteralPath (Join-Path $windowsRoot "..\LICENSE") -Destination (Join-Path $stageRoot "LICENSE")
Copy-Item -LiteralPath (Join-Path $windowsRoot "..\MONKSYNTH-LICENSE.txt") -Destination (Join-Path $stageRoot "MONKSYNTH-LICENSE.txt")
Copy-Item -LiteralPath (Join-Path $windowsRoot "README.md") -Destination (Join-Path $stageRoot "README-Windows.md")

Compress-Archive -LiteralPath $stageRoot -DestinationPath $zipPath -CompressionLevel Optimal -Force
Require-File -Path $zipPath

$hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
$checksumLine = "{0}  {1}" -f $hash, (Split-Path -Leaf $zipPath)
Set-Content -LiteralPath $checksumPath -Value $checksumLine -Encoding ascii -NoNewline
Require-File -Path $checksumPath

Write-Host "Created package: $zipPath"
Write-Host "Created checksum: $checksumPath"
Write-Host $checksumLine
