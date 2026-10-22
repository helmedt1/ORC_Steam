# Prints the xlive-dummy implementation of the given ordinals (from build/dummy/dummy_disasm.txt, with pdb symbols).
param([int[]]$Ordinals, [int]$MaxLines = 80)
$root = Split-Path $PSScriptRoot -Parent
$w = "$root\build\dummy"
. "$PSScriptRoot\env.ps1"
$db = Get-Dumpbin
$b = [IO.File]::ReadAllBytes("$w\xlive.dll")
$pe = [BitConverter]::ToInt32($b, 0x3C)
$base = [BitConverter]::ToUInt32($b, $pe + 24 + 28)
$rvas = @{}
& $db /nologo /exports "$w\xlive.dll" | ForEach-Object {
    if ($_ -match '^\s+(\d+)\s+([0-9A-F]{8})\s+\[NONAME\]') { $rvas[[int]$matches[1]] = [Convert]::ToUInt32($matches[2], 16) }
}
$lines = Get-Content "$w\dummy_disasm.txt"
$index = @{}
for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match '^\s+([0-9A-F]{8}):') { $index[$matches[1]] = $i } }
foreach ($ord in $Ordinals) {
    $va = '{0:X8}' -f ($base + $rvas[$ord])
    "===== #$ord at $va  ($($lines[$index[$va] - 1].Trim()))"
    $i = $index[$va]
    for ($n = 0; $n -lt $MaxLines -and $i -lt $lines.Count; $n++, $i++) {
        $lines[$i]
        if ($lines[$i] -match ': ret' -and $lines[$i + 1] -match 'int\s+3|^\s*$|^\S|: CC CC') { break }
    }
}
