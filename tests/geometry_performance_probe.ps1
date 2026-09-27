param([string]$OutputDirectory=(Join-Path $PSScriptRoot '..\artifacts\v031-performance'),[switch]$Realtime)
$ErrorActionPreference='Stop'
if(Get-Process -Name EdgeTuck -ErrorAction SilentlyContinue) { throw 'Exit other EdgeTuck instances before the isolated performance probe.' }
$output=[IO.Path]::GetFullPath($OutputDirectory); [void][IO.Directory]::CreateDirectory($output)
$exe=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\build\Release\EdgeTuck.exe'))
$argument=if($Realtime){'--live-preview'}else{'--material-preview'}
$preview=Start-Process -FilePath $exe -ArgumentList $argument -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $output 'app.log')
try {
    Start-Sleep -Milliseconds 650
    $null=. (Join-Path $PSScriptRoot 'ui_probe.ps1') -Action Inspect
    Add-Type -AssemblyName System.Windows.Forms
    $originalCursor=[Windows.Forms.Cursor]::Position
    [Windows.Forms.Cursor]::Position=[Drawing.Point]::new(960,750)
    $initial=@([EdgeTuckProbe]::Windows() | Where-Object { $_.Class -eq 'EdgeTuck.Drawer' -and $_.Pid -eq $preview.Id })
    if($initial.Count -ne 3) { throw 'Isolated default layout was not loaded; refusing to interact.' }
    foreach($p in [EdgeTuckProbe]::Windows() | Where-Object { $_.Class -in 'EdgeTuck.Material','EdgeTuck.Drawer' }) { [void][EdgeTuckProbe]::PostMessage([IntPtr]$p.Handle,0x10,[IntPtr]::Zero,[IntPtr]::Zero) }
    [void][EdgeTuckProbe]::ShowWindow($control,0)
    Start-Sleep -Milliseconds 650
    function Assert-Closed {
        if([EdgeTuckProbe]::Windows() | Where-Object { $_.Class -eq 'EdgeTuck.Drawer' -and $_.Pid -eq $preview.Id -and $_.HitWidth -gt 40 -and $_.HitHeight -gt 40 }) { throw 'Pointer or another action reopened a drawer; this closed sample is invalid.' }
    }
    Assert-Closed
    & (Join-Path $PSScriptRoot 'performance_probe.ps1') -AppProcessId $preview.Id -State 'All drawers closed before resize' -OutputPath (Join-Path $output 'closed-before.json')
    Assert-Closed
    [void][EdgeTuckProbe]::ShowWindow($control,9)
    Invoke-EdgeControlAction 'Preview'
    Start-Sleep -Milliseconds 300
    $p=[EdgeTuckProbe]::Windows() | Where-Object { $_.Class -eq 'EdgeTuck.Drawer' -and $_.HitWidth -gt 40 -and $_.HitHeight -gt 40 } | Select-Object -First 1
    if(!$p) { throw 'Preview did not open.' }
    if($p.Left -le 0 -or $p.Top -le 0) { throw 'Expected the isolated right-side default drawer.' }
    $drawer=[IntPtr]$p.Handle
    # Hold a real depth-resize gesture so the open scene remains stable during
    # the sample. This protects against auto-close without changing user prefs.
    $packed=1 -bor ([int]($p.Height/2) -shl 16)
    [void][EdgeTuckProbe]::PostMessage($drawer,0x0201,[IntPtr]1,[IntPtr]$packed)
    $packed=([int](1-152*$factor) -band 0xFFFF) -bor ([int]($p.Height/2) -shl 16)
    [void][EdgeTuckProbe]::PostMessage($drawer,0x0200,[IntPtr]1,[IntPtr]$packed)
    Start-Sleep -Milliseconds 350
    & (Join-Path $PSScriptRoot 'performance_probe.ps1') -AppProcessId $preview.Id -State 'Open after depth resize; pointer held stationary' -OutputPath (Join-Path $output 'resized-open.json')
    [void][EdgeTuckProbe]::PostMessage($drawer,0x0202,[IntPtr]::Zero,[IntPtr](80 -bor (30 -shl 16)))
    [void][EdgeTuckProbe]::PostMessage($drawer,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
    [void][EdgeTuckProbe]::ShowWindow($control,0)
    Start-Sleep -Milliseconds 650
    Assert-Closed
    & (Join-Path $PSScriptRoot 'performance_probe.ps1') -AppProcessId $preview.Id -State 'All drawers closed after resize; shared scene released' -OutputPath (Join-Path $output 'closed-after.json')
    Assert-Closed
} finally {
    if('EdgeTuckProbe' -as [type]) { [void][EdgeTuckProbe]::PostMessage([EdgeTuckProbe]::FindWindow('EdgeTuck.Broker','EdgeTuck Broker'),0x8005,[IntPtr]::Zero,[IntPtr]::Zero) }
    if(!$preview.WaitForExit(5000)) { throw 'Preview did not exit.' }
    if($originalCursor) { [Windows.Forms.Cursor]::Position=$originalCursor }
}
