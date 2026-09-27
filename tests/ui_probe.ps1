param(
    [ValidateSet('Inspect', 'Capture', 'CaptureDrawer', 'CaptureDrawerLive', 'CaptureStorage', 'Storage', 'CloseStorage', 'Preview', 'Hide', 'Show', 'Dark', 'Light', 'Solid', 'Glass', 'Material', 'Drawers', 'Appearance', 'General', 'Startup', 'Priority')]
    [string]$Action = 'Inspect',
    [string]$OutputPath,
    [int]$AppProcessId = 0
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class EdgeTuckProbe {
    public static IntPtr FindControl() { return FindWindow("EdgeTuck.Control", null); }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    public delegate bool EnumProc(IntPtr hwnd, IntPtr value);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string name, string title);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern int GetWindowRgn(IntPtr hwnd, IntPtr region);
    [DllImport("gdi32.dll")] public static extern IntPtr CreateRectRgn(int x1, int y1, int x2, int y2);
    [DllImport("gdi32.dll")] public static extern int GetRgnBox(IntPtr region, out RECT bounds);
    [DllImport("gdi32.dll")] public static extern bool DeleteObject(IntPtr obj);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hwnd, ref POINT point);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr data);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr hwnd, StringBuilder name, int count);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr hwnd, StringBuilder name, int count);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtr(IntPtr hwnd, int index);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT point);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int command);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int w, int h, uint flags);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern IntPtr SetProcessDpiAwarenessContext(IntPtr value);
    public static List<object> Windows() {
        var list = new List<object>(); int order = 0;
        EnumWindows((h, p) => {
            var name = new StringBuilder(256); GetClassName(h, name, 256);
            var cls = name.ToString();
            if (cls.StartsWith("EdgeTuck") || cls == "Progman" || cls == "WorkerW") {
                RECT r; GetWindowRect(h, out r); uint pid; GetWindowThreadProcessId(h, out pid);
                RECT hit = r; var region = CreateRectRgn(0, 0, 0, 0);
                if (GetWindowRgn(h, region) != 0) GetRgnBox(region, out hit);
                DeleteObject(region);
                list.Add(new { Handle = h.ToInt64(), Class = cls, Order = order, Visible = IsWindowVisible(h), Pid = pid, Left = r.Left, Top = r.Top, Width = r.Right-r.Left, Height = r.Bottom-r.Top, HitWidth = hit.Right-hit.Left, HitHeight = hit.Bottom-hit.Top, Topmost = (GetWindowLongPtr(h, -20).ToInt64() & 8) != 0 });
            }
            ++order; return true;
        }, IntPtr.Zero);
        return list;
    }
}
'@
[void][EdgeTuckProbe]::SetProcessDpiAwarenessContext([IntPtr](-4))
$owned = @([EdgeTuckProbe]::Windows() | Where-Object { $_.Class -eq 'EdgeTuck.Control' -and (!$AppProcessId -or $_.Pid -eq $AppProcessId) })
if ($owned.Count -ne 1) { throw 'Specify AppProcessId when more than one EdgeTuck instance is running.' }
$AppProcessId = $owned[0].Pid
$control = [IntPtr]$owned[0].Handle
$factor = [EdgeTuckProbe]::GetDpiForWindow($control) / 96.0
function Find-EdgeWindow([string]$Class) {
    $window = [EdgeTuckProbe]::Windows() | Where-Object { $_.Class -eq $Class -and $_.Pid -eq $AppProcessId } | Select-Object -First 1
    if ($window) { return [IntPtr]$window.Handle }; return [IntPtr]::Zero
}
function Click-EdgeControl([double]$X, [double]$Y) {
    $packed = ([int]($X*$factor) -band 0xFFFF) -bor ([int]($Y*$factor) -shl 16)
    [void][EdgeTuckProbe]::PostMessage($control,0x0201,[IntPtr]1,[IntPtr]$packed)
    [void][EdgeTuckProbe]::PostMessage($control,0x0202,[IntPtr]::Zero,[IntPtr]$packed)
    Start-Sleep -Milliseconds 150
}
function Invoke-EdgeControlAction([string]$Name,[int]$Row=0) {
    $client = [EdgeTuckProbe+RECT]::new(); [void][EdgeTuckProbe]::GetClientRect($control,[ref]$client)
    $width=$client.Right/$factor; $height=$client.Bottom/$factor; $contentWidth=$width-240
    if($Name -in 'Preview','Drawers') { Click-EdgeControl 90 122 }
    if($Name -in 'Dark','Light','Solid','Glass','Material','Appearance') { Click-EdgeControl 90 168 }
    if($Name -in 'Storage','Startup','Priority','General') { Click-EdgeControl 90 214 }
    switch($Name) {
        'Preview' { Click-EdgeControl ($width-117) (147+64*$Row) }
        'Hide' { Click-EdgeControl 90 ($height-95) }
        'Dark' { Click-EdgeControl (212+2*(($contentWidth-24)/3+12)+($contentWidth-24)/6) 202 }
        'Light' { Click-EdgeControl (212+($contentWidth-24)/3+12+($contentWidth-24)/6) 202 }
        'Solid' { Click-EdgeControl ($width-88) 352 }
        'Glass' { Click-EdgeControl ($width-185) 352 }
        'Material' { Click-EdgeControl ($width-93) 430 }
        'Storage' { Click-EdgeControl ($width-93) 362 }
        'Startup' { Click-EdgeControl ($width-67) 154 }
        'Priority' { Click-EdgeControl ($width-67) 232 }
    }
}
switch ($Action) {
    'Show' { [void][EdgeTuckProbe]::PostMessage((Find-EdgeWindow 'EdgeTuck.Broker'), 0x8004, [IntPtr]::Zero, [IntPtr]::Zero) }
    'CloseStorage' { [void][EdgeTuckProbe]::PostMessage((Find-EdgeWindow 'EdgeTuck.Storage'), 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
    { $_ -in 'Preview','Hide','Dark','Light','Solid','Glass','Material','Storage','Drawers','Appearance','General','Startup','Priority' } {
        Invoke-EdgeControlAction $Action
    }
    { $_ -in 'Capture','CaptureDrawer','CaptureDrawerLive','CaptureStorage' } {
        if (!$OutputPath) { throw 'OutputPath is required.' }
        $captureWindow = $control
        if ($Action -like 'CaptureDrawer*') {
            $packed = ([int](637 * $factor) -band 0xFFFF) -bor ([int](343 * $factor) -shl 16)
            [void][EdgeTuckProbe]::PostMessage($control, 0x0202, [IntPtr]::Zero, [IntPtr]$packed)
        } elseif ($Action -eq 'CaptureStorage') {
            $captureWindow = (Find-EdgeWindow 'EdgeTuck.Storage')
            if ($captureWindow -eq [IntPtr]::Zero) { throw 'No storage settings window.' }
        } else {
            [void][EdgeTuckProbe]::ShowWindow($control, 9)
        }
        Start-Sleep -Milliseconds 350
        if ($Action -like 'CaptureDrawer*') {
            $panel = [EdgeTuckProbe]::Windows() | Where-Object { $_.Class -eq 'EdgeTuck.Drawer' -and $_.Pid -eq $AppProcessId -and $_.HitWidth -gt 40 -and $_.HitHeight -gt 40 -and $_.Visible } | Select-Object -First 1
            if (!$panel) { throw 'No open drawer.' }
            $captureWindow = [IntPtr]$panel.Handle
        }
        $rect = [EdgeTuckProbe+RECT]::new()
        [void][EdgeTuckProbe]::GetWindowRect($captureWindow, [ref]$rect)
        $bitmap = [System.Drawing.Bitmap]::new($rect.Right-$rect.Left, $rect.Bottom-$rect.Top)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        if ($Action -eq 'CaptureDrawerLive') {
            $center = [EdgeTuckProbe+POINT]::new(); $center.X = ($rect.Left+$rect.Right)/2; $center.Y = ($rect.Top+$rect.Bottom)/2
            if ([EdgeTuckProbe]::WindowFromPoint($center) -ne $captureWindow) { throw 'Drawer is covered; live capture aborted.' }
            $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
        } else {
            $dc = $graphics.GetHdc()
            try { if (![EdgeTuckProbe]::PrintWindow($captureWindow, $dc, 2)) { throw 'PrintWindow failed.' } }
            finally { $graphics.ReleaseHdc($dc) }
        }
        $absolute = [IO.Path]::GetFullPath($OutputPath)
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($absolute)) | Out-Null
        $bitmap.Save($absolute, [System.Drawing.Imaging.ImageFormat]::Png)
        $graphics.Dispose(); $bitmap.Dispose()
        Write-Output $absolute
    }
}
[EdgeTuckProbe]::Windows() | ConvertTo-Json -Depth 4
