# Builds a release package. See "Making a release" in docs\DEVELOPMENT.md.
#   dist\xsvViewer-<version>-x64.zip    the DLL at the root, the guide in doc\
#   dist\pl-entry.json                  the entry for the Notepad++ plugin list
$ErrorActionPreference = 'Stop'

& (Join-Path $PSScriptRoot 'build.ps1')

$dist = Join-Path $PSScriptRoot 'dist'
$dll = Join-Path $dist 'xsvViewer\xsvViewer.dll'
$info = (Get-Item $dll).VersionInfo
$version = '{0}.{1}.{2}.{3}' -f $info.FileMajorPart, $info.FileMinorPart, $info.FileBuildPart, $info.FilePrivatePart
$tag = '{0}.{1}.{2}' -f $info.FileMajorPart, $info.FileMinorPart, $info.FileBuildPart

$stage = Join-Path ([IO.Path]::GetTempPath()) ([IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Force (Join-Path $stage 'doc\images') | Out-Null
try {
    Copy-Item $dll $stage
    Copy-Item (Join-Path $PSScriptRoot 'docs\USER_GUIDE.md') (Join-Path $stage 'doc')
    Copy-Item (Join-Path $PSScriptRoot 'docs\images\*.png') (Join-Path $stage 'doc\images')
    Copy-Item (Join-Path $PSScriptRoot 'LICENSE') (Join-Path $stage 'doc')

    $zip = Join-Path $dist "xsvViewer-$tag-x64.zip"
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
} finally {
    Remove-Item $stage -Recurse -Force
}

$entry = [ordered]@{
    'folder-name'             = 'xsvViewer'
    'display-name'            = 'xsvViewer++'
    'version'                 = $version
    'npp-compatible-versions' = '[8.9.8.1,]'
    'id'                      = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
    'repository'              = "https://github.com/mgalhu/xsvViewer/releases/download/v$tag/xsvViewer-$tag-x64.zip"
    'description'             = 'Shows CSV, TSV and other delimited files as an adjustable table in the tab. Detects delimiters, headers, comments and decimal commas. Sort, filter by text, number or regex, move, resize, hide and freeze columns, colour numbers by value, and export the view to CSV, TSV or Markdown. Handles millions of rows.'
    'author'                  = 'Markus Galhuber'
    'homepage'                = 'https://github.com/mgalhu/xsvViewer'
}
$entry | ConvertTo-Json | Set-Content (Join-Path $dist 'pl-entry.json') -Encoding utf8NoBOM

Write-Output "Package: $zip"
Write-Output "SHA-256: $($entry.id)"
