param([string]$OutputDirectory = (Join-Path $PSScriptRoot '..\artifacts\v021-visual'), [ValidateRange(0,2)][int]$PreviewRow = 0, [switch]$CheckGlass, [switch]$OpticalMaterial)
# Run with Windows PowerShell 5.1 (powershell.exe); the owned WinForms backdrop
# uses the system .NET Framework assemblies, not the PowerShell 7 runtime.
$ErrorActionPreference = 'Stop'
$instances = @(Get-CimInstance Win32_Process -Filter "Name='EdgeTuck.exe'")
if ($instances.Count -ne 1 -or $instances[0].CommandLine -notlike '*--material-preview*') { throw 'Requires exactly one isolated --material-preview instance.' }
$null = . (Join-Path $PSScriptRoot 'ui_probe.ps1') -Action Inspect
[void][EdgeTuckProbe]::ShowWindow($control, 9)
[void][EdgeTuckProbe]::SetForegroundWindow($control)
# Normalize the settings list before choosing a row; the user may have scrolled it.
for ($i=0; $i -lt 20; ++$i) { [void][EdgeTuckProbe]::PostMessage($control,0x020A,[IntPtr](120 -shl 16),[IntPtr]::Zero) }
Start-Sleep -Milliseconds 150
Add-Type -AssemblyName System.Windows.Forms
Add-Type -ReferencedAssemblies System.Windows.Forms,System.Drawing @'
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;
public class EdgeTuckTestBackground : Form {
    public bool Changed { get; set; }
    protected override bool ShowWithoutActivation { get { return true; } }
    public EdgeTuckTestBackground() {
        FormBorderStyle = FormBorderStyle.None; ShowInTaskbar = false;
        StartPosition = FormStartPosition.Manual; DoubleBuffered = true;
    }
    protected override void OnPaint(PaintEventArgs e) {
        var r = ClientRectangle;
        using (var brush = new LinearGradientBrush(r, Changed ? Color.FromArgb(147,48,30) : Color.FromArgb(14,43,74), Changed ? Color.FromArgb(220,160,55) : Color.FromArgb(73,138,154), 38f)) e.Graphics.FillRectangle(brush, r);
        using (var pen = new Pen(Color.FromArgb(170,212,220,232), 15)) e.Graphics.DrawLine(pen, 30, -20, r.Width-60, r.Height+20);
        using (var brush = new SolidBrush(Color.FromArgb(218,162,74))) e.Graphics.FillEllipse(brush, 110, 45, 93, 93);
        using (var pen = new Pen(Color.FromArgb(110,225,245,245), 1)) {
            for (int x=0; x<r.Width; x+=25) e.Graphics.DrawLine(pen, x, 0, x, r.Height);
            for (int y=0; y<r.Height; y+=25) e.Graphics.DrawLine(pen, 0, y, r.Width, y);
        }
    }
}
public static class EdgeTuckPixelCheck {
    static double Luma(Color c) { return (c.R + c.G + c.B)/3.0; }
    public static double Difference(string first, string second, int x, int y, int w, int h) {
        using (var a = new Bitmap(first)) using (var b = new Bitmap(second)) {
            double sum = 0;
            for (int j=y; j<y+h; ++j) for (int i=x; i<x+w; ++i) {
                var p=a.GetPixel(i,j); var q=b.GetPixel(i,j);
                sum += (Math.Abs(p.R-q.R)+Math.Abs(p.G-q.G)+Math.Abs(p.B-q.B))/3.0;
            }
            return sum/(w*h);
        }
    }
    public static double TextureEnergy(string path, int x, int y, int w, int h) {
        using (var a = new Bitmap(path)) {
            double sum=0;
            for (int j=y; j<y+h; ++j) for (int i=x; i<x+w; ++i) {
                double p=Luma(a.GetPixel(i,j));
                sum += Math.Abs(2*p-Luma(a.GetPixel(i-1,j))-Luma(a.GetPixel(i+1,j)));
                sum += Math.Abs(2*p-Luma(a.GetPixel(i,j-1))-Luma(a.GetPixel(i,j+1)));
            }
            return sum/(w*h);
        }
    }
}
'@
$output = [IO.Path]::GetFullPath($OutputDirectory)
[void][IO.Directory]::CreateDirectory($output)
# Preview the first drawer through its real settings action, then create an
# owned background covering only that drawer. No desktop files or wallpaper change.
Invoke-EdgeControlAction 'Preview' $PreviewRow
Start-Sleep -Milliseconds 300
$panel = [EdgeTuckProbe]::Windows() | Where-Object { $_.Class -eq 'EdgeTuck.Drawer' -and $_.HitWidth -gt 40 -and $_.HitHeight -gt 40 } | Select-Object -First 1
if (!$panel) { throw 'No expanded drawer.' }
$drawer = [IntPtr]$panel.Handle
$form = [EdgeTuckTestBackground]::new()
$form.Bounds = [System.Drawing.Rectangle]::new($panel.Left, $panel.Top, $panel.Width+16, $panel.Height+16)
function Capture-Owned([string]$Name, [bool]$RequireExpanded = $false) {
    $center = [EdgeTuckProbe+POINT]::new(); $center.X = $panel.Left+$panel.Width/2; $center.Y = $panel.Top+$panel.Height/2
    $at = [EdgeTuckProbe]::WindowFromPoint($center)
    if ($at -ne $drawer -and $at -ne $form.Handle) { throw 'Owned test area is covered; capture aborted.' }
    if ($RequireExpanded -and $at -ne $drawer) { throw 'Drawer preview closed during capture; visual check interrupted.' }
    $bitmap = [System.Drawing.Bitmap]::new($form.Width, $form.Height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($form.Left, $form.Top, 0, 0, $bitmap.Size)
        $bitmap.Save((Join-Path $output $Name), [System.Drawing.Imaging.ImageFormat]::Png)
    } finally { $graphics.Dispose(); $bitmap.Dispose() }
}
try {
    [void][EdgeTuckProbe]::PostMessage($drawer, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 330
    $form.Show(); $form.Refresh(); [System.Windows.Forms.Application]::DoEvents()
    [void][EdgeTuckProbe]::SetWindowPos($form.Handle, [IntPtr]::Zero, 0,0,0,0,0x0213)
    Start-Sleep -Milliseconds 250
    Capture-Owned 'background.png'
    if ($CheckGlass) {
        # A desktop-layer closed drawer would be covered by the owned test form.
        # Temporarily raise only the tested app window so the sliver is visible.
        [void][EdgeTuckProbe]::SetWindowPos($drawer, [IntPtr]::Zero, 0,0,0,0,0x0213)
        Start-Sleep -Milliseconds 90
        Capture-Owned 'closed-before.png'
    }
    $client=[EdgeTuckProbe+RECT]::new(); [void][EdgeTuckProbe]::GetClientRect($control,[ref]$client)
    $packed=([int]($client.Right-117*$factor) -band 0xFFFF) -bor ([int]((147+64*$PreviewRow)*$factor) -shl 16)
    [void][EdgeTuckProbe]::PostMessage($control,0x0201,[IntPtr]1,[IntPtr]$packed)
    [void][EdgeTuckProbe]::PostMessage($control,0x0202,[IntPtr]::Zero,[IntPtr]$packed)
    # Capture a few real presented states, not synthetic mockups. File I/O and
    # screen copy timings mean this is not a frame-rate measurement.
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $samples = @()
    foreach ($deadline in @(0, 40, 85, 140, 230, 330)) {
        while ($clock.ElapsedMilliseconds -lt $deadline) { Start-Sleep -Milliseconds 1; [System.Windows.Forms.Application]::DoEvents() }
        $ms = $clock.ElapsedMilliseconds
        Capture-Owned ('open-{0:D3}.png' -f $deadline)
        $samples += [pscustomobject]@{ PlannedMs=$deadline; CaptureStartedMs=$ms }
    }
    Capture-Owned 'expanded.png' $true
    if ($CheckGlass) {
        $form.Changed = $true; $form.Refresh(); [System.Windows.Forms.Application]::DoEvents()
        Start-Sleep -Milliseconds 130
        Capture-Owned 'live-background-changed.png' $true
        $form.Changed = $false; $form.Refresh(); [System.Windows.Forms.Application]::DoEvents()
        Start-Sleep -Milliseconds 100
    }
    [void][EdgeTuckProbe]::PostMessage($drawer, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 70
    Capture-Owned 'closing.png'
    Start-Sleep -Milliseconds 260
    if ($CheckGlass) {
        [void][EdgeTuckProbe]::SetWindowPos($drawer, [IntPtr]::Zero, 0,0,0,0,0x0213)
        Start-Sleep -Milliseconds 90
    }
    Capture-Owned 'collapsed.png'
    if ($CheckGlass) {
        $rx = [int]($panel.Width*.56); $ry = [int](92*$factor); $rw = [int]($panel.Width*.3); $rh = [int](55*$factor)
        $beforeEnergy = [EdgeTuckPixelCheck]::TextureEnergy((Join-Path $output 'background.png'),$rx,$ry,$rw,$rh)
        $afterEnergy = [EdgeTuckPixelCheck]::TextureEnergy((Join-Path $output 'expanded.png'),$rx,$ry,$rw,$rh)
        $liveDifference = [EdgeTuckPixelCheck]::Difference((Join-Path $output 'expanded.png'),(Join-Path $output 'live-background-changed.png'),$rx,$ry,$rw,$rh)
        $closedDifference = [EdgeTuckPixelCheck]::Difference((Join-Path $output 'closed-before.png'),(Join-Path $output 'collapsed.png'),0,0,$panel.Width,$panel.Height)
        $checks = [ordered]@{ TextureBefore=$beforeEnergy; TextureAfter=$afterEnergy; BlurRatio=$afterEnergy/[Math]::Max(.001,$beforeEnergy); LiveBackgroundDifference=$liveDifference; ClosedBeforeAfterDifference=$closedDifference }
        # Sample all four material rims inside the outer highlight, not only the
        # center. The old raw snapshot rim passed center-only blur checks.
        $inset = [int](4*$factor); $band = [int](6*$factor); $margin = [int](40*$factor)
        $edgeRegions = @{
            Left=@($inset,$margin,$band,($panel.Height-2*$margin))
            Right=@(($panel.Width-$inset-$band),$margin,$band,($panel.Height-2*$margin))
            Top=@($margin,$inset,($panel.Width-2*$margin),$band)
            Bottom=@($margin,($panel.Height-$inset-$band),($panel.Width-2*$margin),$band)
        }
        $edgeRatios = [ordered]@{}
        foreach ($edgeName in @('Left','Right','Top','Bottom')) {
            $roi = $edgeRegions[$edgeName]
            $raw = [EdgeTuckPixelCheck]::TextureEnergy((Join-Path $output 'background.png'),$roi[0],$roi[1],$roi[2],$roi[3])
            $filtered = [EdgeTuckPixelCheck]::TextureEnergy((Join-Path $output 'expanded.png'),$roi[0],$roi[1],$roi[2],$roi[3])
            $edgeRatios[$edgeName] = $filtered/[Math]::Max(.001,$raw)
        }
        $checks.RimTextureRatios = $edgeRatios
        $checks | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'glass-checks.json') -Encoding utf8
        if ($checks.BlurRatio -ge $(if ($OpticalMaterial) { .95 } else { .6 })) { throw 'Background is not sufficiently blurred.' }
        if ($OpticalMaterial) {
            # This version deliberately uses one scene for the entire material.
            # Assert a coherent snapshot, never mislabel this as live capture.
            if ($liveDifference -gt 2) { throw 'Optical snapshot unexpectedly mixes live pixels into its center.' }
        } elseif ($liveDifference -lt 15) { throw 'Blurred background did not update live.' }
        if ($closedDifference -gt 2) { throw 'Collapsed drawer differs after opening and closing.' }
        if (!$OpticalMaterial) { foreach ($edgeName in $edgeRatios.Keys) { if ($edgeRatios[$edgeName] -ge .5) { throw "Sharp background survived on the $edgeName rim." } } }
    }
    $samples | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'capture-timing.json') -Encoding utf8
    [EdgeTuckProbe]::Windows() | Where-Object Class -eq 'EdgeTuck.Drawer' | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'windows-after.json') -Encoding utf8
} finally {
    $form.Close(); $form.Dispose()
    [void][EdgeTuckProbe]::PostMessage([EdgeTuckProbe]::FindWindow('EdgeTuck.Broker','EdgeTuck Broker'),0x8002,[IntPtr]::Zero,[IntPtr]::Zero)
}
Write-Output $output
