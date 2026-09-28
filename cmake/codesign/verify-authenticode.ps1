param(
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][string]$ExpectedSignerSha256,
    [Parameter(Mandatory = $true)][string]$ExpectedRootSha256
)
$ErrorActionPreference = 'Stop'

$signature = Get-AuthenticodeSignature -LiteralPath $Path
if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
    throw "Authenticode signature is not valid: $($signature.Status)"
}
$sha = [System.Security.Cryptography.SHA256]::Create()
try {
    $signerHash = [BitConverter]::ToString($sha.ComputeHash($signature.SignerCertificate.RawData)).Replace('-', '')
    if ($signerHash -ne $ExpectedSignerSha256.ToUpperInvariant()) {
        throw 'Signer certificate fingerprint does not match the pinned identity'
    }
    $chain = [System.Security.Cryptography.X509Certificates.X509Chain]::new()
    $chain.ChainPolicy.RevocationMode = [System.Security.Cryptography.X509Certificates.X509RevocationMode]::NoCheck
    if (-not $chain.Build($signature.SignerCertificate)) {
        throw "Signer chain is not trusted: $($chain.ChainStatus.Status)"
    }
    $rootCert = $chain.ChainElements[$chain.ChainElements.Count - 1].Certificate
    $rootHash = [BitConverter]::ToString($sha.ComputeHash($rootCert.RawData)).Replace('-', '')
    if ($rootHash -ne $ExpectedRootSha256.ToUpperInvariant()) {
        throw 'Trusted root certificate fingerprint does not match the pinned identity'
    }
} finally {
    $sha.Dispose()
}
