param([string]$OutputDirectory='artifacts/v050-idle')
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
if(Get-Process EdgeTuck -ErrorAction SilentlyContinue) {throw 'Exit regular EdgeTuck before isolated measurement.'}
$output=[IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory)); [void][IO.Directory]::CreateDirectory($output)
$fixture=Join-Path $output 'watch-me.txt'; [IO.File]::WriteAllText($fixture,'Owned file notification fixture.')
$previous=$env:EDGETUCK_PREVIEW_ITEMS; $env:EDGETUCK_PREVIEW_ITEMS=$fixture
$app=$null
try {
 $app=Start-Process (Join-Path $repo 'build/Release/EdgeTuck.exe') -ArgumentList '--live-preview' -WindowStyle Hidden -PassThru
 Start-Sleep -Milliseconds 1200
 . (Join-Path $PSScriptRoot 'ui_probe.ps1') -Action Hide | Out-Null
 foreach($w in [EdgeTuckProbe]::Windows() | Where-Object {$_.Pid -eq $app.Id -and $_.Class -eq 'EdgeTuck.Material'}) { [void][EdgeTuckProbe]::ShowWindow([IntPtr]$w.Handle,0) }
 Start-Sleep -Milliseconds 800
 function Assert-Closed {
   $panels=@([EdgeTuckProbe]::Windows() | Where-Object {$_.Pid -eq $app.Id -and $_.Class -eq 'EdgeTuck.Drawer'})
   if($panels.Count -ne 3 -or @($panels | Where-Object {$_.HitWidth -gt 40 -and $_.HitHeight -gt 40}).Count) {throw 'Drawers are not all closed; measurement invalid.'}
 }
 Assert-Closed
 & (Join-Path $PSScriptRoot 'performance_probe.ps1') -AppProcessId $app.Id -State 'All three drawers closed, file notifications registered, no directory polling' -OutputPath (Join-Path $output 'closed-performance.json')
 Assert-Closed
}
finally {
 if($app -and !$app.HasExited) {
   [void][EdgeTuckProbe]::PostMessage([EdgeTuckProbe]::FindWindow('EdgeTuck.Broker','EdgeTuck Broker'),0x8005,[IntPtr]::Zero,[IntPtr]::Zero)
   if(!$app.WaitForExit(5000)) {throw 'Isolated app did not exit normally.'}
 }
 $env:EDGETUCK_PREVIEW_ITEMS=$previous
}
