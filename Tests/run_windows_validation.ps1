[CmdletBinding()]
param(
    [ValidateSet('Auto', 'MSVC', 'ClangCl', 'Clang', 'GCC')]
    [string]$Compiler = 'Auto',
    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot
$dspDirectory = Join-Path $repoRoot 'DSP'
$testSource = Join-Path $PSScriptRoot 'windows_smoke.c'
$windowsBridgeSource = Join-Path $repoRoot 'Windows\standalone_bridge_win32.c'
$windowsBridgeTestSource = Join-Path $PSScriptRoot 'windows_bridge_smoke.c'

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $tempRoot = if (-not [string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
        $env:RUNNER_TEMP
    } else {
        [IO.Path]::GetTempPath()
    }
    $OutputDirectory = Join-Path $tempRoot ('delay-lama-windows-validation-' + [guid]::NewGuid().ToString('N'))
}

$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

function Get-ExecutablePath {
    param([Parameter(Mandatory = $true)][string]$Name)

    $command = Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $command) {
        return $command.Source
    }
    return $null
}

function Get-VsWherePath {
    $fromPath = Get-ExecutablePath 'vswhere.exe'
    if ($null -ne $fromPath) {
        return $fromPath
    }

    $knownLocations = @(
        'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe',
        'C:\Program Files\Microsoft Visual Studio\Installer\vswhere.exe'
    )
    foreach ($location in $knownLocations) {
        if (Test-Path -LiteralPath $location) {
            return $location
        }
    }
    return $null
}

function Import-VisualStudioEnvironment {
    $vswhere = Get-VsWherePath
    if ($null -eq $vswhere) {
        return $false
    }

    $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
        Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($installPath)) {
        return $false
    }

    $devCommand = Join-Path $installPath 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path -LiteralPath $devCommand)) {
        return $false
    }

    $commandLine = '"' + $devCommand + '" -arch=x64 -host_arch=x64 && set'
    $environmentLines = cmd.exe /d /s /c $commandLine
    if ($LASTEXITCODE -ne 0) {
        throw "VsDevCmd.bat failed with exit code $LASTEXITCODE."
    }

    foreach ($line in $environmentLines) {
        $separator = $line.IndexOf('=')
        if ($separator -le 0) {
            continue
        }
        $name = $line.Substring(0, $separator)
        $value = $line.Substring($separator + 1)
        Set-Item -Path ('Env:' + $name) -Value $value
    }
    return $true
}

function Select-Compiler {
    param([Parameter(Mandatory = $true)][string]$Requested)

    $cl = Get-ExecutablePath 'cl.exe'
    if ($null -eq $cl -and $Requested -in @('Auto', 'MSVC', 'ClangCl')) {
        [void](Import-VisualStudioEnvironment)
        $cl = Get-ExecutablePath 'cl.exe'
    }

    if ($Requested -eq 'MSVC' -or ($Requested -eq 'Auto' -and $null -ne $cl)) {
        if ($null -eq $cl) {
            throw 'MSVC was requested, but cl.exe was not found. Run from a Visual Studio Developer shell or install Visual Studio C++ build tools.'
        }
        return [pscustomobject]@{ Kind = 'MSVC'; Path = $cl }
    }

    $clangCl = Get-ExecutablePath 'clang-cl.exe'
    if ($Requested -eq 'ClangCl' -or ($Requested -eq 'Auto' -and $null -ne $clangCl)) {
        if ($null -eq $clangCl) {
            throw 'ClangCl was requested, but clang-cl.exe was not found.'
        }
        return [pscustomobject]@{ Kind = 'ClangCl'; Path = $clangCl }
    }

    $clang = Get-ExecutablePath 'clang.exe'
    if ($Requested -eq 'Clang' -or ($Requested -eq 'Auto' -and $null -ne $clang)) {
        if ($null -eq $clang) {
            throw 'Clang was requested, but clang.exe was not found.'
        }
        return [pscustomobject]@{ Kind = 'Clang'; Path = $clang }
    }

    $gcc = Get-ExecutablePath 'gcc.exe'
    if ($Requested -eq 'GCC' -or ($Requested -eq 'Auto' -and $null -ne $gcc)) {
        if ($null -eq $gcc) {
            throw 'GCC was requested, but gcc.exe was not found.'
        }
        return [pscustomobject]@{ Kind = 'GCC'; Path = $gcc }
    }

    throw 'No Windows C compiler found. windows-latest should provide MSVC; otherwise install clang-cl or MinGW and pass -Compiler explicitly.'
}

function Invoke-Compiler {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object[]]$Arguments
    )

    Write-Host ('> ' + $Path + ' ' + ($Arguments -join ' '))
    & $Path @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Compiler command failed with exit code $LASTEXITCODE."
    }
}

function Invoke-External {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object[]]$Arguments
    )

    Write-Host ('> ' + $Path + ' ' + ($Arguments -join ' '))
    & $Path @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "External command failed with exit code $LASTEXITCODE."
    }
}

$toolchain = Select-Compiler $Compiler
Write-Host ("Using {0}: {1}" -f $toolchain.Kind, $toolchain.Path)
Write-Host ("Output directory: {0}" -f $OutputDirectory)

