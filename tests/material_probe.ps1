param([string]$OutputDirectory = (Join-Path $PSScriptRoot '..\artifacts\v030-material'))
# Run against an isolated material preview: neither mode reads or saves user settings.
$ErrorActionPreference = 'Stop'
$null = . (Join-Path $PSScriptRoot 'ui_probe.ps1') -Action Inspect
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class MaterialProbe {
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr hwnd,int id);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hwnd,uint msg,IntPtr wp,IntPtr lp);
    [DllImport("user32.dll")] public static extern bool GetWindowDisplayAffinity(IntPtr hwnd,out uint value);
}
'@
$output = [IO.Path]::GetFullPath($OutputDirectory)
[void][IO.Directory]::CreateDirectory($output)
$windowInfo = [EdgeTuckProbe]::Windows() | Where-Object Class -eq 'EdgeTuck.Material' | Select-Object -First 1
$window = [IntPtr]$windowInfo.Handle
if ($window -eq [IntPtr]::Zero) { throw 'Start the isolated --material-preview first.' }
$pidValue = [uint32]0
[void][EdgeTuckProbe]::GetWindowThreadProcessId($window,[ref]$pidValue)
$processInfo = Get-CimInstance Win32_Process -Filter "ProcessId=$pidValue"
if ($processInfo.CommandLine -notmatch '\-\-(material|live)-preview') { throw 'Refusing to change regular user settings.' }
$initialLive=$processInfo.CommandLine -match '\-\-live-preview'
function Shot([string]$Name) {
    $r = [EdgeTuckProbe+RECT]::new()
    [void][EdgeTuckProbe]::GetWindowRect($window,[ref]$r)
    $bmp = [Drawing.Bitmap]::new($r.Right-$r.Left,$r.Bottom-$r.Top)
    $g = [Drawing.Graphics]::FromImage($bmp)
    try {
        $dc = $g.GetHdc()
        try { if (![EdgeTuckProbe]::PrintWindow($window,$dc,2)) { throw 'Material window capture failed.' } }
        finally { $g.ReleaseHdc($dc) }
        $bmp.Save((Join-Path $output $Name),[Drawing.Imaging.ImageFormat]::Png)
    } finally { $g.Dispose(); $bmp.Dispose() }
}
function Check-Positions([int[]]$Expected) {
    for ($i=0;$i -lt 4;++$i) {
        $slider = [MaterialProbe]::GetDlgItem($window,200+$i)
        $position = [MaterialProbe]::SendMessage($slider,0x400,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32()
        if ($position -ne $Expected[$i]) { throw "Slider $i expected $($Expected[$i]), got $position" }
    }
}
Check-Positions @(120,18,75,55)
Shot 'settings-clear.png'
$realtime=[MaterialProbe]::GetDlgItem($window,303)
function Check-Live([bool]$Enabled) {
    $expected=if($Enabled){1}else{0}
    if([MaterialProbe]::SendMessage($realtime,0xF0,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -ne $expected) { throw 'Realtime checkbox state did not match.' }
    foreach($d in [EdgeTuckProbe]::Windows() | Where-Object { $_.Class -eq 'EdgeTuck.Drawer' -and $_.Pid -eq $pidValue }) {
        $affinity=[uint32]0
        if(![MaterialProbe]::GetWindowDisplayAffinity([IntPtr]$d.Handle,[ref]$affinity) -or $affinity -ne 0) { throw 'A drawer was unexpectedly excluded from screenshots.' }
    }
}
Check-Live $initialLive
[void][MaterialProbe]::SendMessage($realtime,0xF5,[IntPtr]::Zero,[IntPtr]::Zero)
Check-Live (!$initialLive)
[void][MaterialProbe]::SendMessage($realtime,0xF5,[IntPtr]::Zero,[IntPtr]::Zero)
Check-Live $initialLive
[void][MaterialProbe]::SendMessage($window,0x111,[IntPtr]112,[IntPtr]::Zero)
Check-Positions @(45,24,100,85)
$slider = [MaterialProbe]::GetDlgItem($window,200)
[void][MaterialProbe]::SendMessage($slider,0x405,[IntPtr]1,[IntPtr]317)
[void][MaterialProbe]::SendMessage($window,0x114,[IntPtr]5,$slider)
Check-Positions @(317,24,100,85)
$toggle = [MaterialProbe]::GetDlgItem($window,300)
[void][MaterialProbe]::SendMessage($toggle,0xF5,[IntPtr]::Zero,[IntPtr]::Zero)
if ([MaterialProbe]::SendMessage($toggle,0xF0,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -ne 0) { throw 'Refraction switch did not turn off.' }
Shot 'settings-custom.png'
[void][MaterialProbe]::SendMessage($window,0x111,[IntPtr]110,[IntPtr]::Zero)
Check-Positions @(120,18,75,55)
[void][MaterialProbe]::SendMessage($window,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
if ([EdgeTuckProbe]::Windows() | Where-Object Class -eq 'EdgeTuck.Material') { throw 'Material window did not close.' }
[pscustomobject]@{Passed=$true;Checks=@('Clear defaults','Realtime default','Realtime toggle and all drawer capture affinities in both directions','Crystal preset','Custom slider','Optical switch','Preset restores values','Close settings');UserConfiguration='Not read or written'} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'ui-checks.json') -Encoding utf8
Write-Output $output
