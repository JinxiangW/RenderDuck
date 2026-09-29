param(
  [Parameter(Mandatory=$true)][string]$Runtime,
  [string]$Output = '',
  [string]$QtPrefix = '',
  [switch]$Gui,
  [switch]$MouseOnly
)
$ErrorActionPreference = 'Stop'
if ($MouseOnly) { $Gui = $true }
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$Runtime = (Resolve-Path -LiteralPath $Runtime).Path
if (!$Output) { $Output = Join-Path $repo 'artifacts/replay-overrides-tests' }
$Output = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio C++ tools are required.' }
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
$compile = @"
@echo off
call "$vcvars" >nul
cl /nologo /EHsc /std:c++17 /I "$repo" "$PSScriptRoot\fixture.cpp" /Fe:"$Output\fixture.exe" /Fo:"$Output\fixture.obj" d3dcompiler.lib dxguid.lib
if errorlevel 1 exit /b %errorlevel%
cl /nologo /EHsc "$PSScriptRoot\pyhost.cpp" /Fe:"$Output\pyhost.exe" /Fo:"$Output\pyhost.obj"
"@
if ($Gui) {
  if (!$QtPrefix -or !(Test-Path -LiteralPath "$QtPrefix/include/QtWidgets/QWidget")) {
    throw '-Gui requires -QtPrefix pointing to the Qt distribution used for the application build.'
  }
  $guiSource = if ($MouseOnly) { 'mouse_test.cpp' } else { 'gui_test.cpp' }
  $compile += @"

if errorlevel 1 exit /b %errorlevel%
cl /nologo /MD /EHsc /std:c++17 /LD /DRIDERDUCK_PLATFORM_WIN32 /DWIN64 /I "$repo\qriderduck" /I "$repo\riderduck\api\replay" /I "$QtPrefix\include" /I "$QtPrefix\include\QtCore" /I "$QtPrefix\include\QtGui" /I "$QtPrefix\include\QtWidgets" "$PSScriptRoot\$guiSource" /Fe:"$Output\gui_test.dll" /Fo:"$Output\gui_test.obj" /link "$Runtime\riderduck.lib" /LIBPATH:"$QtPrefix\lib" Qt5Core.lib Qt5Gui.lib Qt5Widgets.lib
"@
}
$buildFile = Join-Path $Output 'build.cmd'
Set-Content -LiteralPath $buildFile -Value $compile
& cmd.exe /c $buildFile > (Join-Path $Output 'build.log') 2>&1
if ($LASTEXITCODE) { throw "Fixture build failed. See $Output/build.log." }
$savedPath = $env:PATH
$savedPython = $env:PYTHONHOME
$savedQpa = $env:QT_QPA_PLATFORM
$savedOutput = $env:REPLAY_OVERRIDE_TEST_OUTPUT
try {
  $env:PATH = $Runtime + ';' + $env:PATH
  $env:PYTHONHOME = $Runtime
  $fixturePrefix = Join-Path $Output ('fixture-' + [guid]::NewGuid().ToString('N'))
  & "$Output/fixture.exe" "$Runtime/riderduck.dll" $fixturePrefix > "$Output/capture.log" 2>&1
  if ($LASTEXITCODE) { throw "Capture failed. See $Output/capture.log." }
  $capture = Get-ChildItem -LiteralPath $Output -Filter ((Split-Path $fixturePrefix -Leaf) + '*.rdc') | Select-Object -First 1
  if (!$capture) { throw 'Fixture produced no capture.' }
  if (!$MouseOnly) {
    & "$Output/pyhost.exe" "$PSScriptRoot/verify.py" $Runtime $capture.FullName > "$Output/verify.log" 2>&1
    if ($LASTEXITCODE) { throw "Replay tests failed. See $Output/verify.log." }
    Get-Content -LiteralPath "$Output/verify.log"
  }
  if ($Gui) {
    # A native HWND is required for D3D11 replay outputs; Qt's offscreen plugin is unsuitable.
    $env:QT_QPA_PLATFORM = 'windows'
    $env:REPLAY_OVERRIDE_TEST_OUTPUT = $Output
    $args = @('--python', ('"' + "$PSScriptRoot/gui_test.py" + '"'), ('"' + $capture.FullName + '"'))
    $process = Start-Process -FilePath "$Runtime/qriderduck.exe" -ArgumentList $args -WindowStyle Hidden -PassThru
    if (!$process.WaitForExit(60000)) {
      Stop-Process -Id $process.Id
      throw 'The dedicated UI test process timed out.'
    }
    $result = Get-Content -LiteralPath "$Output/gui-test.log" -Raw
    if ($result -notmatch '^PASS:') { throw $result }
    Write-Output $result
  }
} finally {
  $env:PATH = $savedPath
  $env:PYTHONHOME = $savedPython
  $env:QT_QPA_PLATFORM = $savedQpa
  $env:REPLAY_OVERRIDE_TEST_OUTPUT = $savedOutput
}
