[CmdletBinding()]
param([string]$CacheRoot = (Join-Path $env:USERPROFILE '.cache\sternenflug-native'))
$ErrorActionPreference = 'Stop'
$portableRoot = [System.IO.Path]::GetFullPath((Join-Path $CacheRoot 'obs-portable'))
$exe = Join-Path $portableRoot 'bin\64bit\obs64.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw 'Prepare the isolated portable OBS first with Test-Portable.ps1.' }
if (Get-CimInstance Win32_Process -Filter "Name='obs64.exe'" | Where-Object {$_.ExecutablePath -eq $exe}) {
    throw 'Close the portable test OBS before replacing its test scene.'
}
$configRoot = Join-Path $portableRoot 'config\obs-studio'
$profiles = Join-Path $configRoot 'basic\profiles\SternenflugTest'
$scenes = Join-Path $configRoot 'basic\scenes'
New-Item -ItemType Directory -Force -Path $profiles, $scenes | Out-Null
@'
[General]
LastVersion=537001986
EnableAutoUpdates=false
MaxLogs=10

[OBSWebSocket]
ServerEnabled=false
'@ | Set-Content -LiteralPath (Join-Path $configRoot 'global.ini') -Encoding utf8
@'
[General]
FirstRun=true
Language=de-DE

[Basic]
Profile=Sternenflug Test
ProfileDir=SternenflugTest
SceneCollection=Sternenflug Test
SceneCollectionFile=SternenflugTest.json
ConfigOnNewProfile=false

[BasicWindow]
PreviewEnabled=true
StudioMode=false
'@ | Set-Content -LiteralPath (Join-Path $configRoot 'user.ini') -Encoding utf8
@'
[General]
Name=Sternenflug Test

[Video]
BaseCX=1920
BaseCY=1080
OutputCX=1920
OutputCY=1080
FPSType=0
FPSCommon=60
ColorFormat=NV12
ColorSpace=709
ColorRange=Partial

[Audio]
SampleRate=48000
ChannelSetup=Stereo
DesktopDevice1=disabled
DesktopDevice2=disabled
AuxDevice1=disabled
AuxDevice2=disabled
AuxDevice3=disabled

[Output]
Mode=Simple
'@ | Set-Content -LiteralPath (Join-Path $profiles 'basic.ini') -Encoding utf8

$sourceId = 'sternenflug_native_source'
$uuidA = [guid]::NewGuid().ToString()
$uuidB = [guid]::NewGuid().ToString()
$sceneUuid = [guid]::NewGuid().ToString()
$heart = @(@(0.5,0.3),@(0.42,0.2),@(0.3,0.2),@(0.25,0.32),@(0.3,0.47),@(0.5,0.74),@(0.7,0.47),@(0.75,0.32),@(0.7,0.2),@(0.58,0.2),@(0.5,0.3))
$config = @{schemaVersion=1; settings=@{values=@{density=350; symbol_share=35}; seed=1; colorMode=1; emissionForever=$true}; draft=@{id='100'; strokes=@(,$heart)}} | ConvertTo-Json -Depth 12 -Compress
function TestSource([string]$Name,[string]$Uuid) {
    @{prev_ver=537001986;name=$Name;uuid=$Uuid;id=$sourceId;versioned_id=$sourceId;settings=@{width=1920;height=1080;configuration=$config};mixers=0;sync=0;flags=0;volume=1;enabled=$true;muted=$true;private_settings=@{};hotkeys=@{}}
}
function TestItem([string]$Name,[string]$Uuid,[int]$Id,[bool]$Visible) {
    @{name=$Name;source_uuid=$Uuid;visible=$Visible;locked=$false;rot=0;align=5;bounds_type=0;bounds_align=0;crop_left=0;crop_top=0;crop_right=0;crop_bottom=0;id=$Id;pos=@{x=0;y=0};scale=@{x=1;y=1};scale_ref=@{x=1920;y=1080};bounds=@{x=0;y=0};scale_filter='disable';blend_method='default';blend_type='normal';private_settings=@{}}
}
$sourceA = TestSource 'Sternenflug Test A' $uuidA
$sourceB = TestSource 'Sternenflug Test B' $uuidB
$scene = @{prev_ver=537001986;name='Sternenflug Test';uuid=$sceneUuid;id='scene';versioned_id='scene';settings=@{id_counter=2;custom_size=$false;items=@((TestItem 'Sternenflug Test A' $uuidA 1 $true),(TestItem 'Sternenflug Test B' $uuidB 2 $false))};mixers=0;sync=0;flags=0;volume=1;enabled=$true;muted=$false;private_settings=@{};hotkeys=@{};canvas_uuid='6c69626f-6273-4c00-9d88-c5136d61696e'}
$collection = @{name='Sternenflug Test';current_scene='Sternenflug Test';current_program_scene='Sternenflug Test';scene_order=@(@{name='Sternenflug Test'});sources=@($sourceA,$sourceB,$scene);groups=@();transitions=@(@{name='Fade';id='fade_transition';settings=@{}});quick_transitions=@();current_transition='Fade';transition_duration=300;preview_locked=$false;scaling_enabled=$false;scaling_level=0;scaling_off_x=0;scaling_off_y=0}
$collection | ConvertTo-Json -Depth 18 | Set-Content -LiteralPath (Join-Path $scenes 'SternenflugTest.json') -Encoding utf8
Write-Host "Prepared isolated profile and two native sources: $configRoot"
