#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$QtDir = $env:QT_DIR,
    [string]$BuildDir,
    [ValidateSet('Release','Debug')][string]$Configuration = 'Release',
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repoRoot = [IO.Path]::GetFullPath((Join-Path $sourceRoot '..'))
if (-not $QtDir) { $QtDir = Join-Path $repoRoot '.tools/Qt/6.8.3/msvc2022_64' }
$QtDir = [IO.Path]::GetFullPath($QtDir)
if (-not (Test-Path -LiteralPath (Join-Path $QtDir 'bin/Qt6Widgets.dll'))) {
    throw 'Qt 6.8+ MSVC x64 SDK not found. Pass -QtDir or set QT_DIR.'
}
if (-not $BuildDir) { $BuildDir = Join-Path $sourceRoot 'build-native' }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
$deps = Join-Path $repoRoot '.tools/vcpkg/installed/x64-windows'
$onnx = Join-Path $repoRoot '.tools/onnxruntime/package'
if (-not (Test-Path -LiteralPath (Join-Path $deps 'bin/raw_r.dll')) -or -not (Test-Path -LiteralPath (Join-Path $onnx 'runtimes/win-x64/native/onnxruntime.dll'))) {
    & (Join-Path $PSScriptRoot 'prepare-dependencies.ps1')
}
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = Join-Path $env:ProgramFiles 'CMake/bin/cmake.exe' }
$ctest = Join-Path ([IO.Path]::GetDirectoryName($cmake)) 'ctest.exe'
# Reuse the upstream icon as a Windows ICO with a PNG payload.
$png = [IO.File]::ReadAllBytes((Join-Path $repoRoot 'Compositor/Assets.xcassets/AppIcon.appiconset/app-icon-256.png'))
$memory = [IO.MemoryStream]::new()
$writer = [IO.BinaryWriter]::new($memory)
$writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]1)
$writer.Write([byte]0); $writer.Write([byte]0); $writer.Write([byte]0); $writer.Write([byte]0)
$writer.Write([uint16]1); $writer.Write([uint16]32)
$writer.Write([uint32]$png.Length); $writer.Write([uint32]22); $writer.Write($png)
[IO.File]::WriteAllBytes((Join-Path $sourceRoot 'resources/app.ico'), $memory.ToArray())
$writer.Dispose(); $memory.Dispose()
& $cmake -S $sourceRoot -B $BuildDir -G 'Visual Studio 17 2022' -A x64 -T host=x64 "-DCMAKE_PREFIX_PATH=$QtDir" "-DCOMPOSITOR_DEPS=$deps" "-DCOMPOSITOR_ONNX=$onnx" -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $cmake --build $BuildDir --config $Configuration --parallel 6
if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
if (-not $SkipTests) {
    $env:PATH = (Join-Path $QtDir 'bin') + [IO.Path]::PathSeparator + (Join-Path $repoRoot '.tools/vcpkg/installed/x64-windows/bin') + [IO.Path]::PathSeparator + (Join-Path $repoRoot '.tools/onnxruntime/package/runtimes/win-x64/native') + [IO.Path]::PathSeparator + $env:PATH
    & $ctest --test-dir $BuildDir -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Native tests failed.' }
}
Write-Output (Join-Path $BuildDir "$Configuration/Compositor.exe")
