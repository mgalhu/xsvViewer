# Installs the plugin. Build first with .\build.ps1
#
#   .\install.ps1 -Portable    sets up a portable Notepad++ with the plugin in .\portable
#                              (no administrator rights needed)
#   .\install.ps1              copies the plugin into the installed Notepad++
#                              (needs administrator rights; asks for them if necessary)
param(
    [switch] $Portable,
    [string] $NppDir = (Join-Path $env:ProgramFiles 'Notepad++'),
    [string] $Version = '8.9.8.1'
)
$ErrorActionPreference = 'Stop'

$dll = Join-Path $PSScriptRoot 'dist\xsvViewer\xsvViewer.dll'
if (-not (Test-Path $dll)) { throw 'dist\xsvViewer\xsvViewer.dll is missing. Run .\build.ps1 first.' }

if ($Portable) {
    $target = Join-Path $PSScriptRoot 'portable'
    if (-not (Test-Path (Join-Path $target 'notepad++.exe'))) {
        $release = "https://github.com/notepad-plus-plus/notepad-plus-plus/releases/download/v$Version"
        $asset = "npp.$Version.portable.x64.zip"
        $download = Join-Path ([IO.Path]::GetTempPath()) ([IO.Path]::GetRandomFileName())
        New-Item -ItemType Directory -Force $download | Out-Null
        try {
            Invoke-WebRequest "$release/$asset" -OutFile (Join-Path $download $asset) -UseBasicParsing
            Invoke-WebRequest "$release/npp.$Version.checksums.sha256" -OutFile (Join-Path $download 'sums.txt') -UseBasicParsing

            $line = Get-Content (Join-Path $download 'sums.txt') | Where-Object { $_ -match [regex]::Escape($asset) }
            if (-not $line) { throw "No checksum published for $asset." }
            $expected = ($line -split '\s+')[0]
            $actual = (Get-FileHash (Join-Path $download $asset) -Algorithm SHA256).Hash
            if ($actual -ne $expected) { throw "Checksum mismatch for $asset." }

            Expand-Archive (Join-Path $download $asset) -DestinationPath $target
        } finally {
            Remove-Item $download -Recurse -Force
        }
    }
    New-Item -ItemType Directory -Force (Join-Path $target 'plugins\xsvViewer') | Out-Null
    Copy-Item $dll (Join-Path $target 'plugins\xsvViewer') -Force
    Write-Output "Ready: $(Join-Path $target 'notepad++.exe')"
    exit 0
}

$plugins = Join-Path $NppDir 'plugins'
if (-not (Test-Path $plugins)) { throw "No Notepad++ found at $NppDir. Pass -NppDir, or use -Portable." }
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
