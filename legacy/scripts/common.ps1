function Assert-ComicChatX86Pe {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$LiteralPath
    )

    $bytes = [IO.File]::ReadAllBytes($LiteralPath)
    if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4d -or $bytes[1] -ne 0x5a) {
        throw "File has no valid DOS header: $LiteralPath"
    }

    $peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
    $coffHeaderEnd = [int64]$peOffset + 24
    if ($peOffset -lt 0 -or $coffHeaderEnd -gt $bytes.LongLength -or
        [BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x00004550) {
        throw "File has a missing or truncated PE/COFF header: $LiteralPath"
    }

    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    if ($machine -ne 0x014c) {
        throw ('Expected an x86 PE image; found machine type 0x{0:x4}: {1}' -f $machine, $LiteralPath)
    }

    $numberOfSections = [BitConverter]::ToUInt16($bytes, $peOffset + 6)
    $sizeOfOptionalHeader = [BitConverter]::ToUInt16($bytes, $peOffset + 20)
    $characteristics = [BitConverter]::ToUInt16($bytes, $peOffset + 22)
    $optionalHeaderOffset = [int64]$peOffset + 24
    $optionalHeaderEnd = $optionalHeaderOffset + $sizeOfOptionalHeader
    $sectionTableEnd = $optionalHeaderEnd + ([int64]$numberOfSections * 40)

    if ($numberOfSections -eq 0 -or $sizeOfOptionalHeader -lt 96 -or
        $optionalHeaderEnd -gt $bytes.LongLength -or $sectionTableEnd -gt $bytes.LongLength) {
        throw "File has a malformed or truncated PE image header: $LiteralPath"
    }
    if ([BitConverter]::ToUInt16($bytes, [int]$optionalHeaderOffset) -ne 0x010b) {
        throw "Expected a PE32 optional header: $LiteralPath"
    }
    if (($characteristics -band 0x0002) -eq 0 -or ($characteristics -band 0x2000) -ne 0) {
        throw "Expected an executable image rather than a DLL: $LiteralPath"
    }
}
