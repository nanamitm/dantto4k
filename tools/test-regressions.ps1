param(
    [ValidateSet('paths', 'acas', 'casproxy', 'demux', 'bontuner', 'ttml')]
    [string[]]$Tests = @('paths', 'acas', 'casproxy', 'demux', 'bontuner', 'ttml'),
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..").Path
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw 'Visual Studio C++ x64 tools are required' }
$msbuild = Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'
if (!$SkipBuild) {
    & $msbuild "$repo\msvc\dantto4k.sln" /m /p:Configuration=Release /p:Platform=x64 /nodeReuse:false /v:minimal
    if ($LASTEXITCODE -ne 0) { throw 'Release build failed' }
}
$objectDir = "$repo\msvc\x64\Release\dantto4k"
$tsduck = "$repo\thirdparty\tsduck"
$buildDir = "$repo\work\regression-tests"
New-Item -ItemType Directory -Force $buildDir | Out-Null
$includeDirs = @("$repo\src", "$repo\thirdparty\asio\asio\include", "$tsduck\bin\include")
if ($Tests -contains 'bontuner') {
    foreach ($library in @('libtscore', 'libtsduck')) {
        $includeDirs += Get-ChildItem "$tsduck\src\$library" -Recurse -Filter '*.h' |
            Where-Object { $_.DirectoryName -notmatch '\\(unix|linux|mac|bsd|private|__pycache__)(\\|$)' } |
            ForEach-Object { $_.DirectoryName } | Sort-Object -Unique
    }
}
Push-Location $buildDir
try {
    foreach ($test in $Tests) {
        if ($test -eq 'paths') {
            & "$PSScriptRoot\test-file-path-safety.ps1"
            continue
        }
        $name = switch ($test) {
            acas { 'acas_safety_test' }
            casproxy { 'cas_proxy_safety_test' }
            demux { 'demux_safety_test' }
            bontuner { 'bontuner_safety_test' }
            ttml { 'ttml_test' }
        }
        $objects = switch ($test) {
            acas { @('acasHandler', 'acasCard', 'aes', 'config') | ForEach-Object { Get-Item "$objectDir\$_.obj" } }
            casproxy { Get-Item "$objectDir\casProxyClient.obj" }
            default {
                $excluded = @('dantto4k', 'dllmain')
                if ($test -ne 'bontuner') { $excluded += @('bonTuner', 'remuxerHandler', 'tsduckNames') }
                Get-ChildItem $objectDir -Filter '*.obj' | Where-Object { $_.BaseName -notin $excluded }
            }
        }
        $compilerArgs = @('/nologo', '/O2', '/MD', '/EHsc', '/std:c++20', '/utf-8',
            '/DWIN32', '/DWIN32_LEAN_AND_MEAN', '/D_WIN32_WINNT=0x0601',
            '/DTSDUCK_STATIC_LIBRARY', '/DTSCORE_STATIC_LIBRARY')
        $compilerArgs += $includeDirs | ForEach-Object { '/I"' + $_ + '"' }
        $compilerArgs += '"' + "$PSScriptRoot\$name.cpp" + '"'
        $compilerArgs += $objects | ForEach-Object { '"' + $_.FullName + '"' }
        $compilerArgs += @("/Fe:$name.exe", '/link', 'winscard.lib', 'gdi32.lib', 'user32.lib')
        if ($test -eq 'bontuner') {
            $compilerArgs += @('/LIBPATH:"' + "$tsduck\bin\Release-x64" + '"',
                'tsducklib.lib', 'tscorelib.lib', '/wholearchive:tsducklib.lib', '/wholearchive:tscorelib.lib',
                'ole32.lib', 'oleaut32.lib', 'advapi32.lib')
        }
        Set-Content "$name.rsp" ($compilerArgs -join ' ') -Encoding utf8
        $commands = @('@echo off',
            ('call "' + "$installation\VC\Auxiliary\Build\vcvars64.bat" + '" >nul'),
            "cl @$name.rsp", 'if errorlevel 1 exit /b 1', "$name.exe")
        Set-Content "$name.cmd" $commands -Encoding ascii
        & ".\$name.cmd"
        if ($LASTEXITCODE -ne 0) { throw "$name failed (exit $LASTEXITCODE)" }
    }
    Write-Output 'PASS: all requested regression tests'
}
finally { Pop-Location }
