param(
  [Parameter(Mandatory=$true)][string]$Runtime,
  [Parameter(Mandatory=$true)][string]$Capture,
  [Parameter(Mandatory=$true)][string]$Output,
  [int]$Seed = 0,
  [string]$Config = '',
  [string]$Recipe = '',
  [string]$QtPrefix = 'D:/Qt/5.9.4/msvc2015_64'
)
$ErrorActionPreference = 'Stop'
if (!$Recipe -and !$Config) { throw 'Specify -Recipe or -Config; there is no implicit game layout.' }
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$Runtime = (Resolve-Path -LiteralPath $Runtime).Path
$Capture = (Resolve-Path -LiteralPath $Capture).Path
$Output = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
$compile = @"
@echo off
call "$vcvars" >nul
cl /nologo /MD /EHsc /std:c++17 /LD /DRIDERDUCK_PLATFORM_WIN32 /DWIN64 /I "$repo\qriderduck" /I "$repo\riderduck\api\replay" /I "$QtPrefix\include" /I "$QtPrefix\include\QtCore" /I "$QtPrefix\include\QtGui" /I "$QtPrefix\include\QtWidgets" "$PSScriptRoot\auto_camera_test.cpp" /Fe:"$Output\gui_test.dll" /Fo:"$Output\gui_test.obj" /link "$Runtime\riderduck.lib" /LIBPATH:"$QtPrefix\lib" Qt5Core.lib Qt5Gui.lib Qt5Widgets.lib
"@
Set-Content -LiteralPath "$Output/build.cmd" -Value $compile
& cmd.exe /c "$Output/build.cmd" > "$Output/build.log" 2>&1
if ($LASTEXITCODE) { throw "Test build failed: $Output/build.log" }
$owned = Join-Path $Output 'owned.rdc'
if (Test-Path -LiteralPath $owned) { throw 'Use a fresh output directory for this destructive-fixture test.' }
Copy-Item -LiteralPath $Capture -Destination $owned
$env:REPLAY_OVERRIDE_TEST_OUTPUT = $Output
$env:REPLAY_CAMERA_SEED = $Seed
$env:REPLAY_CAMERA_CONFIG = $Config
$env:REPLAY_CAMERA_RECIPE = $Recipe
$env:RIDERDUCK_DIAG_SHADER_METADATA = '0'
$args = @('--python', ('"' + "$PSScriptRoot/gui_test.py" + '"'), ('"' + $owned + '"'))
$process = Start-Process -FilePath "$Runtime/qriderduck.exe" -ArgumentList $args -WindowStyle Hidden -PassThru
Write-Output "Test PID: $($process.Id)"
if (!$process.WaitForExit(180000)) {
  Stop-Process -Id $process.Id
  throw 'Dedicated auto-camera test timed out.'
}
$result = Get-Content -LiteralPath "$Output/gui-test.log" -Raw
if ($result -notmatch '^PASS:') { throw $result }
Write-Output $result
