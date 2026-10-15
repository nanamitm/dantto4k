# Writes the version this build comes from to a C/C++ header, the way TVTest's
# TVTestVersionHashGen.bat does: a pre-build step runs it, the header is not
# committed (.gitignore) and the sources include it with __has_include.
#
# The version is `git describe --tags --always --dirty`, e.g. "v20261015a"
# for a tagged commit, "v20261015a-2-gce5bea5" two commits after it, with
# "-dirty" when there are uncommitted changes, and "unknown" without git. The
# header is rewritten only when it changes, so it does not cause rebuilds.
#
#   #define <Name>_VERSION_STRING   "v20261015a-2-gce5bea5"
#   #define <Name>_VERSION_STRING_W L"v20261015a-2-gce5bea5"
#   #define <Name>_VERSION_NUMBERS  2026,10,15,2   (VERSIONINFO; 0,0,0,0 without a vYYYYMMDD tag)
param(
    [Parameter(Mandatory)] [string]$OutFile,
    [Parameter(Mandatory)] [string]$Name
)
$ErrorActionPreference = 'Stop'

$version = $null
try {
    $output = & git -C $PSScriptRoot describe --tags --always --dirty 2>$null
    if ($LASTEXITCODE -eq 0 -and $output) {
        $version = "$output".Trim()
    }
}
catch {
    # git is not installed
}
if (-not $version) {
    $version = 'unknown'
}

$numbers = '0,0,0,0'
if ($version -match '^v(\d{4})(\d{2})(\d{2})[a-z]?(?:-(\d+)-g)?') {
    $commits = if ($Matches[4]) { [int]$Matches[4] } else { 0 }
    $numbers = '{0},{1},{2},{3}' -f [int]$Matches[1], [int]$Matches[2], [int]$Matches[3], [Math]::Min($commits, 65535)
}

$content = "#define ${Name}_VERSION_STRING `"$version`"`r`n" +
    "#define ${Name}_VERSION_STRING_W L`"$version`"`r`n" +
    "#define ${Name}_VERSION_NUMBERS $numbers`r`n"
# Projects built in parallel may run this for the same header at the same
# time: take turns, so that one writes it and the others find it current.
$fullPath = [IO.Path]::GetFullPath($OutFile)
$sha = [Security.Cryptography.SHA1]::Create()
$key = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($fullPath.ToLowerInvariant()))).Replace('-', '')
$mutex = New-Object Threading.Mutex($false, "Local\generate-version-header-$key")
try {
    try {
        [void]$mutex.WaitOne()
    }
    catch [Threading.AbandonedMutexException] {
        # a previous run was killed while holding it
    }
    if ((Test-Path -LiteralPath $fullPath) -and [IO.File]::ReadAllText($fullPath) -ceq $content) {
        return
    }
    $directory = Split-Path -Parent $fullPath
    if ($directory) {
        New-Item -ItemType Directory -Force $directory | Out-Null
    }
    [IO.File]::WriteAllText($fullPath, $content)
    Write-Host "${Name} version: $version"
}
finally {
    $mutex.ReleaseMutex()
    $mutex.Dispose()
}
