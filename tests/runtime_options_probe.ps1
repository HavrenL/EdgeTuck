param([string]$OutputDirectory='artifacts/v061-runtime')
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
$output=[IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory))
[void][IO.Directory]::CreateDirectory($output)
$exe=Join-Path $repo 'dist\EdgeTuck.exe'
$app=Get-Process EdgeTuck | Where-Object {$_.Path -eq $exe} | Select-Object -First 1
if(!$app) {throw 'No installed EdgeTuck instance.'}
. (Join-Path $PSScriptRoot 'ui_probe.ps1') -Action Show | Out-Null
Start-Sleep -Milliseconds 200
function Click-RunOption([string]$Name) { Invoke-EdgeControlAction $Name; Start-Sleep -Milliseconds 250 }
$runKey='HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$expected='"'+$exe+'" --tray'
function Run-Value { try { Get-ItemPropertyValue -LiteralPath $runKey -Name EdgeTuck -ErrorAction Stop } catch [System.Management.Automation.PSArgumentException] { return $null } }
if((Run-Value) -eq $expected) { Click-RunOption 'Startup' }
if(Run-Value) {throw 'Startup must be off before enable test.'}
Click-RunOption 'Startup'
if((Run-Value) -ne $expected) {throw 'Startup registration mismatch.'}
Click-RunOption 'Startup'
if(Run-Value) {throw 'Startup disable did not remove own entry.'}
Click-RunOption 'Startup'
if((Run-Value) -ne $expected) {throw 'Startup re-enable failed.'}
$app.Refresh()
if($app.PriorityClass -eq 'AboveNormal') { Click-RunOption 'Priority' }
$app.Refresh()
if($app.PriorityClass -ne 'Normal') {throw 'Initial normal priority expected.'}
Click-RunOption 'Priority'
$app.Refresh()
if($app.PriorityClass -ne 'AboveNormal') {throw 'Priority increase failed.'}
Click-RunOption 'Priority'
$app.Refresh()
if($app.PriorityClass -ne 'Normal') {throw 'Priority reset failed.'}
Click-RunOption 'Priority'
$app.Refresh()
if($app.PriorityClass -ne 'AboveNormal') {throw 'Priority re-enable failed.'}
& (Join-Path $PSScriptRoot 'ui_probe.ps1') -Action Capture -OutputPath (Join-Path $output 'settings.png') | Out-Null
& (Join-Path $PSScriptRoot 'ui_probe.ps1') -Action Hide | Out-Null
[pscustomobject]@{StartupCommand=(Run-Value);Priority=$app.PriorityClass.ToString();Pid=$app.Id;Checks='Real UI startup enable/disable/re-enable and process priority above-normal/normal/above-normal';ActualWindowsLogonTested=$false} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8
Get-Content -LiteralPath (Join-Path $output 'result.json')
