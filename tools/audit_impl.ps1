# Checks that every orcsteam implementation pops exactly the GFWL argument size (stdcall ret N).
# A wrong size corrupts the game's stack (that is how the old stub's #62 and #5316 crashed it).
# Needs a fresh build (scripts/build.ps1): the self-test prints ordinal -> implementation RVA.
$root = Split-Path $PSScriptRoot -Parent
$dll = "$root\build\orcsteam\xlive.dll"
. "$PSScriptRoot\env.ps1"
$db = Get-Dumpbin

# Argument bytes from the GFWL signatures (XUID/ULONGLONG = 8).
$expected = @{
    3 = 12; 4 = 4; 5 = 8; 6 = 12; 7 = 20; 8 = 20; 9 = 12; 11 = 12; 12 = 12; 13 = 8; 14 = 12; 15 = 20; 18 = 16
    20 = 24; 22 = 16; 24 = 24; 25 = 36; 26 = 4; 51 = 4; 55 = 8; 57 = 12; 60 = 12; 62 = 12; 69 = 20; 70 = 48; 72 = 4; 73 = 4
    651 = 16; 1082 = 4; 1083 = 12; 5001 = 4; 5002 = 0; 5003 = 0; 5007 = 4; 5008 = 12; 5030 = 4
    5034 = 20; 5035 = 20; 5036 = 8; 5038 = 4; 5209 = 16; 5210 = 12; 5215 = 4; 5251 = 4; 5252 = 12; 5254 = 4
    5256 = 20; 5260 = 8; 5262 = 4; 5263 = 12; 5265 = 12; 5267 = 12; 5270 = 8; 5276 = 16; 5277 = 12; 5278 = 12
    5284 = 28; 5286 = 32; 5293 = 20; 5297 = 8; 5300 = 32; 5310 = 0; 5311 = 0; 5312 = 20; 5314 = 16; 5315 = 8
    5316 = 20; 5317 = 24; 5318 = 12; 5321 = 40; 5322 = 20; 5323 = 16; 5324 = 0; 5325 = 16; 5326 = 20; 5327 = 20
    5330 = 8; 5331 = 28; 5332 = 8; 5336 = 16; 5339 = 36; 5365 = 20
}

$b = [IO.File]::ReadAllBytes($dll)
$pe = [BitConverter]::ToInt32($b, 0x3C)
$base = [BitConverter]::ToUInt32($b, $pe + 24 + 28)

Push-Location "$root\build\selftest"
$targets = @{}
.\selftest.exe targets | ForEach-Object { $p = $_ -split ' '; $targets[[int]$p[0]] = [Convert]::ToUInt32($p[1], 16) }
Pop-Location

$bad = 0
foreach ($ord in ($expected.Keys | Sort-Object)) {
    $pops = $null
    if ($targets.ContainsKey($ord)) {
        $va = $base + $targets[$ord]
        $dis = & $db /nologo /disasm:nobytes ("/range:0x{0:X},0x{1:X}" -f $va, ($va + 0x1000)) $dll
        $first = $dis | Where-Object { $_ -match '^\s+[0-9A-F]{8}: ret' } | Select-Object -First 1
        if ($first -match 'ret\s+([0-9A-F]+)h?\s*$') { $pops = [Convert]::ToInt32($matches[1].TrimEnd('h'), 16) }
        elseif ($first -match 'ret\s*$') { $pops = 0 }
    }
    $ok = $null -ne $pops -and $pops -eq $expected[$ord]
    if (-not $ok) { $bad++ }
    "{0,5}  expect {1,2}  ours {2,2}  {3}" -f $ord, $expected[$ord], $pops, $(if ($ok) { 'ok' } else { 'MISMATCH' })
}
"exports checked: $($expected.Count) of $($targets.Count), mismatches: $bad"
exit $bad
