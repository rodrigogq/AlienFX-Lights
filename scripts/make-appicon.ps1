# make-appicon.ps1 - render the app's icon: a simple, generic alien-head
# silhouette, fully white with transparent eye cut-outs and a transparent
# background (macOS-style filled glyph). Drawn from scratch with GDI+ - no
# third-party or trademarked art. Deliberately rounded/generic, distinct
# from any specific vendor logo. The same glyph is the app icon and the
# tray / menu-bar icon.
#
# Outputs:
#   Resources/appicon-1024.png  - white silhouette, transparent background
#   Resources/menuicon.png      - same glyph, small (macOS template / tray)
#
# Run make-ico.ps1 / make-icns.sh afterwards to regenerate the .ico / .icns.
#
# This file is part of AlienFX-Lights and is distributed under the GNU GPLv3.

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$root = Split-Path $PSScriptRoot -Parent

# Add an ellipse rotated by $deg degrees about its own centre to $path.
function Add-RotatedEllipse($path, [single]$cx, [single]$cy, [single]$rw, [single]$rh, [single]$deg) {
    $ep = New-Object System.Drawing.Drawing2D.GraphicsPath
    $ep.AddEllipse((-$rw), (-$rh), ($rw * 2), ($rh * 2))
    $m = New-Object System.Drawing.Drawing2D.Matrix
    $m.Translate($cx, $cy)
    $m.Rotate($deg)
    $ep.Transform($m)
    $path.AddPath($ep, $false)
}

function Render-Alien([int]$size, [string]$path) {
    $b = New-Object System.Drawing.Bitmap($size, $size)
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::FromArgb(0, 0, 0, 0)) # transparent

    $s = [single]$size
    # Head: smooth closed curve - wide rounded crown, tapering to a soft chin.
    $head = New-Object System.Drawing.Drawing2D.GraphicsPath
    [System.Drawing.PointF[]]$pts = @(
        (New-Object System.Drawing.PointF(($s*0.50), ($s*0.10))),  # crown
        (New-Object System.Drawing.PointF(($s*0.87), ($s*0.36))),  # right temple
        (New-Object System.Drawing.PointF(($s*0.70), ($s*0.82))),  # right jaw
        (New-Object System.Drawing.PointF(($s*0.50), ($s*0.93))),  # chin
        (New-Object System.Drawing.PointF(($s*0.30), ($s*0.82))),  # left jaw
        (New-Object System.Drawing.PointF(($s*0.13), ($s*0.36)))   # left temple
    )
    $head.AddClosedCurve($pts, [single]0.55)

    # Combine head + eyes into one region and fill with even-odd so the eyes
    # become transparent holes.
    $full = New-Object System.Drawing.Drawing2D.GraphicsPath
    $full.FillMode = [System.Drawing.Drawing2D.FillMode]::Alternate
    $full.AddPath($head, $false)
    Add-RotatedEllipse $full ($s*0.345) ($s*0.52) ($s*0.10) ($s*0.155) 22   # left eye
    Add-RotatedEllipse $full ($s*0.655) ($s*0.52) ($s*0.10) ($s*0.155) -22  # right eye

    $white = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 255, 255, 255))
    $g.FillPath($white, $full)

    $white.Dispose(); $g.Dispose()
    $b.Save((Join-Path $root $path), [System.Drawing.Imaging.ImageFormat]::Png)
    $b.Dispose()
}

Render-Alien 1024 "assets\appicon-1024.png"
Render-Alien 88   "assets\menuicon.png"

# The tiled light/dark variant is no longer used; drop it if present.
$legacy = Join-Path $root "assets\appicon-1024-light.png"
if (Test-Path $legacy) { Remove-Item $legacy }

Write-Host "Wrote appicon-1024.png and menuicon.png (white alien silhouette, transparent)"
