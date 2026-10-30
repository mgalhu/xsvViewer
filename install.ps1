# Copies the built plugin into Notepad++'s plugins folder, the way the Notepad++ manual describes
# for a manual install. Build first with .\build.ps1. Needs administrator rights, and asks for them.
param([string] $NppDir = (Join-Path $env:ProgramFiles 'Notepad++'))
$ErrorActionPreference = 'Stop'

$dll = Join-Path $PSScriptRoot 'dist\xsvViewer\xsvViewer.dll'
if (-not (Test-Path $dll)) { throw 'dist\xsvViewer\xsvViewer.dll is missing. Run .\build.ps1 first.' }

$plugins = Join-Path $NppDir 'plugins'
if (-not (Test-Path $plugins)) { throw "No Notepad++ found at $NppDir. Pass -NppDir." }
if (Get-Process notepad++ -ErrorAction SilentlyContinue) { throw 'Close Notepad++ first; it keeps its plugins locked.' }

$identity = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -NppDir `"$NppDir`""
    Start-Process pwsh -Verb RunAs -ArgumentList $arguments -Wait
    exit 0
}

New-Item -ItemType Directory -Force (Join-Path $plugins 'xsvViewer') | Out-Null
Copy-Item $dll (Join-Path $plugins 'xsvViewer') -Force
Write-Output "Installed into $(Join-Path $plugins 'xsvViewer'). Start Notepad++ to use it."
