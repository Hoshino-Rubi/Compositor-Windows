#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$FilePath,
    [Parameter(Mandatory)][ValidatePattern('^[0-9A-Fa-f]{40}$')][string]$CertificateThumbprint,
    [Parameter(Mandatory)][uri]$TimestampUrl,
    [string]$SignTool
)
$ErrorActionPreference = 'Stop'
$file = (Resolve-Path -LiteralPath $FilePath).Path
if ([IO.Path]::GetExtension($file) -ne '.exe') { throw 'Expected an executable.' }
if ($TimestampUrl.Scheme -notin 'https','http') { throw 'Expected an HTTP(S) RFC3161 timestamp service.' }
# Only resolve the explicitly selected certificate; never enumerate private stores.
$certificate = Get-Item -LiteralPath "Cert:/CurrentUser/My/$CertificateThumbprint"
if (-not $certificate.HasPrivateKey -or $certificate.NotAfter -le [DateTime]::Now) { throw 'A valid code-signing certificate with its private key is required.' }
$usage = $certificate.Extensions | Where-Object { $_.Oid.Value -eq '2.5.29.37' }
if ('1.3.6.1.5.5.7.3.3' -notin $usage.EnhancedKeyUsages.Value) { throw 'Certificate is not authorized for code signing.' }
if (-not $SignTool) {
    $sdk = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/bin'
    $SignTool = Get-ChildItem -LiteralPath $sdk -Directory | Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName 'x64/signtool.exe' } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $SignTool) { throw 'Windows SDK SignTool not found.' }
& $SignTool sign /sha1 $CertificateThumbprint /fd SHA256 /tr $TimestampUrl.AbsoluteUri /td SHA256 $file
if ($LASTEXITCODE -ne 0) { throw 'Authenticode signing failed.' }
& $SignTool verify /pa /all $file
if ($LASTEXITCODE -ne 0) { throw 'Authenticode verification failed.' }
if ((Get-AuthenticodeSignature -LiteralPath $file).Status -ne 'Valid') { throw 'The resulting signature is not trusted.' }
Write-Output "Verified Authenticode signature: $file"
