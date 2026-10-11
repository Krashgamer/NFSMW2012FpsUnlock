<#
.SYNOPSIS
    Builds the drag-and-drop release archive from a built MW2012FpsUnlock.asi.

.DESCRIPTION
    Downloads Ultimate ASI Loader (pinned version, verified by SHA-256) and
    puts it next to the plugin, its INI, a short readme, the loader settings
    and both licenses:

        dinput8.dll
        scripts\global.ini
        scripts\MW2012FpsUnlock.asi
        scripts\MW2012FpsUnlock.ini
        scripts\MW2012FpsUnlock-README.txt
        scripts\MW2012FpsUnlock-LICENSE.txt       (MIT: must travel with the .asi)
        scripts\Ultimate-ASI-Loader-LICENSE.txt   (MIT: must travel with dinput8.dll)

    Used by the GitHub workflow; runs the same way locally after a Release
    build. Prints the archive path and its SHA-256.

.PARAMETER ExpectTag
    When set (a tag such as v1.0.0), fail unless it matches the version in
    src\version.h.
#>
param(
    [string]$Asi = "build\Release\MW2012FpsUnlock.asi",
    [string]$OutDir = "build\dist",
    [string]$ExpectTag = ""
)

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)

# Pinned loader release. Update all three together.
$LoaderVersion = '9.7.4'
$LoaderUrl     = "https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/v$LoaderVersion/Ultimate-ASI-Loader-NoPDB.zip"
$LoaderZipHash = '14b3a1ad018899571ac9aa01482977f3c6d49e6cba99f552d01c5acacd1315e1'
$LoaderDllHash = 'ec2f4824eca58dd40f425756a4a7cec77b8e381f21d11d7c846ec4b339b617ab'

function Get-Sha256([string]$Path) { (Get-FileHash -Algorithm SHA256 $Path).Hash.ToLowerInvariant() }

# Version from src\version.h.
$header = Get-Content 'src\version.h' -Raw
$parts = foreach ($name in 'MAJOR', 'MINOR', 'PATCH') {
    if ($header -notmatch "#define MW_VERSION_$name (\d+)") { throw "MW_VERSION_$name not found in src\version.h" }
    $Matches[1]
}
$version = $parts -join '.'
if ($ExpectTag -and $ExpectTag -ne "v$version") {
    throw "Tag $ExpectTag does not match src\version.h (v$version)"
}

if (-not (Test-Path $Asi)) { throw "$Asi not found - build the Release configuration first" }

# Loader download, cached between local runs.
$cache = 'build\cache'
New-Item -ItemType Directory -Force $cache | Out-Null
$loaderZip = Join-Path $cache "Ultimate-ASI-Loader-$LoaderVersion.zip"
if (-not (Test-Path $loaderZip) -or (Get-Sha256 $loaderZip) -ne $LoaderZipHash) {
    Write-Host "Downloading Ultimate ASI Loader v$LoaderVersion"
    for ($attempt = 1; ; $attempt++) {
        try {
            Invoke-WebRequest -Uri $LoaderUrl -OutFile $loaderZip -UseBasicParsing
            break
        }
        catch {
            if ($attempt -ge 3) { throw }
            Write-Host "Download failed ($($_.Exception.Message)), retrying"
            Start-Sleep -Seconds (5 * $attempt)
        }
    }
}
$zipHash = Get-Sha256 $loaderZip
if ($zipHash -ne $LoaderZipHash) { throw "Loader archive SHA-256 mismatch: $zipHash" }

# Stage the archive contents.
$stage = Join-Path $OutDir 'stage'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
$scripts = Join-Path $stage 'scripts'
New-Item -ItemType Directory -Force $scripts | Out-Null

Expand-Archive -Path $loaderZip -DestinationPath (Join-Path $OutDir 'loader') -Force
$dll = Join-Path $OutDir 'loader\dinput8.dll'
$dllHash = Get-Sha256 $dll
if ($dllHash -ne $LoaderDllHash) { throw "dinput8.dll SHA-256 mismatch: $dllHash" }

Copy-Item $dll (Join-Path $stage 'dinput8.dll')
Copy-Item 'dist\scripts\global.ini' $scripts
Copy-Item $Asi $scripts
Copy-Item 'MW2012FpsUnlock.ini' $scripts
Copy-Item 'dist\scripts\MW2012FpsUnlock-README.txt' $scripts
Copy-Item 'LICENSE' (Join-Path $scripts 'MW2012FpsUnlock-LICENSE.txt')
Copy-Item 'dist\licenses\Ultimate-ASI-Loader.txt' (Join-Path $scripts 'Ultimate-ASI-Loader-LICENSE.txt')

# Zip with forward-slash entry names (what every unzipper expects).
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$zipPath = Join-Path (Resolve-Path $OutDir) "MW2012FpsUnlock-v$version.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
$stageFull = (Resolve-Path $stage).Path
$zip = [System.IO.Compression.ZipFile]::Open($zipPath, 'Create')
try {
    Get-ChildItem $stageFull -Recurse -File | Sort-Object FullName | ForEach-Object {
        $entry = $_.FullName.Substring($stageFull.Length + 1).Replace('\', '/')
        [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $zip, $_.FullName, $entry, [System.IO.Compression.CompressionLevel]::Optimal)
    }
}
finally {
    $zip.Dispose()
}

Write-Host "Archive: $zipPath"
Write-Host "SHA-256: $(Get-Sha256 $zipPath)"
if ($env:GITHUB_OUTPUT) {
    "zip=$zipPath" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
    "version=$version" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
}
