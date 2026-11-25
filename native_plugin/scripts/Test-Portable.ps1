[CmdletBinding()]
param(
    [string]$CacheRoot = (Join-Path $env:USERPROFILE '.cache\sternenflug-native'),
    [string]$PackageRoot = '',
    [switch]$Launch
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
if (-not $PackageRoot) { $PackageRoot = Join-Path $CacheRoot 'build\package\sternenflug' }
$portableRoot = Join-Path $CacheRoot 'obs-portable'
$archive = Join-Path $CacheRoot 'OBS-Studio-32.2.2-Windows-x64.zip'
$expectedHash = '4d6e40e3ab155f56b30de517380566a206d74b63cdf5ad49aa596924768f97e1'
New-Item -ItemType Directory -Force -Path $CacheRoot | Out-Null
if (-not (Test-Path -LiteralPath $archive)) {
    Invoke-WebRequest 'https://github.com/obsproject/obs-studio/releases/download/32.2.2/OBS-Studio-32.2.2-Windows-x64.zip' -OutFile $archive
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expectedHash) { throw 'Portable OBS archive checksum mismatch.' }
if (-not (Test-Path -LiteralPath (Join-Path $portableRoot 'bin\64bit\obs64.exe'))) {
    Expand-Archive -LiteralPath $archive -DestinationPath $portableRoot
}
$exe = [System.IO.Path]::GetFullPath((Join-Path $portableRoot 'bin\64bit\obs64.exe'))
$running = Get-CimInstance Win32_Process -Filter "Name='obs64.exe'" | Where-Object { $_.ExecutablePath -eq $exe }
if ($running) { throw 'The isolated portable test OBS is already running. Close it before staging a new build.' }
$module = Join-Path $PackageRoot 'bin\64bit\sternenflug.dll'
if (-not (Test-Path -LiteralPath $module)) { throw "Build the plugin first: missing $module" }
New-Item -ItemType File -Force -Path (Join-Path $portableRoot 'portable_mode.txt') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $portableRoot 'obs-plugins\64bit'), (Join-Path $portableRoot 'data\obs-plugins\sternenflug') | Out-Null
Copy-Item -LiteralPath $module -Destination (Join-Path $portableRoot 'obs-plugins\64bit\sternenflug.dll') -Force
$dataPath = Join-Path $PackageRoot 'data'
if (Test-Path -LiteralPath $dataPath) {
    Get-ChildItem -LiteralPath $dataPath -Force | Copy-Item -Destination (Join-Path $portableRoot 'data\obs-plugins\sternenflug') -Recurse -Force
}
Write-Host "Isolated test OBS: $portableRoot"
Write-Host 'The main OBS installation and its profiles are untouched.'
if ($Launch) {
    $process = Start-Process -FilePath $exe -ArgumentList '--portable', '--multi', '--disable-shutdown-check', '--disable-missing-files-check' -WorkingDirectory (Split-Path -Parent $exe) -WindowStyle Hidden -PassThru
    Write-Host "Started isolated OBS process $($process.Id). Close only this test instance when finished."
}
