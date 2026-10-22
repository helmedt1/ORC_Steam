param([string]$Path, [switch]$Exports)
# Minimal PE32 parser: lists imports (dll -> name/ordinal) or exports.
$b = [IO.File]::ReadAllBytes($Path)
function U16($o) { [BitConverter]::ToUInt16($b, $o) }
function U32($o) { [BitConverter]::ToUInt32($b, $o) }
function Str($o) { $e = $o; while ($b[$e] -ne 0) { $e++ }; [Text.Encoding]::ASCII.GetString($b, $o, $e - $o) }
$pe = U32 0x3C
$nsec = U16 ($pe + 6)
$optsz = U16 ($pe + 20)
$opt = $pe + 24
if ((U16 $opt) -ne 0x10B) { throw "Not PE32" }
$secs = @()
$so = $opt + $optsz
for ($i = 0; $i -lt $nsec; $i++) {
    $o = $so + $i * 40
    $secs += [pscustomobject]@{ Name = ([Text.Encoding]::ASCII.GetString($b, $o, 8)).Trim([char]0); VA = U32 ($o + 12); VSize = U32 ($o + 8); Raw = U32 ($o + 20); RawSize = U32 ($o + 16) }
}
function R2O($rva) { foreach ($s in $secs) { if ($rva -ge $s.VA -and $rva -lt $s.VA + [Math]::Max($s.VSize, $s.RawSize)) { return $rva - $s.VA + $s.Raw } }; throw "rva $rva" }
$dd = $opt + 96
if ($Exports) {
    $erva = U32 $dd
    if ($erva -eq 0) { "no exports"; return }
    $e = R2O $erva
    $base = U32 ($e + 16); $nfun = U32 ($e + 20); $nnam = U32 ($e + 24)
    $fun = R2O (U32 ($e + 28))
    $names = @{}
    if ($nnam) { $nam = R2O (U32 ($e + 32)); $ord = R2O (U32 ($e + 36)) }
    for ($i = 0; $i -lt $nnam; $i++) { $names[[int](U16 ($ord + 2 * $i))] = Str (R2O (U32 ($nam + 4 * $i))) }
    for ($i = 0; $i -lt $nfun; $i++) { if ((U32 ($fun + 4 * $i)) -ne 0) { "{0}`t{1}" -f ($base + $i), $names[$i] } }
    return
}
$irva = U32 ($dd + 8)
$d = R2O $irva
while ($true) {
    $oft = U32 $d; $nameRva = U32 ($d + 12); $ft = U32 ($d + 16)
    if ($nameRva -eq 0) { break }
    $dll = Str (R2O $nameRva)
    $t = R2O ($(if ($oft) { $oft } else { $ft }))
    while (($v = U32 $t) -ne 0) {
        if ($v -band 0x80000000) { "{0}`t#{1}" -f $dll, ($v -band 0xFFFF) } else { "{0}`t{1}" -f $dll, (Str ((R2O $v) + 2)) }
        $t += 4
    }
    $d += 20
}
