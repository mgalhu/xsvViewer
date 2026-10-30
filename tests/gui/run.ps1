# Runs the end-to-end tests against a Notepad++ on a hidden desktop.
#   .\run.ps1 -Npp <folder of a Notepad++ kept for testing> -Work <scratch folder> [-Scenario name]
# That copy keeps its own settings, so nothing here touches an installed Notepad++.
#
# The pluginsAdmin scenario installs from the public release, so the plugin must not be copied in:
#   .\run.ps1 -Npp <folder> -Work <scratch folder> -Scenario pluginsAdmin -Exe notepad++.x64.dbg.exe -NoDeploy
param(
    [Parameter(Mandatory)] [string] $Npp,
    [Parameter(Mandatory)] [string] $Work,
    [string] $Scenario = 'all',
    [string] $Dll = (Join-Path $PSScriptRoot '..\..\build\Release\xsvViewer.dll'),
    [string] $Exe = 'notepad++.exe',
    [switch] $NoDeploy
)
$ErrorActionPreference = 'Stop'

if (-not $NoDeploy) {
    $target = Join-Path $Npp 'plugins\xsvViewer'
    New-Item -ItemType Directory -Force $target | Out-Null
    Copy-Item $Dll $target -Force

    $shimTarget = Join-Path $Npp 'plugins\xsvTestShim'
    New-Item -ItemType Directory -Force $shimTarget | Out-Null
    Copy-Item (Join-Path (Split-Path $Dll) 'xsvTestShim.dll') $shimTarget -Force
}

$samples = Join-Path $Work 'samples'
$shots = Join-Path $Work 'shots'
New-Item -ItemType Directory -Force $shots | Out-Null
& (Join-Path $PSScriptRoot 'make-samples.ps1') -Out $samples | Out-Null

$references = @(
    'System.Drawing.Common', 'System.Drawing.Primitives', 'System.Private.Windows.GdiPlus', 'System.Private.Windows.Core',
    'System.Threading.Thread', 'System.Threading', 'System.Diagnostics.Process', 'System.ComponentModel.Primitives',
    'System.Collections', 'System.Runtime.InteropServices', 'System.Console', 'Microsoft.Win32.Primitives', 'System.Memory',
    'System.Linq', 'System.Text.RegularExpressions', 'System.Text.Encoding.Extensions'
)
$sources = (Join-Path $PSScriptRoot 'NppDriver.cs'), (Join-Path $PSScriptRoot 'GuiTests.cs')
Add-Type -Path $sources -ReferencedAssemblies $references -ErrorAction Stop

$failures = [GuiTests]::Run((Join-Path $Npp $Exe), $samples, $shots, $Scenario)
exit $failures
