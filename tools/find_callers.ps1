# Finds call sites of xlive ordinals in RaccoonCity.exe: IAT slot -> import thunk (jmp [slot]) -> E8 calls to it.
param([int[]]$Ordinals, [string]$Exe)
. "$PSScriptRoot\env.ps1"
if (-not $Exe) { $Exe = "$(Get-GamePath)\RaccoonCity.exe" }

Add-Type -TypeDefinition @"
using System; using System.Collections.Generic;
public static class PeScan {
    public static List<uint> FindCalls(byte[] b, uint textRaw, uint textSize, uint textVa, uint target) {
        var r = new List<uint>();
        for (uint i = textRaw; i + 5 <= textRaw + textSize; i++) {
            if (b[i] != 0xE8) continue;
            int rel = BitConverter.ToInt32(b, (int)i + 1);
            uint va = textVa + (i - textRaw);
            if ((uint)(va + 5 + rel) == target) r.Add(va);
        }
        return r;
    }
    public static List<uint> FindJmpSlot(byte[] b, uint textRaw, uint textSize, uint textVa, uint slotVa) {
        var r = new List<uint>();
        for (uint i = textRaw; i + 6 <= textRaw + textSize; i++)
            if (b[i] == 0xFF && b[i + 1] == 0x25 && BitConverter.ToUInt32(b, (int)i + 2) == slotVa) r.Add(textVa + (i - textRaw));
        return r;
    }
}
"@

$b = [IO.File]::ReadAllBytes($Exe)
$pe = [BitConverter]::ToInt32($b, 0x3C)
$base = [BitConverter]::ToUInt32($b, $pe + 24 + 28)
$nsec = [BitConverter]::ToUInt16($b, $pe + 6)
$so = $pe + 24 + [BitConverter]::ToUInt16($b, $pe + 20)
$secs = for ($i = 0; $i -lt $nsec; $i++) {
    $o = $so + $i * 40
    [pscustomobject]@{ Name = [Text.Encoding]::ASCII.GetString($b, $o, 8).Trim([char]0); VA = [BitConverter]::ToUInt32($b, $o + 12)
        VSize = [BitConverter]::ToUInt32($b, $o + 8); Raw = [BitConverter]::ToUInt32($b, $o + 20); RawSize = [BitConverter]::ToUInt32($b, $o + 16) }
}
function R2O($rva) { foreach ($s in $secs) { if ($rva -ge $s.VA -and $rva -lt $s.VA + [Math]::Max($s.VSize, $s.RawSize)) { return $rva - $s.VA + $s.Raw } } }
$text = $secs | Where-Object Name -eq '.text'

# Walk the import directory to the xlive.dll descriptor and map ordinal -> IAT slot VA.
$d = R2O ([BitConverter]::ToUInt32($b, $pe + 24 + 96 + 8))
$slots = @{}
while ($true) {
    $oft = [BitConverter]::ToUInt32($b, $d); $nameRva = [BitConverter]::ToUInt32($b, $d + 12); $ft = [BitConverter]::ToUInt32($b, $d + 16)
    if ($nameRva -eq 0) { break }
    $o = R2O $nameRva; $e = $o; while ($b[$e] -ne 0) { $e++ }
    if ([Text.Encoding]::ASCII.GetString($b, $o, $e - $o) -ieq 'xlive.dll') {
        $t = R2O ($(if ($oft) { $oft } else { $ft })); $k = 0
        while (($v = [BitConverter]::ToUInt32($b, $t)) -ne 0) { $slots[[int]($v -band 0xFFFF)] = $base + $ft + 4 * $k; $t += 4; $k++ }
    }
    $d += 20
}

foreach ($ord in $Ordinals) {
    $slot = $slots[$ord]
    $thunks = [PeScan]::FindJmpSlot($b, $text.Raw, $text.RawSize, $base + $text.VA, $slot)
    foreach ($th in $thunks) {
        $calls = [PeScan]::FindCalls($b, $text.Raw, $text.RawSize, $base + $text.VA, $th)
        "#{0}: slot 0x{1:X}, thunk 0x{2:X}, callers: {3}" -f $ord, $slot, $th, (($calls | ForEach-Object { '0x{0:X}' -f $_ }) -join ' ')
    }
}
