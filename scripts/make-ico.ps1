# make-ico.ps1 - build a multi-size Windows .ico from a source PNG using
# System.Drawing (no ImageMagick dependency). PNG-compressed entries are
# valid for all sizes since Windows Vista.
#
# Usage: pwsh scripts/make-ico.ps1 [source.png] [output.ico]
#
# This file is part of AlienFX-Lights and is distributed under the GNU GPLv3.

param(
    [string]$Source = "assets/appicon-1024.png",
    [string]$Output = "src/windows/AlienFXLights.ico"
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

if (-not (Test-Path $Source)) {
    throw "Source image not found: $Source"
}

$sizes = 256, 64, 48, 40, 32, 24, 20, 16
$src = [System.Drawing.Image]::FromFile((Resolve-Path $Source))
try {
    $pngBlobs = foreach ($size in $sizes) {
        $bmp = New-Object System.Drawing.Bitmap($size, $size)
        $gfx = [System.Drawing.Graphics]::FromImage($bmp)
        $gfx.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $gfx.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $gfx.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $gfx.DrawImage($src, 0, 0, $size, $size)
        $gfx.Dispose()
        $ms = New-Object System.IO.MemoryStream
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        , $ms.ToArray()
    }
} finally {
    $src.Dispose()
}

# Assemble the ICO container: ICONDIR + ICONDIRENTRY per image + PNG payloads.
$out = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($out)
$writer.Write([uint16]0)               # reserved
$writer.Write([uint16]1)               # type: icon
$writer.Write([uint16]$sizes.Count)    # image count

$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $size = $sizes[$i]
    $blob = $pngBlobs[$i]
    $writer.Write([byte]($size -band 0xff))  # width (0 means 256)
    $writer.Write([byte]($size -band 0xff))  # height
    $writer.Write([byte]0)                   # palette colors
    $writer.Write([byte]0)                   # reserved
    $writer.Write([uint16]1)                 # color planes
    $writer.Write([uint16]32)                # bits per pixel
    $writer.Write([uint32]$blob.Length)      # payload size
    $writer.Write([uint32]$offset)           # payload offset
    $offset += $blob.Length
}
foreach ($blob in $pngBlobs) {
    $writer.Write($blob)
}
$writer.Flush()

$outPath = Join-Path (Get-Location) $Output
[System.IO.File]::WriteAllBytes($outPath, $out.ToArray())
$writer.Dispose()
Write-Host "Wrote $Output ($($sizes -join ', ') px)"
