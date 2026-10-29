# Builds orcsteam (x86) into build\orcsteam\xlive.dll, and the self-test into build\selftest.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$src = "$root\src\orcsteam"
$out = "$root\build\orcsteam"
New-Item -ItemType Directory -Force $out | Out-Null

# Export exactly the xlive ordinals RaccoonCity.exe imports, by ordinal only (like the real xlive.dll).
# Read from your own copy of the game (backup\RaccoonCity.exe if present, else the installed game).
$exe = "$root\backup\RaccoonCity.exe"
if (-not (Test-Path $exe)) { $exe = "$(Get-GamePath)\RaccoonCity.exe" }
$ords = & "$root\tools\pe.ps1" -Path $exe | Where-Object { $_ -like 'xlive.dll*' } |
    ForEach-Object { [int](($_ -split '#')[1]) } | Sort-Object -Unique
$ords | ForEach-Object { "X($_)" } | Set-Content "$out\ordinals.inc" -Encoding ascii
@('LIBRARY xlive', 'EXPORTS') + ($ords | ForEach-Object { "    Stub_$_ @$_ NONAME" }) | Set-Content "$out\xlive.def" -Encoding ascii

. "$root\tools\env.ps1"
$devcmd = "$(Get-VsPath)\Common7\Tools\VsDevCmd.bat"
$sources = 'core', 'overrides', 'sessions', 'steam', 'net', 'invites', 'compat', 'voice', 'spatial', 'unlock', 'tweaks' | ForEach-Object { "`"$src\$_.cpp`"" }
# MinHook (BSD, third_party\minhook) for the Wwise hooks in spatial.cpp
$mh = "$root\third_party\minhook\src"
$sources += 'hook.c', 'buffer.c', 'trampoline.c', 'hde\hde32.c' | ForEach-Object { "`"$mh\$_`"" }
$rc = "rc /nologo /fo `"$out\orcsteam.res`" `"$src\orcsteam.rc`""
$cl = "cl /nologo /O2 /MT /W3 /EHs-c- /I`"$out`" /Fo`"$out\\`" /Fe`"$out\xlive.dll`" $($sources -join ' ') /LD /link ws2_32.lib ole32.lib user32.lib `"$out\orcsteam.res`" /DEF:`"$out\xlive.def`" /PDB:`"$out\xlive.pdb`" /DEBUG /INCREMENTAL:NO /OPT:REF"
cmd /c "`"$devcmd`" -arch=x86 -host_arch=x64 -no_logo 2>nul && $rc && $cl"
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Write-Host "built $out\xlive.dll ($($ords.Count) ordinals)"

$test = "$root\build\selftest"
New-Item -ItemType Directory -Force $test | Out-Null
Copy-Item "$out\xlive.dll" $test -Force
cmd /c "`"$devcmd`" -arch=x86 -host_arch=x64 -no_logo 2>nul && cl /nologo /O1 /W3 /I`"$src`" /Fo`"$test\\`" /Fe`"$test\selftest.exe`" `"$src\selftest.cpp`" >nul"
if ($LASTEXITCODE -ne 0) { throw "selftest build failed ($LASTEXITCODE)" }
Write-Host "built $test\selftest.exe"
