# Draws the orcsteam guide/repository logo (original artwork, no third-party marks) into docs\logo.png.
param([int]$Size = 512)
Add-Type -AssemblyName System.Drawing
$root = Split-Path $PSScriptRoot -Parent
$out = "$root\docs\logo.png"

$bmp = New-Object System.Drawing.Bitmap $Size, $Size
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = 'AntiAlias'
$g.TextRenderingHint = 'AntiAliasGridFit'
$s = $Size / 512.0

# Background: near-black with a dark red glow from the top.
$rect = New-Object System.Drawing.Rectangle 0, 0, $Size, $Size
$bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, ([System.Drawing.Color]::FromArgb(255, 70, 6, 10)), ([System.Drawing.Color]::FromArgb(255, 10, 10, 12)), 90
$g.FillRectangle($bg, $rect)

# Network motif: three nodes joined by lines (peer-to-peer co-op).
$red = [System.Drawing.Color]::FromArgb(255, 220, 30, 40)
$line = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(180, 220, 30, 40)), (6 * $s)
$nodes = @(@(256, 96), @(150, 190), @(362, 190))
$g.DrawLine($line, $nodes[0][0] * $s, $nodes[0][1] * $s, $nodes[1][0] * $s, $nodes[1][1] * $s)
$g.DrawLine($line, $nodes[0][0] * $s, $nodes[0][1] * $s, $nodes[2][0] * $s, $nodes[2][1] * $s)
$g.DrawLine($line, $nodes[1][0] * $s, $nodes[1][1] * $s, $nodes[2][0] * $s, $nodes[2][1] * $s)
$fill = New-Object System.Drawing.SolidBrush $red
$ring = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 245, 235, 235)), (5 * $s)
foreach ($n in $nodes) {
    $r = 22 * $s
    $g.FillEllipse($fill, $n[0] * $s - $r, $n[1] * $s - $r, 2 * $r, 2 * $r)
    $g.DrawEllipse($ring, $n[0] * $s - $r, $n[1] * $s - $r, 2 * $r, 2 * $r)
}

$center = New-Object System.Drawing.StringFormat
$center.Alignment = 'Center'
$center.LineAlignment = 'Center'
$white = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 245, 240, 240))
$shadow = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(160, 0, 0, 0))

# "ORC" big, with a soft shadow.
$big = New-Object System.Drawing.Font 'Impact', (150 * $s), ([System.Drawing.FontStyle]::Regular), ([System.Drawing.GraphicsUnit]::Pixel)
$g.DrawString('ORC', $big, $shadow, (New-Object System.Drawing.RectangleF (6 * $s), (236 * $s), $Size, (170 * $s)), $center)
$g.DrawString('ORC', $big, $white, (New-Object System.Drawing.RectangleF 0, (230 * $s), $Size, (170 * $s)), $center)

# Red band with "ONLINE CO-OP".
$band = New-Object System.Drawing.SolidBrush $red
$g.FillRectangle($band, 0, 392 * $s, $Size, 56 * $s)
$mid = New-Object System.Drawing.Font 'Segoe UI', (34 * $s), ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
$g.DrawString('ONLINE CO-OP', $mid, $white, (New-Object System.Drawing.RectangleF 0, (392 * $s), $Size, (56 * $s)), $center)

# Small "orcsteam" at the bottom.
$small = New-Object System.Drawing.Font 'Segoe UI', (24 * $s), ([System.Drawing.FontStyle]::Regular), ([System.Drawing.GraphicsUnit]::Pixel)
$grey = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 170, 160, 160))
$g.DrawString('orcsteam', $small, $grey, (New-Object System.Drawing.RectangleF 0, (452 * $s), $Size, (50 * $s)), $center)

$g.Dispose()
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Get-Item $out | Select-Object FullName, Length
