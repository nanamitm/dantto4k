param([string]$Executable = "$PSScriptRoot\..\msvc\x64\Release\dantto4k.exe")
$ErrorActionPreference = 'Stop'
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$testDir = Join-Path ([IO.Path]::GetTempPath()) ("dantto4k-paths-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $testDir | Out-Null
Push-Location $testDir
try {
    $bytes = [byte[]](0x7f, 0xff, 0, 0)
    [IO.File]::WriteAllBytes("$testDir\input.mmts", $bytes)
    [IO.File]::WriteAllBytes("$testDir\output.mmts", $bytes)
    New-Item -ItemType HardLink -Path "$testDir\linked.mmts" -Target "$testDir\input.mmts" | Out-Null
    $cases = @(
        @('--decode-mmts', 'input.mmts', '.\input.mmts'),
        @('--decode-mmts', 'input.mmts', 'INPUT.MMTS'),
        @('--decode-mmts', 'input.mmts', 'linked.mmts'),
        @('--write-mmtsmap-only', '--mmtsmap', 'input.mmts', 'input.mmts'),
        @('--decode-mmts', '--mmtsmap', '.\input.mmts', 'input.mmts', 'output.mmts'),
        @('--decode-mmts', '--mmtsmap', '.\output.mmts', 'input.mmts', 'output.mmts')
    )
    foreach ($arguments in $cases) {
        & $Executable --frontend-descrambled --no-progress --no-stats @arguments
        if ($LASTEXITCODE -eq 0) { throw "Accepted colliding paths: $arguments" }
        foreach ($name in @('input.mmts', 'output.mmts')) {
            if ([Convert]::ToHexString([IO.File]::ReadAllBytes("$testDir\$name")) -ne '7FFF0000') {
                throw "Changed $name while rejecting: $arguments"
            }
        }
    }
    & $Executable --frontend-descrambled --no-progress --no-stats --decode-mmts --write-mmtsmap input.mmts valid.mmts
    if ($LASTEXITCODE -ne 0 -or [Convert]::ToHexString([IO.File]::ReadAllBytes("$testDir\valid.mmts")) -ne '7FFF0000') {
        throw 'Valid conversion failed'
    }
    if (!(Test-Path -LiteralPath "$testDir\valid.mmtsmap")) { throw 'Missing map' }
    Write-Output 'PASS: six path collisions preserve files; distinct outputs convert successfully'
}
finally {
    Pop-Location
    # Remove only the uniquely created test directory inside the temp directory.
    $resolvedTestDir = [IO.Path]::GetFullPath($testDir)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if (!$resolvedTestDir.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Test cleanup path is outside the temporary directory'
    }
    Remove-Item -LiteralPath $resolvedTestDir -Recurse -Force
}
