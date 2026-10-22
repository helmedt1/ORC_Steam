# Shared helpers for the scripts: where Visual Studio, dumpbin and the game are on this machine.
# Dot-source it:  . "$PSScriptRoot\..\tools\env.ps1"

function Get-VsPath {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw 'Visual Studio not found (no vswhere.exe)' }
    $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $path) { throw 'Visual Studio with the C++ x86/x64 tools not found' }
    $path
}

function Get-Dumpbin {
    Get-ChildItem "$(Get-VsPath)\VC\Tools\MSVC" -Recurse -Filter dumpbin.exe |
        Where-Object { $_.FullName -match 'Hostx64\\x86' } | Select-Object -First 1 -ExpandProperty FullName
}

# The game folder: searched in every Steam library listed in libraryfolders.vdf.
function Get-GamePath {
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if (-not $steam) { throw 'Steam not found in the registry; pass the game folder explicitly' }
    $libraries = @($steam -replace '/', '\')
    $vdf = "$steam\steamapps\libraryfolders.vdf"
    if (Test-Path $vdf) {
        $libraries += [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"') | ForEach-Object { $_.Groups[1].Value -replace '\\\\', '\' }
    }
    foreach ($lib in $libraries | Select-Object -Unique) {
        $game = Join-Path $lib 'steamapps\common\Resident Evil Operation Raccoon City'
        if (Test-Path "$game\RaccoonCity.exe") { return $game }
    }
    throw 'Resident Evil: Operation Raccoon City not found in any Steam library; pass the game folder explicitly'
}
