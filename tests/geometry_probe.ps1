param([string]$OutputDirectory = (Join-Path $PSScriptRoot '..\artifacts\v031-geometry'))
# Real presented-window regression. Only run against --material-preview, which
# never reads or saves user settings. The backdrop is an owned temporary form.
$ErrorActionPreference = 'Stop'
$instances = @(Get-CimInstance Win32_Process -Filter "Name='EdgeTuck.exe'")
if ($instances.Count -ne 1 -or $instances[0].CommandLine -notlike '*--material-preview*') { throw 'Requires exactly one isolated --material-preview instance.' }
$null = . (Join-Path $PSScriptRoot 'ui_probe.ps1') -Action Inspect
Add-Type -AssemblyName System.Windows.Forms
Add-Type -ReferencedAssemblies System.Windows.Forms,System.Drawing @'
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;
using System.Runtime.InteropServices;
public class EdgeGeometryBackground : Form {
    public EdgeGeometryBackground() { FormBorderStyle=FormBorderStyle.None; ShowInTaskbar=false; StartPosition=FormStartPosition.Manual; DoubleBuffered=true; }
    protected override bool ShowWithoutActivation { get { return true; } }
    protected override void OnPaint(PaintEventArgs e) {
        var r=ClientRectangle;
        using(var brush=new LinearGradientBrush(r,Color.FromArgb(15,50,88),Color.FromArgb(63,145,155),38f)) e.Graphics.FillRectangle(brush,r);
        using(var brush=new SolidBrush(Color.FromArgb(240,181,66))) e.Graphics.FillEllipse(brush,110,180,410,410);
        using(var brush=new SolidBrush(Color.FromArgb(229,103,83))) e.Graphics.FillEllipse(brush,r.Width-330,420,400,400);
        using(var pen=new Pen(Color.FromArgb(225,246,240),22)) e.Graphics.DrawLine(pen,15,0,r.Width-30,r.Height);
        using(var pen=new Pen(Color.FromArgb(80,225,245,245),1)) {
            for(int x=0;x<r.Width;x+=25) e.Graphics.DrawLine(pen,x,0,x,r.Height);
            for(int y=0;y<r.Height;y+=25) e.Graphics.DrawLine(pen,0,y,r.Width,y);
        }
    }
}
public static class EdgeGeometryCheck {
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hwnd,uint msg,IntPtr wp,IntPtr lp);
    public static double Difference(string first,string second) {
        using(var a=new Bitmap(first)) using(var b=new Bitmap(second)) {
            if(a.Size!=b.Size) return 999;
            double sum=0; int count=0;
            for(int y=30;y<a.Height-30;y++) for(int x=30;x<a.Width-30;x++) {
                var p=a.GetPixel(x,y); var q=b.GetPixel(x,y);
                sum+=(Math.Abs(p.R-q.R)+Math.Abs(p.G-q.G)+Math.Abs(p.B-q.B))/3.0; count++;
            }
            return sum/Math.Max(1,count);
        }
    }
}
'@
$output = [IO.Path]::GetFullPath($OutputDirectory)
[void][IO.Directory]::CreateDirectory($output)
foreach ($m in [EdgeTuckProbe]::Windows() | Where-Object Class -eq 'EdgeTuck.Material') { [void][EdgeTuckProbe]::PostMessage([IntPtr]$m.Handle,0x10,[IntPtr]::Zero,[IntPtr]::Zero) }
[void][EdgeTuckProbe]::ShowWindow($control,9)
function Mouse([IntPtr]$Window,[uint32]$Message,[int]$X,[int]$Y) {
    $packed=($X -band 0xFFFF) -bor (($Y -band 0xFFFF) -shl 16)
    [void][EdgeGeometryCheck]::SendMessage($Window,$Message,[IntPtr]1,[IntPtr]$packed)
}
function Read-Panel([IntPtr]$Window) { [EdgeTuckProbe]::Windows() | Where-Object Handle -eq $Window.ToInt64() | Select-Object -First 1 }
function Capture-Panel([IntPtr]$Window,[string]$Name) {
    $p=Read-Panel $Window
    $center=[EdgeTuckProbe+POINT]::new(); $center.X=$p.Left+$p.Width/2; $center.Y=$p.Top+$p.Height/2
    if([EdgeTuckProbe]::WindowFromPoint($center) -ne $Window) { throw 'Test drawer is covered or has closed.' }
    $bmp=[Drawing.Bitmap]::new($p.Width,$p.Height); $g=[Drawing.Graphics]::FromImage($bmp)
    try { $g.CopyFromScreen($p.Left,$p.Top,0,0,$bmp.Size); $bmp.Save((Join-Path $output $Name),[Drawing.Imaging.ImageFormat]::Png) }
    finally { $g.Dispose(); $bmp.Dispose() }
}
function Preview([int]$Row) { Invoke-EdgeControlAction 'Preview' $Row; Start-Sleep -Milliseconds 290 }
function Close-Panel([IntPtr]$Window) {
    Mouse $Window 0x0202 80 30
    [void][EdgeGeometryCheck]::SendMessage($Window,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
    Start-Sleep -Milliseconds 340
}
$form=[EdgeGeometryBackground]::new(); $form.Bounds=[Windows.Forms.Screen]::PrimaryScreen.WorkingArea
$results=@()
try {
    $form.Show(); $form.Refresh(); [Windows.Forms.Application]::DoEvents()
    foreach($row in 0,1,2) {
        Preview $row
        $p=[EdgeTuckProbe]::Windows() | Where-Object { $_.Class -eq 'EdgeTuck.Drawer' -and $_.HitWidth -gt 40 -and $_.HitHeight -gt 40 } | Select-Object -First 1
        if(!$p) { throw 'No preview drawer.' }
        $drawer=[IntPtr]$p.Handle
        $side=if($p.Top -eq $form.Top) {'top'} elseif($p.Left -eq $form.Left) {'left'} else {'right'}
        Close-Panel $drawer
        [void][EdgeTuckProbe]::SetWindowPos($form.Handle,[IntPtr]::Zero,0,0,0,0,0x0213)
        [void][EdgeTuckProbe]::SetWindowPos($drawer,[IntPtr]::Zero,0,0,0,0,0x0213)
        $form.Refresh(); [Windows.Forms.Application]::DoEvents(); Start-Sleep -Milliseconds 100
        Preview $row
        foreach($gesture in 'move','grow-axis','grow-depth','shrink-depth') {
            $p=Read-Panel $drawer
            $x=[int](80*$factor); $y=[int](30*$factor); $dx=0; $dy=0
            # Large enough for two cells on supported test DPI values; actual
            # native tests separately assert the exact physical grid pitch.
            $delta=[int](180*$factor)
            if($gesture -eq 'move') { if($side -eq 'top') {$dx=$delta} else {$dy=$delta} }
            if($gesture -eq 'grow-axis') { if($side -eq 'top') {$x=$p.Width-2;$y=[int]($p.Height/2);$dx=$delta} else {$x=[int]($p.Width/2);$y=$p.Height-2;$dy=$delta} }
            if($gesture -like '*depth') {
                if($gesture -eq 'shrink-depth') {$delta=-$delta}
                if($side -eq 'top') {$x=[int]($p.Width/2);$y=$p.Height-2;$dy=$delta}
                elseif($side -eq 'left') {$x=$p.Width-2;$y=[int]($p.Height/2);$dx=$delta}
                else {$x=1;$y=[int]($p.Height/2);$dx=-$delta}
            }
            Mouse $drawer 0x0201 $x $y
            Mouse $drawer 0x0200 ($x+$dx) ($y+$dy)
            Start-Sleep -Milliseconds 100
            $moved=Read-Panel $drawer
            if($p.Left -eq $moved.Left -and $p.Top -eq $moved.Top -and $p.Width -eq $moved.Width -and $p.Height -eq $moved.Height) { throw "Gesture did not change geometry: $side $gesture" }
            $name="$side-$gesture"
            Capture-Panel $drawer "$name-retained.png"
            Close-Panel $drawer
            Preview $row
            Mouse $drawer 0x0201 ([int](80*$factor)) ([int](30*$factor))
            Capture-Panel $drawer "$name-fresh.png"
            $difference=[EdgeGeometryCheck]::Difference((Join-Path $output "$name-retained.png"),(Join-Path $output "$name-fresh.png"))
            $results += [pscustomobject]@{Side=$side;Gesture=$gesture;Width=$moved.Width;Height=$moved.Height;RetainedVsFreshDifference=$difference}
            Mouse $drawer 0x0202 80 30
            if($difference -gt 1.0) { throw "Background crop/material mismatch: $name difference=$difference" }
        }
        Close-Panel $drawer
    }
    $results | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'results.json') -Encoding UTF8
    $results | Format-Table -AutoSize
} finally {
    foreach($p in [EdgeTuckProbe]::Windows() | Where-Object Class -eq 'EdgeTuck.Drawer') { [void][EdgeTuckProbe]::PostMessage([IntPtr]$p.Handle,0x001F,[IntPtr]::Zero,[IntPtr]::Zero); [void][EdgeTuckProbe]::PostMessage([IntPtr]$p.Handle,0x10,[IntPtr]::Zero,[IntPtr]::Zero) }
    $form.Close(); $form.Dispose()
    [void][EdgeTuckProbe]::PostMessage([EdgeTuckProbe]::FindWindow('EdgeTuck.Broker','EdgeTuck Broker'),0x8005,[IntPtr]::Zero,[IntPtr]::Zero)
}
