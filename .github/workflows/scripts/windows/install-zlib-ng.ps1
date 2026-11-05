[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$DependencyRoot
)

$ErrorActionPreference = 'Stop'
$dependencyPath = [IO.Path]::GetFullPath($DependencyRoot)
$destination = Join-Path $dependencyPath 'bin\z.dll'
if (-not (Test-Path -LiteralPath $destination -PathType Leaf)) {
    throw 'Install the zlib headers and import library before its zlib-ng runtime.'
}

# Keep the existing zlib import name used by libpng and the Windows dependency set.
$version = '2.3.3'
$url = "https://github.com/zlib-ng/zlib-ng/releases/download/$version/zlib-ng-win-x86-64-compat.zip"
$expectedHash = 'bc8b87f8ed5c149785852c20c7c9c58f0ad4270bfd82e2366687d9dfacf86805'
Add-Type -AssemblyName System.Net.Http
Add-Type -AssemblyName System.IO.Compression
$client = [Net.Http.HttpClient]::new()
try {
    $bytes = $client.GetByteArrayAsync($url).GetAwaiter().GetResult()
}
finally {
    $client.Dispose()
}
$sha256 = [Security.Cryptography.SHA256]::Create()
try {
    $actualHash = [BitConverter]::ToString($sha256.ComputeHash($bytes)).Replace('-', '').ToLowerInvariant()
}
finally {
    $sha256.Dispose()
}
if ($actualHash -ne $expectedHash) {
    throw 'The zlib-ng archive does not match its pinned SHA-256.'
}

$archiveStream = [IO.MemoryStream]::new($bytes, $false)
$archive = [IO.Compression.ZipArchive]::new($archiveStream, [IO.Compression.ZipArchiveMode]::Read)
try {
    $entry = $archive.GetEntry('bin/zlib1.dll')
    if ($null -eq $entry) { throw 'The zlib-ng archive contains no compatibility DLL.' }
    $source = $entry.Open()
    $output = $null
    try {
        $output = [IO.File]::Open($destination, [IO.FileMode]::Open, [IO.FileAccess]::Write, [IO.FileShare]::None)
        $output.SetLength(0)
        $source.CopyTo($output)
        $output.Flush($true)
    }
    finally {
        if ($null -ne $output) { $output.Dispose() }
        $source.Dispose()
    }
}
finally {
    $archive.Dispose()
    $archiveStream.Dispose()
}
Write-Host "Installed pinned zlib-ng $version as the Windows zlib runtime."
