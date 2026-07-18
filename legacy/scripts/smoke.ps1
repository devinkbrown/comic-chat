[CmdletBinding()]
param(
    [string]$PackageDirectory,
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [ValidateRange(1, 60)]
    [int]$RunSeconds = 8,
    [switch]$ValidateOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$legacyRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $PackageDirectory) {
    $PackageDirectory = Join-Path $legacyRoot 'out'
}
elseif (-not [IO.Path]::IsPathRooted($PackageDirectory)) {
    $PackageDirectory = Join-Path $legacyRoot $PackageDirectory
}
$PackageDirectory = [IO.Path]::GetFullPath($PackageDirectory)

$packageSuffix = if ($Configuration -eq 'Release') { 'win32' } else { 'win32-debug' }
$packageName = "ComicChat-2.5-beta-1-legacy-port-$packageSuffix"
$zipPath = Join-Path $PackageDirectory "$packageName.zip"
$hashFile = Join-Path $PackageDirectory 'SHA256SUMS.txt'
if (-not (Test-Path -LiteralPath $zipPath)) {
    throw "Package is missing: $zipPath"
}
if (-not (Test-Path -LiteralPath $hashFile)) {
    throw "Checksum file is missing: $hashFile"
}

$checksumMatches = @(
    foreach ($line in Get-Content -LiteralPath $hashFile) {
        $match = [regex]::Match($line, '^([0-9a-fA-F]{64})  ([^\r\n]+)$')
        if ($match.Success -and $match.Groups[2].Value -ceq "$packageName.zip") {
            $match
        }
    }
)
if ($checksumMatches.Count -ne 1) {
    throw "SHA256SUMS.txt must contain exactly one checksum for $packageName.zip."
}
$expectedHash = $checksumMatches[0].Groups[1].Value.ToLowerInvariant()
$actualHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualHash -ne $expectedHash) {
    throw "Package checksum mismatch: expected $expectedHash, found $actualHash"
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) "ComicChat legacy smoke $([guid]::NewGuid())"
$unrelatedWorkingDirectory = Join-Path ([IO.Path]::GetTempPath()) "Unrelated ComicChat cwd $([guid]::NewGuid())"
try {
    New-Item -ItemType Directory -Path $testRoot, $unrelatedWorkingDirectory -Force | Out-Null
    Expand-Archive -LiteralPath $zipPath -DestinationPath $testRoot
    $packageRoot = Join-Path $testRoot $packageName
    $executable = Join-Path $packageRoot 'CChat.exe'

    foreach ($required in @(
        $executable,
        (Join-Path $packageRoot 'ComicArt'),
        (Join-Path $packageRoot 'cchat.hlp'),
        (Join-Path $packageRoot 'cchat.cnt'),
        (Join-Path $packageRoot 'LICENSE.txt'),
        (Join-Path $packageRoot 'NOTICE.txt'),
        (Join-Path $packageRoot 'PROVENANCE.md'),
        (Join-Path $packageRoot 'UNOFFICIAL-BUILD.txt')
    )) {
        if (-not (Test-Path -LiteralPath $required)) {
            throw "Packaged runtime input is missing: $required"
        }
    }

    if (-not (Get-ChildItem -LiteralPath (Join-Path $packageRoot 'ComicArt') -Filter '*.avb' -File)) {
        throw 'Package contains no AVB character art.'
    }
    if (-not (Get-ChildItem -LiteralPath (Join-Path $packageRoot 'ComicArt') -Filter '*.bgb' -File)) {
        throw 'Package contains no BGB backdrop art.'
    }

    . (Join-Path $PSScriptRoot 'common.ps1')
    Assert-ComicChatX86Pe -LiteralPath $executable

    if ($ValidateOnly) {
        Write-Host 'PASS: checksum, archive layout, runtime inputs, bundled art, and x86 PE image are valid.'
        return
    }

    $process = $null
    try {
        $process = Start-Process -FilePath $executable -WorkingDirectory $unrelatedWorkingDirectory -PassThru
        $deadline = [DateTime]::UtcNow.AddSeconds($RunSeconds)
        $sawWindow = $false
        while ([DateTime]::UtcNow -lt $deadline) {
            Start-Sleep -Milliseconds 500
            $process.Refresh()
            if ($process.HasExited) {
                throw "CChat.exe exited during smoke testing with code $($process.ExitCode)."
            }
            if ($process.MainWindowHandle -ne [IntPtr]::Zero) {
                $sawWindow = $true
            }
        }
        if (-not $sawWindow) {
            throw 'CChat.exe remained alive but did not create a top-level window.'
        }
        Write-Host "PASS: displayed a window and remained running for $RunSeconds seconds."
    }
    finally {
        if ($null -ne $process) {
            $process.Refresh()
            if (-not $process.HasExited) {
                Stop-Process -Id $process.Id
                $process.WaitForExit(5000) | Out-Null
            }
            $process.Dispose()
        }
    }
}
finally {
    Remove-Item -LiteralPath $testRoot, $unrelatedWorkingDirectory -Recurse -Force -ErrorAction SilentlyContinue
}
