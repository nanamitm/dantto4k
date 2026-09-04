<#
.SYNOPSIS
    Builds tsduck.dtv.names from the vendored TSDuck sources.

.DESCRIPTION
    TSDuck's own build concatenates the per-topic .names files into one
    tsduck.dtv.names and installs it beside the binaries; nothing in
    dantto4k.sln does that, so a build of this solution alone never produced
    the file. It is embedded in the binaries as a resource - see
    src/tsduckNames.h - so this runs as a pre-build step of every project that
    carries it.

    The inputs match src/libtsduck/config/Makefile in the TSDuck tree.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $OutFile
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$builder = Join-Path $root 'thirdparty\tsduck\scripts\build-dtv-names.py'
if (-not (Test-Path -LiteralPath $builder)) {
    throw "TSDuck sources not found: $builder (did you clone the submodules?)"
}
if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
    throw "python is required to build tsduck.dtv.names"
}

$inputs = @(
    (Join-Path $root 'thirdparty\tsduck\src\libtsduck\dtv'),
    (Join-Path $root 'thirdparty\tsduck\src\libtsduck\mcast')
) | Where-Object { Test-Path -LiteralPath $_ }

New-Item -ItemType Directory -Force (Split-Path -Parent $OutFile) | Out-Null

# A build environment can leave PSModulePath pointing away from Windows
# PowerShell's own modules, which takes Get-FileHash and Write-Host with it.
# Nothing below reaches outside Core and Management for that reason.
function Test-SameContent([string] $Left, [string] $Right) {
    if (-not (Test-Path -LiteralPath $Right)) {
        return $false
    }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $a = $sha.ComputeHash([System.IO.File]::ReadAllBytes($Left))
        $b = $sha.ComputeHash([System.IO.File]::ReadAllBytes($Right))
    }
    finally {
        $sha.Dispose()
    }
    return [System.Convert]::ToBase64String($a) -eq [System.Convert]::ToBase64String($b)
}

# Both projects that carry the resource run this on the same output, and
# "msbuild /m" runs them at once, so serialize: one generating while the other
# has the file open for the resource compiler is a build that fails at random.
$lock = [System.Threading.Mutex]::new($false, 'Global\dantto4k-generate-tsduck-names')
[void]$lock.WaitOne()
try {
    $staging = "$OutFile.$PID.tmp"
    & python $builder $staging @inputs
    if ($LASTEXITCODE -ne 0) {
        Remove-Item -LiteralPath $staging -ErrorAction SilentlyContinue
        throw "build-dtv-names.py failed with exit code $LASTEXITCODE"
    }

    # Replace the real file only when the contents changed, so an unchanged
    # TSDuck tree does not send the resource compiler and the linker through
    # every build.
    if (Test-SameContent $staging $OutFile) {
        Remove-Item -LiteralPath $staging
    } else {
        Move-Item -LiteralPath $staging -Destination $OutFile -Force
        [Console]::Out.WriteLine("Generated $OutFile")
    }
}
finally {
    if ($staging) {
        Remove-Item -LiteralPath $staging -ErrorAction SilentlyContinue
    }
    $lock.ReleaseMutex()
    $lock.Dispose()
}
