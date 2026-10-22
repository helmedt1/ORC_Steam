# Packs the release: orcsteam-<version>.zip with the installer, both readmes and build\orcsteam\xlive.dll.
# (No Cyrillic literals here: Windows PowerShell 5.1 reads BOM-less scripts as ANSI.)
param([string]$Version = '1.0')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$d = "$root\dist"
New-Item -ItemType Directory -Force "$d\orcsteam" | Out-Null
Get-ChildItem "$d\orcsteam" | Remove-Item
Copy-Item "$root\build\orcsteam\xlive.dll" "$d\orcsteam\xlive.dll"

# Batch files need CRLF and UTF-8 without BOM (chcp 65001); the Russian readme keeps a BOM for Notepad.
$files = Get-ChildItem $d -File | Where-Object { $_.Extension -in '.bat', '.txt' }
foreach ($f in $files) {
    $russian = $f.Extension -eq '.txt' -and $f.Name -ne 'README.txt'
    $text = [IO.File]::ReadAllText($f.FullName) -replace "`r?`n", "`r`n"
    [IO.File]::WriteAllText($f.FullName, $text, (New-Object Text.UTF8Encoding($russian)))
}

$zip = "$root\orcsteam-$Version.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (@($files.FullName) + "$d\orcsteam") -DestinationPath $zip
Get-Item $zip | Select-Object Name, Length