$sourceFiles = @(
    (Join-Path $dspDirectory 'delay.c'),
    (Join-Path $dspDirectory 'synth.c'),
    (Join-Path $dspDirectory 'voice.c'),
    $testSource
)
$objectFiles = @()

foreach ($sourceFile in $sourceFiles) {
    $objectExtension = if ($toolchain.Kind -in @('MSVC', 'ClangCl')) { '.obj' } else { '.o' }
    $objectFile = Join-Path $OutputDirectory (([IO.Path]::GetFileNameWithoutExtension($sourceFile)) + $objectExtension)
    $objectFiles += $objectFile

    if ($toolchain.Kind -in @('MSVC', 'ClangCl')) {
        $compileArguments = @('/nologo', '/std:c11', '/O2', '/W4', ('/I' + $dspDirectory), '/c', $sourceFile, ('/Fo' + $objectFile))
    } else {
        $compileArguments = @('-std=c11', '-O2', '-Wall', '-Wextra', '-I', $dspDirectory, '-c', $sourceFile, '-o', $objectFile)
    }
    Invoke-Compiler $toolchain.Path $compileArguments
}

$executable = Join-Path $OutputDirectory 'windows_smoke.exe'
if ($toolchain.Kind -in @('MSVC', 'ClangCl')) {
    $linkArguments = @('/nologo') + $objectFiles + @('/Fe:' + $executable)
} else {
    $linkArguments = $objectFiles + @('-o', $executable)
    if ($toolchain.Kind -eq 'GCC') {
        $linkArguments += '-lm'
    }
}
Invoke-Compiler $toolchain.Path $linkArguments

Write-Host ('> ' + $executable)
& $executable
if ($LASTEXITCODE -ne 0) {
    throw "windows_smoke.exe failed with exit code $LASTEXITCODE."
}

if (-not (Test-Path -LiteralPath $windowsBridgeSource)) {
    throw "Windows bridge implementation was not found at $windowsBridgeSource."
}

$objectExtension = if ($toolchain.Kind -in @('MSVC', 'ClangCl')) { '.obj' } else { '.o' }
$bridgeObject = Join-Path $OutputDirectory ('standalone_bridge_win32' + $objectExtension)
$bridgeTestObject = Join-Path $OutputDirectory ('windows_bridge_smoke' + $objectExtension)

foreach ($sourceFile in @($windowsBridgeSource, $windowsBridgeTestSource)) {
    $objectFile = if ($sourceFile -eq $windowsBridgeSource) { $bridgeObject } else { $bridgeTestObject }
    if ($toolchain.Kind -in @('MSVC', 'ClangCl')) {
        $compileArguments = @('/nologo', '/std:c11', '/O2', '/W4', ('/I' + $dspDirectory), ('/I' + (Join-Path $repoRoot 'Sources')), '/c', $sourceFile, ('/Fo' + $objectFile))
    } else {
        $compileArguments = @('-std=c11', '-O2', '-Wall', '-Wextra', '-I', $dspDirectory, '-I', (Join-Path $repoRoot 'Sources'), '-c', $sourceFile, '-o', $objectFile)
    }
    Invoke-Compiler $toolchain.Path $compileArguments
}

$bridgeExecutable = Join-Path $OutputDirectory 'windows_bridge_smoke.exe'
if ($toolchain.Kind -in @('MSVC', 'ClangCl')) {
    $bridgeLinkArguments = @('/nologo') + $objectFiles[0..2] + @($bridgeObject, $bridgeTestObject, ('/Fe:' + $bridgeExecutable), '/link', 'winmm.lib')
} else {
    $bridgeLinkArguments = $objectFiles[0..2] + @($bridgeObject, $bridgeTestObject, '-o', $bridgeExecutable, '-lwinmm')
    if ($toolchain.Kind -eq 'GCC') {
        $bridgeLinkArguments += '-lm'
    }
}
Invoke-Compiler $toolchain.Path $bridgeLinkArguments

Write-Host ('> ' + $bridgeExecutable)
& $bridgeExecutable
if ($LASTEXITCODE -ne 0) {
    throw "windows_bridge_smoke.exe failed with exit code $LASTEXITCODE."
}

$windowsCMakeDirectory = Join-Path $repoRoot 'Windows'
$windowsCMakeLists = Join-Path $windowsCMakeDirectory 'CMakeLists.txt'
$cmake = Get-ExecutablePath 'cmake.exe'
$ctest = Get-ExecutablePath 'ctest.exe'
if (-not (Test-Path -LiteralPath $windowsCMakeLists)) {
    throw "Windows CMake project was not found at $windowsCMakeLists."
}
if ($null -eq $cmake -or $null -eq $ctest) {
    throw 'CMake and ctest are required for the Windows application build check.'
}

$cmakeBuildDirectory = Join-Path $OutputDirectory 'cmake-build'
Invoke-External $cmake @('-S', $windowsCMakeDirectory, '-B', $cmakeBuildDirectory, '-A', 'x64')
Invoke-External $cmake @('--build', $cmakeBuildDirectory, '--config', 'Release', '--target', 'DelayLamaStandalone', 'delay_lama_dsp_smoke', '--parallel')
Invoke-External $ctest @('--test-dir', $cmakeBuildDirectory, '-C', 'Release', '--output-on-failure')

Write-Host 'Windows application build, DSP compilation, linking, and executable smoke tests passed.'
