param([string]$Version = '')
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$builtVersion = (Get-Item -LiteralPath (Join-Path $taskRoot 'out\rel64\SumatraPDF.exe')).VersionInfo.ProductVersion
if (!$Version) { $Version = $builtVersion }
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid version' }
if ($Version -ne $builtVersion) { throw "Package version $Version does not match executable version $builtVersion" }
$releaseDir = Join-Path $taskRoot 'out\public-release'
$baseName = "SumatraPDF-Research-v$Version-x64"
$portableDir = Join-Path $releaseDir $baseName
$zipPath = "$portableDir.zip"
$installerPath = Join-Path $releaseDir "$baseName-install.exe"
if (Test-Path -LiteralPath $portableDir) { throw 'Package already exists' }
New-Item -ItemType Directory -Path $portableDir -Force | Out-Null
foreach ($name in @('SumatraPDF.exe', 'libsumatrapdf.dll', 'PdfFilter.dll', 'PdfPreview.dll', 'sumatrapdf-tool.exe')) {
    Copy-Item -LiteralPath (Join-Path $taskRoot "out\rel64\$name") -Destination $portableDir
}
foreach ($name in @('AUTHORS', 'COPYING', 'COPYING.BSD', 'README.md')) {
    Copy-Item -LiteralPath (Join-Path $taskRoot $name) -Destination $portableDir
}
Copy-Item -LiteralPath (Join-Path $taskRoot 'resources\markdown\THIRD-PARTY-NOTICES.txt') -Destination $portableDir
New-Item -ItemType Directory -Path (Join-Path $portableDir 'research') | Out-Null
foreach ($name in @('Install-Vietnamese-Voice.ps1', 'Speak-Vietnamese.ps1', 'THIRD-PARTY.txt', 'SciHub-MIT-LICENSE.txt', 'Anna-API-MIT-LICENSE.txt')) {
    Copy-Item -LiteralPath (Join-Path $taskRoot "research\$name") -Destination (Join-Path $portableDir 'research')
}
Set-Content -LiteralPath (Join-Path $portableDir 'SumatraPDF-settings.txt') -Encoding ascii -Value @(
    'ToolbarPosition = auto', 'CheckForUpdates = false', 'UiLanguage = en'
)
$hashLines = Get-ChildItem -LiteralPath $portableDir -File -Recurse | Sort-Object FullName | ForEach-Object {
    $name = $_.FullName.Substring($portableDir.Length + 1).Replace('\', '/')
    $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $name"
}
Set-Content -LiteralPath (Join-Path $portableDir 'SHA256SUMS.txt') -Value $hashLines -Encoding ascii
Compress-Archive -LiteralPath $portableDir -DestinationPath $zipPath
Copy-Item -LiteralPath (Join-Path $portableDir 'SumatraPDF.exe') -Destination $installerPath
$releaseHashes = foreach ($path in @($zipPath, $installerPath)) {
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $([IO.Path]::GetFileName($path))"
}
Set-Content -LiteralPath (Join-Path $releaseDir "$baseName-SHA256SUMS.txt") -Value $releaseHashes -Encoding ascii
Get-Item -LiteralPath $zipPath, $installerPath | Select-Object FullName, Length
