#requires -Version 7.0
[CmdletBinding()]
param([string]$QtDir = $env:QT_DIR, [string]$BuildDir, [switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repoRoot = [IO.Path]::GetFullPath((Join-Path $sourceRoot '..'))
if (-not $QtDir) { $QtDir = Join-Path $repoRoot '.tools/Qt/6.8.3/msvc2022_64' }
$QtDir = [IO.Path]::GetFullPath($QtDir)
if (-not $BuildDir) { $BuildDir = Join-Path $sourceRoot 'build-native' }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
if (-not $SkipBuild) { & (Join-Path $PSScriptRoot 'build.ps1') -QtDir $QtDir -BuildDir $BuildDir }
$distributionRoot = Join-Path $sourceRoot 'dist'
# A fresh staging directory prevents old plugins from leaking into a release.
$stage = Join-Path $distributionRoot ('Compositor-0.4.1a9-win-x64-' + [guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $BuildDir 'Release/Compositor.exe') -Destination $stage
$env:PATH = (Join-Path $QtDir 'bin') + [IO.Path]::PathSeparator + $env:PATH
$vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio/Installer/vswhere.exe'
if (Test-Path -LiteralPath $vswhere) {
    $vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vsRoot) { $env:VCINSTALLDIR = (Join-Path $vsRoot 'VC') + [IO.Path]::DirectorySeparatorChar }
}
& (Join-Path $QtDir 'bin/windeployqt.exe') --release --no-compiler-runtime --no-quick-import --no-opengl-sw --no-system-d3d-compiler --translations zh_CN,zh_TW,ja,ko,de,fr,es --skip-plugin-types generic,networkinformation,iconengines --exclude-plugins qgif,qicns,qico,qtga,qwbmp (Join-Path $stage 'Compositor.exe')
if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed.' }
$crt = Get-ChildItem -LiteralPath (Join-Path $env:VCINSTALLDIR 'Redist/MSVC') -Directory | Where-Object Name -Match '^\d+\.\d+' | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $crt) { throw 'MSVC redistributable DLLs not found.' }
Copy-Item -Path (Join-Path $crt.FullName 'x64/Microsoft.VC143.CRT/*.dll') -Destination $stage
foreach ($name in 'vcruntime140.dll','vcruntime140_1.dll','msvcp140.dll') {
    if (-not (Test-Path -LiteralPath (Join-Path $stage $name))) { throw "Missing runtime: $name" }
}
$native = Join-Path $repoRoot '.tools/vcpkg/installed/x64-windows'
foreach ($dll in 'raw_r.dll','jasper.dll','jpeg62.dll','lcms2-2.dll','heif.dll','libde265.dll','z.dll') {
    Copy-Item -LiteralPath (Join-Path $native "bin/$dll") -Destination $stage
}
$onnx = Join-Path $repoRoot '.tools/onnxruntime/package'
foreach ($dll in 'onnxruntime.dll','onnxruntime_providers_shared.dll') {
    Copy-Item -LiteralPath (Join-Path $onnx "runtimes/win-x64/native/$dll") -Destination $stage
}
Copy-Item -LiteralPath (Join-Path $sourceRoot 'resources/models') -Destination $stage -Recurse
$licenses = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE') -Destination (Join-Path $licenses 'Compositor-MIT.txt')
Copy-Item -Path (Join-Path $sourceRoot 'resources/licenses/*') -Destination $licenses
foreach ($library in 'libraw','jasper','libjpeg-turbo','lcms','libheif','libde265','zlib') {
    Copy-Item -LiteralPath (Join-Path $native "share/$library/copyright") -Destination (Join-Path $licenses "$library-NOTICE.txt")
}
Copy-Item -LiteralPath (Join-Path $onnx 'LICENSE') -Destination (Join-Path $licenses 'ONNXRuntime-MIT.txt')
Copy-Item -LiteralPath (Join-Path $onnx 'ThirdPartyNotices.txt') -Destination (Join-Path $licenses 'ONNXRuntime-ThirdPartyNotices.txt')
# The SDK's SPDX records include the notices for Qt's bundled third-party libraries.
foreach ($module in 'qtbase','qtimageformats','qtsvg') {
    $sbom = Join-Path $QtDir "sbom/$module-6.8.3.spdx.json"
    if (Test-Path -LiteralPath $sbom) { Copy-Item -LiteralPath $sbom -Destination $licenses }
}
Copy-Item -LiteralPath (Join-Path $sourceRoot 'README.zh-CN.md') -Destination (Join-Path $stage 'README.zh-CN.md')
Copy-Item -LiteralPath (Join-Path $sourceRoot 'README.md') -Destination (Join-Path $stage 'README.md')
New-Item -ItemType Directory -Path (Join-Path $stage 'releases') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $sourceRoot 'releases/0.4.1a9.md') -Destination (Join-Path $stage 'releases/0.4.1a9.md')
$archive = Join-Path $distributionRoot 'Compositor-0.4.1a9-win-x64.zip'
Copy-Item -LiteralPath (Join-Path $sourceRoot 'dependencies.lock.json') -Destination $stage
Compress-Archive -LiteralPath $stage -DestinationPath $archive -CompressionLevel Optimal -Force
Write-Output "Application: $(Join-Path $stage 'Compositor.exe')"
Write-Output "Archive: $archive"

(Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash | Set-Content -LiteralPath ($archive+'.sha256') -Encoding utf8
