[CmdletBinding()]
param(
    [ValidateSet('Release', 'RelWithDebInfo')][string]$Configuration = 'Release',
    [string]$ObsRoot = (Join-Path $env:ProgramFiles 'obs-studio'),
    [string]$CacheRoot = (Join-Path $env:USERPROFILE '.cache\sternenflug-native'),
    [string]$BuildRoot = '',
    [switch]$ConfigureOnly,
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$sourceRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildRoot) { $BuildRoot = Join-Path $CacheRoot 'build' }
$obsBin = Join-Path $ObsRoot 'bin\64bit'
foreach ($file in @('obs.dll', 'obs-frontend-api.dll', 'Qt6Core.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $obsBin $file))) { throw "Missing OBS runtime: $file" }
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio C++ tools and CMake are required.' }
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'No Visual Studio installation with C++ x64 tools found.' }
$toolset = Get-ChildItem -LiteralPath (Join-Path $vsRoot 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$toolBin = Join-Path $toolset.FullName 'bin\Hostx64\x64'
$cmake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path -LiteralPath $cmake)) { $cmake = (Get-Command cmake -ErrorAction Stop).Source }

function Get-VerifiedArchive([string]$Url, [string]$Destination, [string]$Hash) {
    if (Test-Path -LiteralPath $Destination) {
        $actual = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash
        if ($actual -eq $Hash) { return }
        throw "Archive checksum mismatch: $Destination. Remove the invalid archive before retrying."
    }
    Write-Host "Downloading $(Split-Path -Leaf $Destination)"
    Invoke-WebRequest -Uri $Url -OutFile $Destination
    if ((Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash -ne $Hash) { throw "Archive checksum mismatch: $Destination" }
}
New-Item -ItemType Directory -Force -Path $CacheRoot | Out-Null
$obsArchive = Join-Path $CacheRoot 'OBS-Studio-32.2.2-Sources.tar.gz'
Get-VerifiedArchive 'https://github.com/obsproject/obs-studio/releases/download/32.2.2/OBS-Studio-32.2.2-Sources.tar.gz' $obsArchive 'ec81fb66b03e75ddb3076b576f62679c39262e0e9960cef3e17a40dc5d68e6b4'
$obsSources = Join-Path $CacheRoot 'obs-studio-32.2.2-sources'
if (-not (Test-Path -LiteralPath (Join-Path $obsSources 'libobs\obs.h'))) {
    # Restrict extraction to public headers/source needed by this module; skip unrelated Unix helper symlinks.
    & tar -xzf $obsArchive -C $CacheRoot 'obs-studio-32.2.2-sources/libobs' 'obs-studio-32.2.2-sources/frontend/api' 'obs-studio-32.2.2-sources/COPYING'
    if ($LASTEXITCODE) { throw 'OBS header extraction failed.' }
}
$qtArchive = Join-Path $CacheRoot 'windows-deps-qt6-2026-07-15-x64.zip'
Get-VerifiedArchive 'https://github.com/obsproject/obs-deps/releases/download/2026-07-15/windows-deps-qt6-2026-07-15-x64.zip' $qtArchive '7c7f985711d80467bdc1795b6592275a27d5b0e5a2c7a61db1f2c1d08d6a5579'
$qtRoot = Join-Path $CacheRoot 'qt6-2026-07-15'
if (-not (Test-Path -LiteralPath (Join-Path $qtRoot 'lib\cmake\Qt6\Qt6Config.cmake'))) {
    Expand-Archive -LiteralPath $qtArchive -DestinationPath $qtRoot
}
$qtVersion = (Get-Item -LiteralPath (Join-Path $obsBin 'Qt6Core.dll')).VersionInfo.ProductVersion
if ($qtVersion -notlike '6.11.1*') { throw "This build targets Qt 6.11.1; installed OBS has $qtVersion." }

$sdkRoot = Join-Path $CacheRoot 'sdk'
New-Item -ItemType Directory -Force -Path (Join-Path $sdkRoot 'include'), (Join-Path $sdkRoot 'lib'), (Join-Path $sdkRoot 'runtime') | Out-Null
$sdkSources = Join-Path $sdkRoot 'obs-source'
if (-not (Test-Path -LiteralPath $sdkSources)) {
    New-Item -ItemType Junction -Path $sdkSources -Target $obsSources | Out-Null
}
@'
#pragma once
#define OBS_DATA_PATH "data"
#define OBS_PLUGIN_PATH "obs-plugins"
#define OBS_PLUGIN_DESTINATION "obs-plugins"
#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
#define OBS_INSTALL_PREFIX ""
'@ | Set-Content -LiteralPath (Join-Path $sdkRoot 'include\obsconfig.h') -Encoding utf8
foreach ($module in @('obs', 'obs-frontend-api')) {
    $dll = Join-Path $obsBin "$module.dll"
    $dump = & (Join-Path $toolBin 'dumpbin.exe') /nologo /exports $dll
    if ($LASTEXITCODE) { throw "Could not inspect exports in $dll" }
    $exports = foreach ($line in $dump) {
        if ($line -match '^\s+\d+\s+[A-Fa-f0-9]+\s+[A-Fa-f0-9]+\s+(\S+)') { $Matches[1] }
    }
    if ($exports.Count -lt 20) { throw "Too few exports found in $dll" }
    $def = Join-Path $sdkRoot "lib\$module.def"
    @("LIBRARY $module.dll", 'EXPORTS') + $exports | Set-Content -LiteralPath $def -Encoding ascii
    & (Join-Path $toolBin 'lib.exe') /nologo /machine:x64 "/def:$def" "/out:$(Join-Path $sdkRoot "lib\$module.lib")"
    if ($LASTEXITCODE) { throw "Import library generation failed for $module" }
    Copy-Item -LiteralPath $dll -Destination (Join-Path $sdkRoot "runtime\$module.dll") -Force
}
$generator = if ($vsRoot -match '\\18\\') { 'Visual Studio 18 2026' } else { 'Visual Studio 17 2022' }
& $cmake -S $sourceRoot -B $BuildRoot -G $generator -A x64 "-DSTERNENFLUG_SDK_ROOT=$sdkRoot" "-DCMAKE_PREFIX_PATH=$qtRoot" -DBUILD_TESTING=ON
if ($LASTEXITCODE) { throw 'CMake configure failed.' }
if ($ConfigureOnly) { return }
& $cmake --build $BuildRoot --config $Configuration --parallel
if ($LASTEXITCODE) { throw 'Build failed.' }
if (-not $SkipTests) {
    $previousTestPath = $env:PATH
    $previousQtPluginPath = $env:QT_QPA_PLATFORM_PLUGIN_PATH
    $previousObsTestBin = $env:STERNENFLUG_OBS_BIN
    try {
        $env:PATH = "$obsBin;$qtRoot\bin;$env:PATH"
        $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $qtRoot 'plugins\platforms'
        $env:STERNENFLUG_OBS_BIN = $obsBin
        & (Join-Path (Split-Path -Parent $cmake) 'ctest.exe') --test-dir $BuildRoot -C $Configuration --output-on-failure
        if ($LASTEXITCODE) { throw 'Tests failed.' }
    } finally {
        $env:PATH = $previousTestPath
        $env:QT_QPA_PLATFORM_PLUGIN_PATH = $previousQtPluginPath
        $env:STERNENFLUG_OBS_BIN = $previousObsTestBin
    }
}
$packageRoot = Join-Path $BuildRoot 'package'
& $cmake --install $BuildRoot --config $Configuration --prefix $packageRoot
if ($LASTEXITCODE) { throw 'Package staging failed.' }
Write-Host "Plugin package: $packageRoot\sternenflug"
Write-Host 'Nothing was installed into the running OBS instance.'
