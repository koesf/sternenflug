[CmdletBinding()]
param(
    [string]$PackageRoot = '',
    [string]$InstallRoot = (Join-Path $env:ProgramData 'obs-studio\plugins\sternenflug')
)
$ErrorActionPreference = 'Stop'
if (-not $PackageRoot) {
    $besideInstaller = Join-Path $PSScriptRoot 'sternenflug'
    if (Test-Path -LiteralPath (Join-Path $besideInstaller 'bin\64bit\sternenflug.dll')) { $PackageRoot = $besideInstaller }
    else { $PackageRoot = Join-Path $env:USERPROFILE '.cache\sternenflug-native\build\package\sternenflug' }
}
$moduleFile = Join-Path $PackageRoot 'bin\64bit\sternenflug.dll'
if (-not (Test-Path -LiteralPath $moduleFile)) { throw 'Das gebaute Sternenflug-Modul fehlt. Zuerst Build.ps1 ausführen.' }
$destinationFile = Join-Path $InstallRoot 'bin\64bit\sternenflug.dll'
if (Test-Path -LiteralPath $destinationFile) {
    $running = Get-Process -Name obs64 -ErrorAction SilentlyContinue
    foreach ($process in $running) {
        $loaded = $null
        try { $loaded = $process.Modules | Where-Object { $_.FileName -eq $destinationFile } } catch { }
        if ($loaded) { throw 'Die installierte Sternenflug-DLL wird von OBS verwendet. OBS schließen und Installation erneut ausführen.' }
    }
    $backupRoot = Join-Path (Split-Path -Parent $PSScriptRoot) ('backups\installed-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Force -Path $backupRoot | Out-Null
    Copy-Item -LiteralPath $InstallRoot -Destination $backupRoot -Recurse
}
New-Item -ItemType Directory -Force -Path (Join-Path $InstallRoot 'bin\64bit'), (Join-Path $InstallRoot 'data') | Out-Null
Copy-Item -LiteralPath $moduleFile -Destination $destinationFile -Force
foreach ($entry in (Get-ChildItem -LiteralPath (Join-Path $PackageRoot 'data') -ErrorAction SilentlyContinue)) {
    Copy-Item -LiteralPath $entry.FullName -Destination (Join-Path $InstallRoot 'data') -Recurse -Force
}
$expected = (Get-FileHash -LiteralPath $moduleFile -Algorithm SHA256).Hash
$actual = (Get-FileHash -LiteralPath $destinationFile -Algorithm SHA256).Hash
if ($expected -ne $actual) { throw 'Die installierte DLL stimmt nicht mit dem geprüften Paket überein.' }
Write-Output "Sternenflug installiert: $destinationFile"
Write-Output "SHA256 geprüft: $actual"
Write-Output 'OBS nach Abschluss laufender Aufnahmen/Streams einmal neu starten. Danach Quellen + → Sternenflug.'
