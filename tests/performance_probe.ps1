param([int]$AppProcessId, [string]$State, [string]$OutputPath)
$ErrorActionPreference = 'Stop'
$cores = [Environment]::ProcessorCount
$observed = @(Get-Process -Id $AppProcessId) + @(Get-Process -Name dwm,explorer -ErrorAction SilentlyContinue)
$startValues = @{}
foreach ($p in $observed) { $startValues[$p.Id] = $p.TotalProcessorTime.TotalMilliseconds }
$clock = [Diagnostics.Stopwatch]::StartNew()
$gpu = @(); $gpuNote = ''
try {
    $samples = Get-Counter '\GPU Engine(*)\Utilization Percentage' -SampleInterval 1 -MaxSamples 3 -ErrorAction Stop
    foreach ($p in $observed) {
        $matching = @($samples.CounterSamples | Where-Object { $_.InstanceName -like "pid_$($p.Id)_*" })
        $gpu += [pscustomobject]@{Process=$p.ProcessName;Id=$p.Id;MaximumSingleEnginePercent=($matching.CookedValue | Measure-Object -Maximum).Maximum;Samples=$matching.Count}
    }
    $gpuNote = 'Three counter snapshots; maximum single-engine value, not total GPU cost or attributed incremental cost.'
} catch { $gpuNote = 'GPU counters unavailable: ' + $_.Exception.Message }
$remaining = [Math]::Max(0,15000-$clock.ElapsedMilliseconds)
Start-Sleep -Milliseconds $remaining
$elapsed = $clock.Elapsed.TotalMilliseconds
$results = foreach ($p in $observed) {
    $p.Refresh()
    $cpu = $p.TotalProcessorTime.TotalMilliseconds-$startValues[$p.Id]
    [pscustomobject]@{Process=$p.ProcessName;Id=$p.Id;CpuMilliseconds=$cpu;MachineCpuPercent=100*$cpu/$elapsed/$cores;PrivateMiB=$p.PrivateMemorySize64/1MB;WorkingSetMiB=$p.WorkingSet64/1MB;Threads=$p.Threads.Count;Handles=$p.HandleCount}
}
[pscustomobject]@{State=$State;ElapsedMilliseconds=$elapsed;LogicalProcessors=$cores;Processes=$results;GPU=$gpu;GpuNote=$gpuNote;Note='Short observation with other applications running. Explorer and DWM totals include unrelated work; no before/after attribution or frame-rate claim.'} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $OutputPath -Encoding utf8
Get-Content -LiteralPath $OutputPath
