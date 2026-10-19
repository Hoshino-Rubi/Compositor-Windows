#requires -Version 7.0
[CmdletBinding()]
param([switch]$SkipNative)
$ErrorActionPreference='Stop'
$sourceRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repoRoot=[IO.Path]::GetFullPath((Join-Path $sourceRoot '..'))
$lock=Get-Content -LiteralPath (Join-Path $sourceRoot 'dependencies.lock.json') -Raw | ConvertFrom-Json
$native=Join-Path $repoRoot '.tools/vcpkg'
$env:VCPKG_DISABLE_METRICS='1'
if (-not $SkipNative) {
    if (-not (Test-Path -LiteralPath (Join-Path $native '.git'))) { & git clone https://github.com/microsoft/vcpkg.git $native; if ($LASTEXITCODE) {throw 'vcpkg clone failed.'} }
    & git -C $native checkout --detach $lock.vcpkgCommit
    if ($LASTEXITCODE) {throw 'vcpkg revision checkout failed.'}
    if (-not (Test-Path -LiteralPath (Join-Path $native 'vcpkg.exe'))) { & (Join-Path $native 'bootstrap-vcpkg.bat') -disableMetrics; if ($LASTEXITCODE) {throw 'vcpkg bootstrap failed.'} }
    # GNU configure/libtool cannot reliably quote a workspace containing spaces.
    # SUBST maps only this repository; no files are moved and the mapping is removed on exit.
    $drive = $null
    $buildRoot = $repoRoot
    try {
        if ($repoRoot.Contains(' ')) {
            $letter = @('Z','Y','X','W','V','U','T','S','R') | Where-Object { -not (Test-Path ($_.ToString() + ':\')) } | Select-Object -First 1
            if (-not $letter) { throw 'A free drive letter is required to build native dependencies in a path with spaces.' }
            $drive = $letter + ':'
            & subst $drive $repoRoot
            if ($LASTEXITCODE) { $drive = $null; throw 'Cannot map the workspace for native dependency builds.' }
            $buildRoot = $drive + '/'
        }
        $buildNative = Join-Path $buildRoot '.tools/vcpkg'
        & (Join-Path $buildNative 'vcpkg.exe') install --triplet x64-windows "--vcpkg-root=$buildNative" "--x-manifest-root=$(Join-Path $buildRoot 'windows')" "--x-install-root=$(Join-Path $buildNative 'installed')" "--x-buildtrees-root=$(Join-Path $buildNative 'buildtrees-native')" "--x-packages-root=$(Join-Path $buildNative 'packages')"
        if ($LASTEXITCODE) {throw 'Native dependency build failed.'}
    } finally {
        if ($drive) { & subst $drive /D }
    }
}
function Get-VerifiedFile([string]$Url,[string]$Path,[string]$Hash) {
    if (Test-Path -LiteralPath $Path) {if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ieq $Hash) {return}}
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($Path)) -Force | Out-Null
    $temporary=$Path+'.download'
    Invoke-WebRequest -Uri $Url -OutFile $temporary
    if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash -ine $Hash) {throw "Checksum mismatch: $Url"}
    Move-Item -LiteralPath $temporary -Destination $Path -Force
}
$onnx=Join-Path $repoRoot '.tools/onnxruntime'
$archive=Join-Path $onnx 'package.zip'
Get-VerifiedFile $lock.onnxUrl $archive $lock.onnxSha256
Expand-Archive -LiteralPath $archive -DestinationPath (Join-Path $onnx 'package') -Force
Get-VerifiedFile $lock.modelUrl (Join-Path $sourceRoot 'resources/models/u2netp.onnx') $lock.modelSha256
Write-Output 'Pinned native decoders, ONNX Runtime and local segmentation model are ready.'
