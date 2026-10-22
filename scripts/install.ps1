# Developer install: puts build\orcsteam\xlive.dll into the game folder, the way dist\install.bat does.
# The first install keeps the game's own xlive.dll as xlive.dll.orcsteam-backup; -Uninstall puts it back.
param([switch]$Uninstall, [string]$Game)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
. "$root\tools\env.ps1"
if (-not $Game) { $Game = Get-GamePath }
if (Get-Process RaccoonCity -ErrorAction SilentlyContinue) { throw 'the game is running' }
$backup = "$Game\xlive.dll.orcsteam-backup"

# Leftovers of the test builds, which loaded the old stub as xlive_orig.dll and logged to orcsteam_xlive.log.
foreach ($old in 'xlive_orig.dll', 'orcsteam_xlive.log') { if (Test-Path "$Game\$old") { Remove-Item "$Game\$old" } }

if ($Uninstall) {
    if (-not (Test-Path $backup)) { throw "no backup in $Game" }
    if ((Get-Item $backup).Length -eq 0) { Remove-Item "$Game\xlive.dll" -ErrorAction SilentlyContinue }
    else { Copy-Item $backup "$Game\xlive.dll" -Force }
    Remove-Item $backup
    Write-Host "restored the original xlive.dll in $Game"
    return
}
if (-not (Test-Path $backup)) {
    if (Test-Path "$Game\xlive.dll") { Copy-Item "$Game\xlive.dll" $backup }
    else { New-Item -ItemType File $backup | Out-Null }  # empty: there was none
}
Copy-Item "$root\build\orcsteam\xlive.dll" "$Game\xlive.dll" -Force
Write-Host "installed orcsteam into $Game; log: $Game\orcsteam.log"
