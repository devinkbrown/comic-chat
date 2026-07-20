$ErrorActionPreference = "Stop"
$exe = (Resolve-Path (Join-Path $PSScriptRoot "..\comicchat.exe")).Path
$command = '"' + $exe + '" "%1"'

foreach ($association in @(
    @{ Extension = ".ccc"; Class = "ComicChat.Conversation"; Description = "Comic Chat conversation" },
    @{ Extension = ".ccr"; Class = "ComicChat.Locator"; Description = "Comic Chat locator" }
)) {
    $extensionKey = "HKCU:\Software\Classes\" + $association.Extension
    $classKey = "HKCU:\Software\Classes\" + $association.Class
    New-Item -Path $extensionKey -Force | Out-Null
    Set-Item -Path $extensionKey -Value $association.Class
    New-Item -Path $classKey -Force | Out-Null
    Set-Item -Path $classKey -Value $association.Description
    New-Item -Path ($classKey + "\shell\open\command") -Force | Out-Null
    Set-Item -Path ($classKey + "\shell\open\command") -Value $command
}

Write-Host "Comic Chat .ccc and .ccr associations were installed for the current user."
