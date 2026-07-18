[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [string]$OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$legacyRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$sourceRoot = Join-Path $legacyRoot 'source'
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $legacyRoot 'out'
}
elseif (-not [IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $legacyRoot $OutputDirectory
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)

$pathComparison = if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
    [StringComparison]::OrdinalIgnoreCase
}
else {
    [StringComparison]::Ordinal
}
$sourcePrefix = $sourceRoot.TrimEnd(
    [IO.Path]::DirectorySeparatorChar,
    [IO.Path]::AltDirectorySeparatorChar
) + [IO.Path]::DirectorySeparatorChar
if ($OutputDirectory.Equals($sourceRoot, $pathComparison) -or
    $OutputDirectory.StartsWith($sourcePrefix, $pathComparison)) {
    throw 'OutputDirectory must be outside the pinned legacy/source tree.'
}

$packageSuffix = if ($Configuration -eq 'Release') { 'win32' } else { 'win32-debug' }
$packageName = "ComicChat-2.5-beta-1-legacy-port-$packageSuffix"
$executable = Join-Path (Join-Path $sourceRoot $Configuration) 'CChat.exe'
$artSource = Join-Path $sourceRoot 'comicart'

foreach ($required in @(
    $executable,
    $artSource,
    (Join-Path $legacyRoot 'LICENSE'),
    (Join-Path $legacyRoot 'NOTICE.txt'),
    (Join-Path $legacyRoot 'PROVENANCE.md'),
    (Join-Path $sourceRoot 'cchat.hlp'),
    (Join-Path $sourceRoot 'cchat.cnt')
)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required package input is missing: $required"
    }
}

. (Join-Path $PSScriptRoot 'common.ps1')
Assert-ComicChatX86Pe -LiteralPath $executable

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$stagingRoot = Join-Path $OutputDirectory ('.comicchat-staging-{0}' -f [guid]::NewGuid().ToString('N'))
$packageRoot = Join-Path $stagingRoot $packageName

try {
    New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null
    Copy-Item -LiteralPath $executable -Destination (Join-Path $packageRoot 'CChat.exe')
    Copy-Item -LiteralPath $artSource -Destination (Join-Path $packageRoot 'ComicArt') -Recurse

    foreach ($name in @('cchat.hlp', 'cchat.cnt', 'readme.gif', 'readme.htm', 'readme.txt')) {
        $path = Join-Path $sourceRoot $name
        if (Test-Path -LiteralPath $path) {
            Copy-Item -LiteralPath $path -Destination $packageRoot
        }
    }

    $sidecarManifest = "$executable.manifest"
    if (Test-Path -LiteralPath $sidecarManifest) {
        Copy-Item -LiteralPath $sidecarManifest -Destination (Join-Path $packageRoot 'CChat.exe.manifest')
    }

    Copy-Item -LiteralPath (Join-Path $legacyRoot 'LICENSE') -Destination (Join-Path $packageRoot 'LICENSE.txt')
    Copy-Item -LiteralPath (Join-Path $legacyRoot 'NOTICE.txt') -Destination $packageRoot
    Copy-Item -LiteralPath (Join-Path $legacyRoot 'PROVENANCE.md') -Destination $packageRoot
    Copy-Item -LiteralPath (Join-Path $legacyRoot 'README.md') -Destination $packageRoot
    Copy-Item -LiteralPath (Join-Path $sourceRoot 'README.md') -Destination (Join-Path $packageRoot 'UPSTREAM-PORT-NOTES.md')

    @(
        'UNOFFICIAL, UNSIGNED, UNSUPPORTED ARCHIVAL BUILD'
        ''
        'Microsoft Chat 2.5 beta 1 legacy port (Windows x86)'
        "Configuration: $Configuration"
        'Upstream: https://github.com/microsoft/comic-chat'
        'Revision: c7df00f60bc8e9fdef413f139e61f7c37e024684'
        "Packaged (UTC): $([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'))"
        ''
        'This is not an official Microsoft product release.'
        'Extract the complete directory before running CChat.exe.'
        'IRC transport is plaintext; use a trusted local TLS tunnel or bouncer.'
        'Do not accept unsolicited DCC/file transfers.'
    ) | Set-Content -LiteralPath (Join-Path $packageRoot 'UNOFFICIAL-BUILD.txt') -Encoding ascii

    $zipPath = Join-Path $OutputDirectory "$packageName.zip"
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    Compress-Archive -LiteralPath $packageRoot -DestinationPath $zipPath -CompressionLevel Optimal

    $hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $hashLines = Get-ChildItem -LiteralPath $OutputDirectory -Filter '*.zip' -File |
        Sort-Object Name |
        ForEach-Object {
            $itemHash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            "$itemHash  $($_.Name)"
        }
    $hashLines | Set-Content -LiteralPath (Join-Path $OutputDirectory 'SHA256SUMS.txt') -Encoding ascii
    Write-Host "Created $zipPath"
    Write-Host "SHA-256: $hash"
}
finally {
    Remove-Item -LiteralPath $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
}
