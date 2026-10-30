# Builds xsvViewer.dll and the unit tests with the Visual Studio 2022 Build Tools.
#   .\build.ps1            configure, build, run the tests
#   .\build.ps1 -Config Debug
#   .\build.ps1 -NoTests
param(
    [ValidateSet('Release', 'Debug')] [string] $Config = 'Release',
    [switch] $NoTests
)
$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio installation with the C++ x64 tools was found.' }

# Import the MSVC environment once, for this process only.
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] }
}

$cmakeDir = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake"
$env:PATH = "$cmakeDir\CMake\bin;$cmakeDir\Ninja;$env:PATH"

$src = $PSScriptRoot
$out = Join-Path $src "build\$Config"

cmake -S $src -B $out -G Ninja "-DCMAKE_BUILD_TYPE=$Config" -Wno-dev | Out-Null
if ($LASTEXITCODE) { throw 'CMake configure failed.' }
cmake --build $out
if ($LASTEXITCODE) { throw 'Build failed.' }

if (-not $NoTests) {
    & (Join-Path $out 'xsvtests.exe')
    if ($LASTEXITCODE) { throw 'Unit tests failed.' }
}

if ($Config -eq 'Release') {
    # Notepad++ needs the folder and the DLL to share a name.
    $dist = Join-Path $src 'dist\xsvViewer'
    New-Item -ItemType Directory -Force $dist | Out-Null
    Copy-Item (Join-Path $out 'xsvViewer.dll') $dist -Force
}
