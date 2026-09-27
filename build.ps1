param([ValidateSet('Release', 'Debug')][string]$Configuration = 'Release', [switch]$Run)
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$buildRoot = Join-Path $projectRoot 'build'
cmake -S $projectRoot -B $buildRoot -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
cmake --build $buildRoot --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
ctest --test-dir $buildRoot -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Verification failed.' }
$distribution = Join-Path $projectRoot 'dist'
New-Item -ItemType Directory -Path $distribution -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $buildRoot "$Configuration\EdgeTuck.exe") -Destination (Join-Path $distribution 'EdgeTuck.exe') -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'README.md') -Destination (Join-Path $distribution 'README.md') -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'LICENSE') -Destination (Join-Path $distribution 'LICENSE') -Force
$publicDocs = Join-Path $distribution 'docs'
New-Item -ItemType Directory -Path $publicDocs -Force | Out-Null
foreach ($name in @('PRODUCT.md','ARCHITECTURE.md','DEVELOPMENT.md','REFERENCES.md','SHOWCASE.md')) {
    Copy-Item -LiteralPath (Join-Path $projectRoot ('docs\' + $name)) -Destination $publicDocs -Force
}
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\assets') -Destination $publicDocs -Recurse -Force
$publicResources = Join-Path $distribution 'resources'
New-Item -ItemType Directory -Path $publicResources -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $projectRoot 'resources\app.svg') -Destination $publicResources -Force
if ($Run) {
    # Use the desktop user's context. A developer tool may inherit a redirected
    # AppData/registry view even though the visible paths are identical.
    & (Join-Path $buildRoot "$Configuration\EdgeTuckExplorerLaunch.exe") (Join-Path $distribution 'EdgeTuck.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Could not launch EdgeTuck through the desktop Explorer session.' }
}
