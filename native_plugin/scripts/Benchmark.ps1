[CmdletBinding()]
param(
    [string]$ObsRoot = '',
    [string]$CacheRoot = (Join-Path $env:USERPROFILE '.cache\sternenflug-native'),
    [string]$BuildRoot = '',
    [string]$OutputRoot = '',
    [string]$BenchmarkExe = '',
    [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$MachineId = 'local-test',
    [ValidateSet('Release','RelWithDebInfo')][string]$Configuration = 'RelWithDebInfo',
    [ValidateRange(1,600)][int]$WarmupFrames = 30,
    [ValidateRange(2,20)][int]$Repetitions = 3,
    [ValidateRange(10,1000)][int]$FramesPerRepeat = 60,
    [ValidateRange(0,16)][int]$Adapter = 0,
    [switch]$SkipBuild,
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
$sourceRoot = Split-Path -Parent $PSScriptRoot
$repositoryRoot = Split-Path -Parent $sourceRoot
if (-not $BuildRoot) { $BuildRoot = Join-Path $CacheRoot 'build' }
if (-not $OutputRoot) { $OutputRoot = Join-Path $repositoryRoot 'docs\benchmarks' }
if (Get-Process -Name obs64 -ErrorAction SilentlyContinue) {
    throw 'OBS läuft. Für vergleichbare isolierte Messungen OBS zuerst regulär schließen.'
}
if (-not $ObsRoot) {
    $candidates = @($env:OBS_ROOT, (Join-Path $env:ProgramFiles 'obs-studio'),
        (Join-Path ${env:ProgramFiles(x86)} 'Steam\steamapps\common\OBS Studio'))
    $ObsRoot = $candidates | Where-Object { $_ -and (Test-Path -LiteralPath (Join-Path $_ 'bin\64bit\obs.dll')) } | Select-Object -First 1
}
if (-not $ObsRoot) { throw 'OBS nicht gefunden. -ObsRoot <OBS-Ordner> angeben.' }
$obsBin = Join-Path $ObsRoot 'bin\64bit'
$qtRoot = Join-Path $CacheRoot 'qt6-2026-07-15'
$portableBenchmark = Join-Path $sourceRoot 'benchmark\sf_renderer_benchmark.exe'
if (-not $BenchmarkExe -and (Test-Path -LiteralPath $portableBenchmark)) { $BenchmarkExe = $portableBenchmark }
$usingPrebuilt = [bool]$BenchmarkExe
if ($usingPrebuilt) {
    $executable = (Resolve-Path -LiteralPath $BenchmarkExe -ErrorAction Stop).Path
} else {
    $cacheFile = Join-Path $BuildRoot 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cacheFile)) {
        if ($SkipBuild) { throw 'Kein vorbereiteter Build. Build.ps1 zuerst ausführen oder -BenchmarkExe angeben.' }
        & (Join-Path $PSScriptRoot 'Build.ps1') -ObsRoot $ObsRoot -CacheRoot $CacheRoot -BuildRoot $BuildRoot -ConfigureOnly
    }
    $cmakeLine = Get-Content -LiteralPath $cacheFile | Where-Object { $_ -like 'CMAKE_COMMAND:INTERNAL=*' } | Select-Object -First 1
    $cmake = if ($cmakeLine) { $cmakeLine.Substring($cmakeLine.IndexOf('=')+1) } else { (Get-Command cmake -ErrorAction Stop).Source }
    $executable = Join-Path $BuildRoot "$Configuration\sf_renderer_benchmark.exe"
}
$releaseDll = Join-Path $sourceRoot 'release\sternenflug\bin\64bit\sternenflug.dll'
$releaseHashBefore = if (Test-Path -LiteralPath $releaseDll) { (Get-FileHash -LiteralPath $releaseDll -Algorithm SHA256).Hash } else { $null }
if (-not $SkipBuild -and -not $usingPrebuilt) {
    & $cmake -S $sourceRoot -B $BuildRoot -DSTERNENFLUG_BUILD_BENCHMARK=ON
    if ($LASTEXITCODE) { throw 'Benchmark-Konfiguration fehlgeschlagen.' }
    & $cmake --build $BuildRoot --config $Configuration --target sf_renderer_benchmark --parallel
    if ($LASTEXITCODE) { throw 'Benchmark-Build fehlgeschlagen.' }
}
if (-not (Test-Path -LiteralPath $executable)) { throw "Benchmark fehlt: $executable" }
$stamp = Get-Date -Format 'yyyy-MM-dd'
$baseName = "$MachineId-$stamp"
$jsonPath = Join-Path $OutputRoot "$baseName.json"
$csvPath = Join-Path $OutputRoot "$baseName.csv"
if (-not $Force -and ((Test-Path -LiteralPath $jsonPath) -or (Test-Path -LiteralPath $csvPath))) {
    throw 'Ergebnis existiert bereits. Anderen -MachineId/-OutputRoot wählen oder bewusst -Force verwenden.'
}
$scratch = Join-Path $CacheRoot ('benchmark-runs\' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $scratch,$OutputRoot | Out-Null
$rawPath = Join-Path $scratch 'raw.json'
$logPath = Join-Path $scratch 'run.log'
$originalPath = $env:PATH
$originalQtPlatformPath = $env:QT_QPA_PLATFORM_PLUGIN_PATH
$timer = [Diagnostics.Stopwatch]::StartNew()
try {
    $env:PATH = "$obsBin;$qtRoot\bin;$env:PATH"
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $qtRoot 'plugins\platforms'
    & $executable --obs-bin $obsBin --output $rawPath --warmup $WarmupFrames --repetitions $Repetitions --samples $FramesPerRepeat --adapter $Adapter *> $logPath
    if ($LASTEXITCODE) { throw "Benchmark fehlgeschlagen. Lokales Protokoll: $logPath" }
} finally {
    $env:PATH = $originalPath
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = $originalQtPlatformPath
}
$timer.Stop()
$result = Get-Content -LiteralPath $rawPath -Raw | ConvertFrom-Json
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$system = Get-CimInstance Win32_ComputerSystem
$os = Get-CimInstance Win32_OperatingSystem
$gpu = Get-CimInstance Win32_VideoController | Where-Object { $_.Name -eq $result.gpu } | Select-Object -First 1
if (-not $gpu) { $gpu = Get-CimInstance Win32_VideoController | Select-Object -First 1 }
$power = (& powercfg /GETACTIVESCHEME 2>$null) -join ' '
$powerName = if ($power -match '\(([^)]+)\)') { $Matches[1] } else { 'unavailable' }
$hashes = [ordered]@{}
foreach ($relative in @('src/core.cpp','src/core.hpp','src/renderer.cpp','src/renderer.hpp','tests/renderer_benchmark.cpp')) {
    $path = Join-Path $sourceRoot $relative
    $hashes[$relative] = if (Test-Path -LiteralPath $path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash } else { $null }
}
$metadata = [ordered]@{
    machine_id = $MachineId
    recorded_at = (Get-Date).ToString('yyyy-MM-ddTHH:mm:sszzz')
    configuration = $(if ($usingPrebuilt) { 'prebuilt; compare benchmark_exe_sha256' } else { $Configuration })
    cpu = $cpu.Name.Trim()
    cpu_cores = $cpu.NumberOfCores
    cpu_logical_processors = $cpu.NumberOfLogicalProcessors
    ram_visible_bytes = $system.TotalPhysicalMemory
    os = $os.Caption
    os_version = $os.Version
    os_build = $os.BuildNumber
    gpu_driver_windows = $gpu.DriverVersion
    power_scheme = $powerName
    obs_version = (Get-Item -LiteralPath (Join-Path $obsBin 'obs.dll')).VersionInfo.ProductVersion
    qt_version = (Get-Item -LiteralPath (Join-Path $obsBin 'Qt6Core.dll')).VersionInfo.ProductVersion
    compiler = 'MSVC; exact version recorded by benchmark executable'
    elapsed_seconds = [Math]::Round($timer.Elapsed.TotalSeconds,3)
    obs_process_running_at_start = $false
    source_sha256 = $hashes
    benchmark_exe_sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
    release_dll_sha256 = $releaseHashBefore
    notes = @('Isolated offscreen D3D11 latency; not encoder or live OBS FPS.',
        'GPU timestamp interval may include GPU idle gaps during CPU geometry preparation.',
        'Readback measured separately after completed GPU queries.',
        '4096 stress uses synthetic renderer input; public density maximum is 2000.')
}
if ($releaseHashBefore -and (Get-FileHash -LiteralPath $releaseDll -Algorithm SHA256).Hash -ne $releaseHashBefore) {
    throw 'Die Release-DLL hat sich während des Benchmarks geändert; Ergebnis nicht veröffentlichen.'
}
$result | Add-Member -NotePropertyName metadata -NotePropertyValue $metadata
$utf8 = [Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText($jsonPath, ($result | ConvertTo-Json -Depth 15), $utf8)
$rows = foreach ($case in $result.cases) {
    $row = [ordered]@{machine_id=$MachineId;case=$case.case;width=$case.width;height=$case.height;
        synthetic=$case.synthetic;samples=$case.sample_count;particles_median=$case.particles_median;
        symbols_median=$case.symbols_median;vertices_median=$case.vertices_median;gpu_disjoint_samples=$case.gpu_disjoint_samples}
    foreach ($metric in $case.metrics_ms.PSObject.Properties) {
        foreach ($stat in @('median','p95')) {
            $value = if ($null -ne $metric.Value) { $metric.Value.$stat } else { $null }
            $row["$($metric.Name)_${stat}_ms"] = if ($null -eq $value) { '' } else { [Convert]::ToString($value,[Globalization.CultureInfo]::InvariantCulture) }
        }
    }
    [pscustomobject]$row
}
$rows | Export-Csv -LiteralPath $csvPath -Delimiter ',' -Encoding UTF8 -NoTypeInformation
Write-Output "Benchmark abgeschlossen: $jsonPath"
Write-Output "Vergleichstabelle: $csvPath"
Write-Output "Messlauf: $($metadata.elapsed_seconds) Sekunden. Release-DLL unverändert."
