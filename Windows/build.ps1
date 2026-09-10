[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$windowsRoot = $PSScriptRoot
$buildRoot = Join-Path $windowsRoot "build"

$cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
if (-not $cmakeCommand) {
    throw "CMake was not found. Install CMake and the Visual Studio C++ workload, then run this script again."
}

if ($Clean -and (Test-Path -LiteralPath $buildRoot)) {
    Remove-Item -LiteralPath $buildRoot -Recurse -Force
}

& $cmakeCommand.Source -S $windowsRoot -B $buildRoot -DCMAKE_BUILD_TYPE=$Configuration
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed with exit code $LASTEXITCODE."
}

& $cmakeCommand.Source --build $buildRoot --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) {
    throw "CMake build failed with exit code $LASTEXITCODE."
}

$ctestCommand = Get-Command ctest.exe -ErrorAction SilentlyContinue
if (-not $ctestCommand) {
    throw "CTest was not found. Install CMake with its command-line tools so the Windows self-test can run."
}

& $ctestCommand.Source --test-dir $buildRoot -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) {
    throw "CTest failed with exit code $LASTEXITCODE."
}

Write-Host "Windows build and DSP smoke tests completed."
Write-Host "Build tree: $buildRoot"
