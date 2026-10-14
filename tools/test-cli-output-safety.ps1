param([string]$Executable = "$PSScriptRoot\..\msvc\x64\Release\dantto4k.exe")
$ErrorActionPreference = 'Stop'
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$testRoot = [IO.Path]::GetFullPath("$PSScriptRoot\..\work\regression-tests")
$testDir = Join-Path $testRoot ("cli-output-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $testDir -Force | Out-Null
$process = $null
try {
    $inputPath = Join-Path $testDir 'input.mmts'
    $bytes = [byte[]]::new(2 * 1024 * 1024)
    for ($i = 0; $i -lt $bytes.Length; $i += 4) { $bytes[$i] = 0x7f; $bytes[$i + 1] = 0xff }
    [IO.File]::WriteAllBytes($inputPath, $bytes)
    $outputPath = Join-Path $testDir 'output.mmts'
    & $Executable --frontend-descrambled --no-progress --no-stats --decode-mmts $inputPath $outputPath
    if ($LASTEXITCODE -ne 0 -or [Convert]::ToHexString([IO.File]::ReadAllBytes($outputPath)) -ne [Convert]::ToHexString($bytes)) {
        throw 'Successful output must include the complete final data'
    }
    $start = [Diagnostics.ProcessStartInfo]::new($Executable)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($argument in @('--frontend-descrambled', '--no-progress', '--no-stats', '--decode-mmts', $inputPath, '-')) {
        $start.ArgumentList.Add($argument)
    }
    $process = [Diagnostics.Process]::Start($start)
    $errorTask = $process.StandardError.ReadToEndAsync()
    # Simulate a pipeline consumer exiting before accepting all output.
    $process.StandardOutput.BaseStream.Dispose()
    if (!$process.WaitForExit(30000)) { throw 'Closed output pipe must terminate conversion promptly' }
    if ($process.ExitCode -eq 0) { throw 'Closed output pipe was reported as a successful conversion' }
    if ($errorTask.GetAwaiter().GetResult() -notmatch 'Unable to finish writing output') {
        throw 'Output failure must include a diagnostic'
    }
    Write-Output 'PASS: complete CLI output and nonzero exit on broken stdout pipe'
}
finally {
    if ($process) {
        if (!$process.HasExited) { $process.Kill(); $process.WaitForExit() }
        $process.Dispose()
    }
    $resolved = [IO.Path]::GetFullPath($testDir)
    if (![string]::Equals([IO.Path]::GetDirectoryName($resolved), $testRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Test cleanup path is outside the regression test directory'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
