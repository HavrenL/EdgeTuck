$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
function New-RoundedPath([single]$X,[single]$Y,[single]$Width,[single]$Height,[single]$Radius) {
    $path = [Drawing.Drawing2D.GraphicsPath]::new()
    $d = 2 * $Radius
    $path.AddArc($X,$Y,$d,$d,180,90)
    $path.AddArc($X+$Width-$d,$Y,$d,$d,270,90)
    $path.AddArc($X+$Width-$d,$Y+$Height-$d,$d,$d,0,90)
    $path.AddArc($X,$Y+$Height-$d,$d,$d,90,90)
    $path.CloseFigure()
    return $path
}
$sizes = @(16,24,32,48,64,128,256)
$images = [Collections.Generic.List[byte[]]]::new()
foreach ($size in $sizes) {
    $bitmap = [Drawing.Bitmap]::new($size,$size,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($bitmap)
    $g.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.ScaleTransform($size/256.0,$size/256.0)
    $fill = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml('#287556'))
    $line = [Drawing.Pen]::new([Drawing.ColorTranslator]::FromHtml('#e7f5ec'),12)
    $line.StartCap = $line.EndCap = [Drawing.Drawing2D.LineCap]::Round
    $outer = New-RoundedPath 8 8 240 240 62
    $inner = New-RoundedPath 61 54 134 148 19
    $g.FillPath($fill,$outer); $g.DrawPath($line,$inner)
    $g.DrawLine($line,63,128,193,128); $g.DrawLine($line,115,90,141,90); $g.DrawLine($line,115,164,141,164)
    $stream = [IO.MemoryStream]::new()
    $bitmap.Save($stream,[Drawing.Imaging.ImageFormat]::Png)
    $images.Add($stream.ToArray())
    $stream.Dispose(); $inner.Dispose(); $outer.Dispose(); $line.Dispose(); $fill.Dispose(); $g.Dispose(); $bitmap.Dispose()
}
$output = [IO.File]::Create((Join-Path $PSScriptRoot 'app.ico'))
$writer = [IO.BinaryWriter]::new($output)
try {
    $writer.Write([UInt16]0); $writer.Write([UInt16]1); $writer.Write([UInt16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($i=0; $i -lt $sizes.Count; $i++) {
        $dimension = if ($sizes[$i] -eq 256) { 0 } else { $sizes[$i] }
        $writer.Write([byte]$dimension); $writer.Write([byte]$dimension); $writer.Write([UInt16]0)
        $writer.Write([UInt16]1); $writer.Write([UInt16]32); $writer.Write([UInt32]$images[$i].Length); $writer.Write([UInt32]$offset)
        $offset += $images[$i].Length
    }
    foreach ($image in $images) { $writer.Write($image) }
} finally { $writer.Dispose(); $output.Dispose() }
