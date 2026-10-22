# Checks how many argument bytes each xlive-dummy export pops (first ret N) against the GFWL signature.
# A mismatch means the stub corrupts the caller's stack: such functions must be implemented by us.
param([string]$Dll = "$PSScriptRoot\..\backup\xlive_dummy.dll")
. "$PSScriptRoot\env.ps1"
$db = Get-Dumpbin

# Ordinals RaccoonCity.exe imports and still forwards to the stub -> expected stdcall argument bytes.
$expected = @{
    3 = 12; 4 = 4; 5 = 8; 6 = 12; 7 = 20; 8 = 20; 9 = 12; 11 = 12; 12 = 12; 13 = 8; 14 = 12; 15 = 20
    18 = 16; 22 = 16; 26 = 4; 51 = 4; 62 = 12; 69 = 20
    651 = 16; 5001 = 4; 5002 = 0; 5007 = 4; 5008 = 12; 5030 = 4
    5034 = 20; 5035 = 20; 5036 = 8; 5038 = 4
    5209 = 16; 5210 = 12; 5215 = 4; 5252 = 12; 5254 = 4; 5256 = 20; 5260 = 8; 5270 = 8
    5278 = 12; 5284 = 28; 5286 = 32; 5297 = 8; 5310 = 0; 5311 = 0; 5312 = 20; 5314 = 16
    5315 = 8; 5316 = 20; 5324 = 0; 5331 = 28; 5339 = 36; 5365 = 20
}

$bytes = [IO.File]::ReadAllBytes($Dll)
$pe = [BitConverter]::ToInt32($bytes, 0x3C)
$base = [BitConverter]::ToUInt32($bytes, $pe + 24 + 28)
$rvas = @{}
& $db /nologo /exports $Dll | ForEach-Object {
    if ($_ -match '^\s+(\d+)\s+([0-9A-F]{8})\s+\[NONAME\]') { $rvas[[int]$matches[1]] = [Convert]::ToUInt32($matches[2], 16) }
}

foreach ($ord in ($expected.Keys | Sort-Object)) {
    if (-not $rvas.ContainsKey($ord)) { "{0,5}  missing" -f $ord; continue }
    $va = $base + $rvas[$ord]
    $dis = & $db /nologo /disasm:nobytes ("/range:0x{0:X},0x{1:X}" -f $va, ($va + 0x400)) $Dll
    $first = $dis | Where-Object { $_ -match '^\s+[0-9A-F]{8}: ret' } | Select-Object -First 1
    $pops = if ($first -match 'ret\s+([0-9A-F]+)h?\s*$') { [Convert]::ToInt32($matches[1].TrimEnd('h'), 16) }
            elseif ($first -match 'ret\s*$') { 0 } else { $null }
    $verdict = if ($null -eq $pops) { "?    $($first.Trim())" } elseif ($pops -eq $expected[$ord]) { "ok" } else { "MISMATCH" }
    "{0,5}  expect {1,2}  stub {2,2}  {3}" -f $ord, $expected[$ord], $pops, $verdict
}
