param(
  [Parameter(Mandatory=$true)][string]$Runtime,
  [string]$Output='artifacts/shader-metadata/tests',
  [switch]$DefaultOn
)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$Runtime=(Resolve-Path -LiteralPath $Runtime).Path
$Output=[IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars=Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
$compile=@"
@echo off
call "$vcvars" >nul
cl /nologo /EHsc /std:c++17 /utf-8 /I "$repo" "$PSScriptRoot\fixture.cpp" /Fe:"$Output\fixture.exe" /Fo:"$Output\fixture.obj" d3dcompiler.lib dxguid.lib psapi.lib
"@
Set-Content -LiteralPath "$Output/build.cmd" -Value $compile
& cmd /c "$Output/build.cmd" > "$Output/build.log" 2>&1
if($LASTEXITCODE){throw "Fixture compilation failed: $Output/build.log"}
$savedDiag=$env:RIDERDUCK_DIAG_SHADER_METADATA
$savedPath=$env:PATH
$savedPython=$env:PYTHONHOME
try {
 $env:RIDERDUCK_DIAG_SHADER_METADATA='0'
 & "$Output/fixture.exe" prepare "$Output/offline.dxbc" > "$Output/prepare.log" 2>&1
 if($LASTEXITCODE){throw "Offline compilation failed: $Output/prepare.log"}
 foreach($mode in @('off','on')){
  $dir=Join-Path $Output $mode
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  $env:RIDERDUCK_DIAG_SHADER_METADATA=if($mode -eq 'on'){'1'}else{'0'}
  if($DefaultOn -and $mode -eq 'on') {$env:RIDERDUCK_DIAG_SHADER_METADATA=$null}
  & "$Output/fixture.exe" "$Runtime/riderduck.dll" "$dir/metadata" "$Output/offline.dxbc" $dir > "$dir/capture.log" 2>&1
  if($LASTEXITCODE){throw "Capture $mode failed: $dir/capture.log"}
  $capture=Get-ChildItem -LiteralPath $dir -Filter '*.rdc' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if(!$capture){throw 'No capture was produced'}
  if($DefaultOn -and $mode -eq 'on') {
   $activation=Get-Content -LiteralPath ($capture.FullName+'.shader-metadata.json') -Raw | ConvertFrom-Json
   if(!$activation.build_default_enabled -or $activation.environment_override -ne '') {
    throw 'Expected build-default activation with an unset environment variable'
   }
  }
  $env:PYTHONHOME=$Runtime
  $env:PATH=$Runtime+';'+$savedPath
  & "$Runtime/pyhost.exe" "$PSScriptRoot/verify.py" $Runtime $capture.FullName "$dir/replay.json" > "$dir/verify.log" 2>&1
  if($LASTEXITCODE){throw "Replay $mode failed: $dir/verify.log"}
  Get-Content "$dir/verify.log"
  # A second independent replay verifies disk persistence rather than an in-process cache.
  & "$Runtime/pyhost.exe" "$PSScriptRoot/verify.py" $Runtime $capture.FullName "$dir/reopened.json" > "$dir/reopened.log" 2>&1
  if($LASTEXITCODE){throw "Reopen $mode failed: $dir/reopened.log"}
 }
 & "$Runtime/pyhost.exe" "$PSScriptRoot/check.py" $Output
 if($LASTEXITCODE){throw 'Metadata comparison failed'}
} finally {
 $env:RIDERDUCK_DIAG_SHADER_METADATA=$savedDiag
 $env:PYTHONHOME=$savedPython
 $env:PATH=$savedPath
}
