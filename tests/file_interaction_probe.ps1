param([string]$OutputDirectory='artifacts/v050-interaction')
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
if(Get-Process EdgeTuck -ErrorAction SilentlyContinue) { throw 'Exit regular EdgeTuck before this isolated test.' }
$output=[IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory))
[void][IO.Directory]::CreateDirectory($output)
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class FilePointer {
 [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }
 [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
 [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
 [DllImport("user32.dll")] public static extern void mouse_event(uint flags,uint x,uint y,uint data,UIntPtr extra);
 [DllImport("user32.dll")] public static extern void keybd_event(byte key,byte scan,uint flags,UIntPtr extra);
 [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hwnd,uint msg,IntPtr wp,IntPtr lp);
 [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr hwnd,uint flag);
}
'@
$files=@('one.txt','two.txt','three.txt') | ForEach-Object { Join-Path $output $_ }
foreach($file in $files) { [IO.File]::WriteAllText($file,'Owned reference fixture: '+[IO.Path]::GetFileName($file)) }
$hashes=Get-FileHash -LiteralPath $files | Select-Object Path,Hash
$oldItems=$env:EDGETUCK_PREVIEW_ITEMS; $env:EDGETUCK_PREVIEW_ITEMS=$files -join "`n"
$oldPointer=[FilePointer+POINT]::new(); [void][FilePointer]::GetCursorPos([ref]$oldPointer)
$app=$null
$dragging=$false
try {
 $app=Start-Process (Join-Path $repo 'build/Release/EdgeTuck.exe') -ArgumentList '--live-preview' -WindowStyle Hidden -PassThru
 Start-Sleep -Milliseconds 1200
 . (Join-Path $PSScriptRoot 'ui_probe.ps1') -Action Inspect | Out-Null
 $grid=(& (Join-Path $repo 'build/Release/EdgeTuckGridProbe.exe')) -join "`n"
 if($grid -notmatch 'spacing=(\d+)x') {throw 'Desktop spacing unavailable.'}
 $pitch=[int]$Matches[1]; $firstX=[int]($pitch*.5); $secondX=[int]($pitch*1.5)
 foreach($w in [EdgeTuckProbe]::Windows() | Where-Object {$_.Class -eq 'EdgeTuck.Material' -and $_.Pid -eq $app.Id}) { [void][EdgeTuckProbe]::ShowWindow([IntPtr]$w.Handle,0) }
 [void][EdgeTuckProbe]::ShowWindow($control,9)
 [void][EdgeTuckProbe]::SetForegroundWindow($control)
 Invoke-EdgeControlAction 'Preview'
 Start-Sleep -Milliseconds 280
 $panel=[EdgeTuckProbe]::Windows() | Where-Object {$_.Class -eq 'EdgeTuck.Drawer' -and $_.Pid -eq $app.Id -and $_.HitWidth -gt 40 -and $_.HitHeight -gt 40} | Select-Object -First 1
 if(!$panel) { throw 'No preview panel.' }
 $handle=[IntPtr]$panel.Handle
 function Count-Items { [FilePointer]::SendMessage($handle,0x805A,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() }
 function At-Item([int]$index) {
   $point=[EdgeTuckProbe+POINT]::new(); $point.X=$panel.Left+$pitch*$index+[int]($pitch/2); $point.Y=$panel.Top+[int](68*$factor)
   if([EdgeTuckProbe]::WindowFromPoint($point) -ne $handle) { throw 'Item is covered; stopping without mouse input.' }
   [void][FilePointer]::SetCursorPos($point.X,$point.Y); Start-Sleep -Milliseconds 100
   return $point
 }
 function Capture([string]$name) {
   $bitmap=[Drawing.Bitmap]::new($panel.Width,$panel.Height); $g=[Drawing.Graphics]::FromImage($bitmap)
   try { $g.CopyFromScreen($panel.Left,$panel.Top,0,0,$bitmap.Size); $bitmap.Save((Join-Path $output $name)) } finally {$g.Dispose();$bitmap.Dispose()}
 }
 if((Count-Items) -ne 3) { throw 'Fixtures were not loaded.' }
 $point=At-Item 0
 [void][EdgeTuckProbe]::PostMessage($handle,0x201,[IntPtr]1,[IntPtr]($firstX -bor ([int](68*$factor) -shl 16)))
 [void][EdgeTuckProbe]::PostMessage($handle,0x202,[IntPtr]::Zero,[IntPtr]($firstX -bor ([int](68*$factor) -shl 16)))
 Start-Sleep -Milliseconds 120
 if([FilePointer]::SendMessage($handle,0x805B,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -ne 1) {throw 'Mouse selection failed.'}
 # Ctrl-modified window input verifies hit testing and selection without altering the user's global keyboard state.
 [void][EdgeTuckProbe]::PostMessage($handle,0x201,[IntPtr]9,[IntPtr]($secondX -bor ([int](68*$factor) -shl 16)))
 [void][EdgeTuckProbe]::PostMessage($handle,0x202,[IntPtr]8,[IntPtr]($secondX -bor ([int](68*$factor) -shl 16)))
 Start-Sleep -Milliseconds 150
 if([FilePointer]::SendMessage($handle,0x805B,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -ne 2) {throw 'Ctrl multi-select failed.'}
 Capture 'multi-selection.png'
 # Delete in a drawer removes the two references only.
 [void][EdgeTuckProbe]::PostMessage($handle,0x100,[IntPtr]0x2E,[IntPtr]::Zero)
 Start-Sleep -Milliseconds 150
 if((Count-Items) -ne 1) {throw 'Reference removal failed.'}
 Capture 'after-remove.png'
 # Inspect native menu appearance, dismiss without invoking any system file operation.
 $point=At-Item 1
 [void][EdgeTuckProbe]::PostMessage($handle,0x7B,$handle,[IntPtr]($point.X -bor ($point.Y -shl 16)))
 $popup=[IntPtr]::Zero
 for($attempt=0;$attempt -lt 50 -and $popup -eq [IntPtr]::Zero;++$attempt) { Start-Sleep -Milliseconds 100; $popup=[EdgeTuckProbe]::FindWindow('#32768',$null) }
 if($popup -eq [IntPtr]::Zero) {throw 'Native popup did not open.'}
 Start-Sleep -Milliseconds 350
 $menuRect=[EdgeTuckProbe+RECT]::new(); [void][EdgeTuckProbe]::GetWindowRect($popup,[ref]$menuRect)
 $menuBitmap=[Drawing.Bitmap]::new($menuRect.Right-$menuRect.Left,$menuRect.Bottom-$menuRect.Top); $menuGraphics=[Drawing.Graphics]::FromImage($menuBitmap)
 try {$menuGraphics.CopyFromScreen($menuRect.Left,$menuRect.Top,0,0,$menuBitmap.Size); $menuBitmap.Save((Join-Path $output 'native-menu.png'))} finally {$menuGraphics.Dispose();$menuBitmap.Dispose()}
 [void][EdgeTuckProbe]::PostMessage($handle,0x1F,[IntPtr]::Zero,[IntPtr]::Zero)
 Start-Sleep -Milliseconds 200
 # Find an exposed real desktop point before starting a real OLE drag. No drop is made onto another app.
 $desktopPoint=$null
 foreach($xy in @(@(1850,950),@(1850,800),@(1800,700),@(100,950))) {
   $p=[EdgeTuckProbe+POINT]::new(); $p.X=$xy[0];$p.Y=$xy[1]
   $root=[FilePointer]::GetAncestor([EdgeTuckProbe]::WindowFromPoint($p),2)
   $record=[EdgeTuckProbe]::Windows() | Where-Object {$_.Handle -eq $root.ToInt64() -and $_.Class -in @('Progman','WorkerW')}
   if($record) {$desktopPoint=$p;break}
 }
 if(!$desktopPoint) {throw 'Desktop is covered; real drag test cannot run.'}
 $point=At-Item 1
 $dragging=$true; [FilePointer]::mouse_event(2,0,0,0,[UIntPtr]::Zero)
 Start-Sleep -Milliseconds 120
 [void][FilePointer]::SetCursorPos($point.X-20,$point.Y+8)
 Start-Sleep -Milliseconds 150
 [void][FilePointer]::SetCursorPos($desktopPoint.X,$desktopPoint.Y)
 Start-Sleep -Milliseconds 250
 if([FilePointer]::GetAncestor([EdgeTuckProbe]::WindowFromPoint($desktopPoint),2) -ne [IntPtr]$record.Handle) {throw 'Desktop drop target changed.'}
 [FilePointer]::mouse_event(4,0,0,0,[UIntPtr]::Zero); $dragging=$false
 Start-Sleep -Milliseconds 450
 if((Count-Items) -ne 0) {throw 'Real desktop drop did not remove the final reference.'}
 $after=Get-FileHash -LiteralPath $files | Select-Object Path,Hash
 if(Compare-Object $hashes $after -Property Path,Hash) {throw 'Original test files changed.'}
 [pscustomobject]@{Passed=$true;Isolated=$true;MouseSelection=$true;MultiSelection=$true;NativeMenu=$true;DesktopDrop=$true;OriginalFilesUnchanged=$true} |
   ConvertTo-Json | Set-Content (Join-Path $output 'result.json') -Encoding utf8
}
finally {
 if($dragging) {
   [FilePointer]::keybd_event(0x1B,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 100
   [FilePointer]::keybd_event(0x1B,0,2,[UIntPtr]::Zero)
   [FilePointer]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
 }
 if($app -and !$app.HasExited) {
   [void][EdgeTuckProbe]::PostMessage($handle,0x1F,[IntPtr]::Zero,[IntPtr]::Zero)
   [void][EdgeTuckProbe]::PostMessage([EdgeTuckProbe]::FindWindow('EdgeTuck.Broker','EdgeTuck Broker'),0x8005,[IntPtr]::Zero,[IntPtr]::Zero)
   if(!$app.WaitForExit(5000)) {throw 'Isolated app failed to exit.'}
 }
 $env:EDGETUCK_PREVIEW_ITEMS=$oldItems
 [void][FilePointer]::SetCursorPos($oldPointer.X,$oldPointer.Y)
}
