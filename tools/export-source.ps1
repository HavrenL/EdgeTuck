param([string]$OutputDirectory = ('artifacts/source-preview-' + (Get-Date -Format 'yyyyMMdd-HHmmss')))
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$destination = [IO.Path]::GetFullPath((Join-Path $projectRoot $OutputDirectory))
$artifactRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot 'artifacts')) + [IO.Path]::DirectorySeparatorChar
if (!$destination.StartsWith($artifactRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Export must be a new directory under artifacts.' }
if (Test-Path -LiteralPath $destination) { throw 'Output already exists. Choose a new directory; existing files are never replaced.' }
foreach ($suffix in @('.zip','.manifest.csv')) { if (Test-Path -LiteralPath ($destination + $suffix)) { throw 'An export sidecar already exists. Choose a new directory name.' } }
$files = [Collections.Generic.List[string]]::new()
foreach ($name in @('.gitignore','.gitattributes','README.md','LICENSE','AGENTS.md','CMakeLists.txt','build.ps1')) { $files.Add($name) }
foreach ($name in @('NOTICE')) { if (Test-Path -LiteralPath (Join-Path $projectRoot $name) -PathType Leaf) { $files.Add($name) } }
foreach ($folder in @('src','resources','tests','tools','experiments')) {
    Get-ChildItem -LiteralPath (Join-Path $projectRoot $folder) -Recurse -Force | ForEach-Object {
        if ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Source export refuses links and reparse points.' }
        if (!$_.PSIsContainer -and $_.Extension -in @('.cpp','.hpp','.hlsl','.rc','.manifest','.svg','.ico','.ps1','.py','.cjs','.html')) {
            $files.Add($_.FullName.Substring($projectRoot.Length + 1))
        }
    }
}
foreach ($name in @('PRODUCT.md','ARCHITECTURE.md','DEVELOPMENT.md','REFERENCES.md','SHOWCASE.md')) { $files.Add('docs\' + $name) }
foreach ($name in @('drawer.png','settings.png','appearance.png','drawer-demo.gif')) { $files.Add('docs\assets\' + $name) }
foreach ($relative in $files) {
    $source = Join-Path $projectRoot $relative
    if (!(Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing public file: $relative" }
    if ((Get-Item -LiteralPath $source).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Source export refuses linked files.' }
}
$manifest = foreach ($relative in ($files | Sort-Object -Unique)) {
    $source = Join-Path $projectRoot $relative
    $target = Join-Path $destination $relative
    [void][IO.Directory]::CreateDirectory((Split-Path $target -Parent))
    Copy-Item -LiteralPath $source -Destination $target
    [pscustomobject]@{Path=$relative.Replace('\','/'); Bytes=(Get-Item -LiteralPath $target).Length; SHA256=(Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash}
}
$manifest | Export-Csv -LiteralPath ($destination + '.manifest.csv') -Encoding UTF8 -NoTypeInformation
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($destination, $destination + '.zip', [IO.Compression.CompressionLevel]::Optimal, $true)
[pscustomobject]@{SourceDirectory=$destination; Files=$manifest.Count; Manifest=$destination+'.manifest.csv'; Archive=$destination+'.zip'}
