param([string]$OutputDirectory = 'artifacts/v043-content', [switch]$Measure)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (Get-Process EdgeTuck -ErrorAction SilentlyContinue) { throw 'Exit the regular EdgeTuck instance before isolated previews.' }
$output = [IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory))
[void][IO.Directory]::CreateDirectory($output)
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ContentPointer {
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT point);
}
'@
$portrait = Join-Path $output 'portrait.jpg'
$landscape = Join-Path $output 'landscape.png'
$document = Join-Path $output 'notes.txt'
foreach ($entry in @(@($portrait,240,480),@($landscape,480,240))) {
    $bitmap = [Drawing.Bitmap]::new($entry[1],$entry[2])
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $rect = [Drawing.Rectangle]::new(0,0,$bitmap.Width,$bitmap.Height)
    $gradient = [Drawing.Drawing2D.LinearGradientBrush]::new($rect,[Drawing.Color]::FromArgb(40,140,180),[Drawing.Color]::FromArgb(14,45,92),90.0)
    $graphics.FillRectangle($gradient,$rect)
    $sun = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(255,200,86))
    $graphics.FillEllipse($sun, [int]($bitmap.Width*.55),[int]($bitmap.Height*.15),[int]($bitmap.Width*.27),[int]($bitmap.Width*.27))
    $points = [Drawing.Point[]]@([Drawing.Point]::new(0,$bitmap.Height),[Drawing.Point]::new([int]($bitmap.Width*.4),[int]($bitmap.Height*.45)),[Drawing.Point]::new($bitmap.Width,$bitmap.Height))
    $mountain = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(205,228,224))
    $graphics.FillPolygon($mountain,$points)
    $format = if($entry[0] -eq $portrait) { [Drawing.Imaging.ImageFormat]::Jpeg } else { [Drawing.Imaging.ImageFormat]::Png }
    $bitmap.Save($entry[0],$format)
    $mountain.Dispose(); $sun.Dispose(); $gradient.Dispose(); $graphics.Dispose(); $bitmap.Dispose()
}
[IO.File]::WriteAllText($document,'Owned preview fixture. No desktop file is changed.')
$before = Get-FileHash -LiteralPath @($portrait,$landscape,$document) | Select-Object Path,Hash
$previousItems = $env:EDGETUCK_PREVIEW_ITEMS
$env:EDGETUCK_PREVIEW_ITEMS = @($portrait,$landscape,$document) -join "`n"
$pointer = [ContentPointer+POINT]::new()
[void][ContentPointer]::GetCursorPos([ref]$pointer)
$app = $null
try {
    $app = Start-Process -FilePath (Join-Path $repo 'build/Release/EdgeTuck.exe') -ArgumentList '--live-preview' -WindowStyle Hidden -PassThru
    Start-Sleep -Milliseconds 1600
    . (Join-Path $PSScriptRoot 'ui_probe.ps1') -Action Inspect | Out-Null
    [void][EdgeTuckProbe]::ShowWindow($control,9)
    [void][EdgeTuckProbe]::SetForegroundWindow($control)
    Invoke-EdgeControlAction 'Preview'
    Start-Sleep -Milliseconds 250
    $panel = [EdgeTuckProbe]::Windows() | Where-Object { $_.Class -eq 'EdgeTuck.Drawer' -and $_.Pid -eq $app.Id -and $_.HitWidth -gt 40 -and $_.HitHeight -gt 40 } | Select-Object -First 1
    if (!$panel) { throw 'No expanded preview drawer.' }
    function Move-Pointer([int]$x,[int]$y) {
        [void][ContentPointer]::SetCursorPos($panel.Left+[int]($x*$factor),$panel.Top+[int]($y*$factor))
        Start-Sleep -Milliseconds 350
    }
    function Capture-Panel([string]$name) {
        $center=[EdgeTuckProbe+POINT]::new(); $center.X=$panel.Left+$panel.Width/2; $center.Y=$panel.Top+$panel.Height/2
        if([EdgeTuckProbe]::WindowFromPoint($center) -ne [IntPtr]$panel.Handle) { throw 'Preview is covered; cannot validate a screen capture.' }
        $bitmap=[Drawing.Bitmap]::new($panel.Width,$panel.Height)
        $graphics=[Drawing.Graphics]::FromImage($bitmap)
        try { $graphics.CopyFromScreen($panel.Left,$panel.Top,0,0,$bitmap.Size); $bitmap.Save((Join-Path $output $name),[Drawing.Imaging.ImageFormat]::Png) }
        finally { $graphics.Dispose(); $bitmap.Dispose() }
    }
    function Capture-Context {
        # Include the preceding desktop column/row to inspect grid alignment.
        $left=[Math]::Max(0,$panel.Left-76); $top=[Math]::Max(0,$panel.Top-91)
        $bitmap=[Drawing.Bitmap]::new($panel.Left+$panel.Width-$left,$panel.Top+$panel.Height-$top)
        $graphics=[Drawing.Graphics]::FromImage($bitmap)
        try { $graphics.CopyFromScreen($left,$top,0,0,$bitmap.Size); $bitmap.Save((Join-Path $output 'desktop-context.png'),[Drawing.Imaging.ImageFormat]::Png) }
        finally { $graphics.Dispose(); $bitmap.Dispose() }
    }
    Move-Pointer 180 18
    Start-Sleep -Milliseconds 1500
    Capture-Panel 'glass-normal.png'
    Capture-Context
    Move-Pointer 114 75
    Capture-Panel 'glass-hover.png'
    if($Measure) {
        & (Join-Path $PSScriptRoot 'performance_probe.ps1') -AppProcessId $app.Id -State 'Open, JPEG + PNG + text cached, hover stationary, static wallpaper' -OutputPath (Join-Path $output 'open-performance.json')
    }
    Invoke-EdgeControlAction 'Solid'
    Start-Sleep -Milliseconds 400
    Capture-Panel 'solid-hover.png'
    Invoke-EdgeControlAction 'Glass'
    Move-Pointer 180 18
    Capture-Panel 'glass-restored.png'
    if($Measure) {
        foreach($window in [EdgeTuckProbe]::Windows() | Where-Object { $_.Pid -eq $app.Id -and $_.Class -in @('EdgeTuck.Control','EdgeTuck.Material') }) { [void][EdgeTuckProbe]::ShowWindow([IntPtr]$window.Handle,0) }
        [void][ContentPointer]::SetCursorPos(960,750)
        [void][EdgeTuckProbe]::PostMessage([IntPtr]$panel.Handle,0x02A3,[IntPtr]::Zero,[IntPtr]::Zero)
        Start-Sleep -Milliseconds 800
        & (Join-Path $PSScriptRoot 'performance_probe.ps1') -AppProcessId $app.Id -State 'All drawers closed, thumbnail cache retained' -OutputPath (Join-Path $output 'closed-performance.json')
    }
    $after = Get-FileHash -LiteralPath @($portrait,$landscape,$document) | Select-Object Path,Hash
    if(Compare-Object $before $after -Property Path,Hash) { throw 'Preview fixture changed.' }
    [pscustomobject]@{Panel=$panel;Scale=$factor;FixturesUnchanged=$true;Files=$after;Isolated=$true} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8
}
finally {
    if($app -and !$app.HasExited) {
        [void][EdgeTuckProbe]::PostMessage([EdgeTuckProbe]::FindWindow('EdgeTuck.Broker','EdgeTuck Broker'),0x8005,[IntPtr]::Zero,[IntPtr]::Zero)
        if(!$app.WaitForExit(5000)) { throw 'Isolated preview did not exit normally.' }
    }
    $env:EDGETUCK_PREVIEW_ITEMS=$previousItems
    [void][ContentPointer]::SetCursorPos($pointer.X,$pointer.Y)
}
