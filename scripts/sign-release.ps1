param(
    [Parameter(Mandatory=$true)]
    [ValidatePattern('^[A-Fa-f0-9]{40}$')]
    [string]$CertificateThumbprint,
    [Parameter(Mandatory=$true)]
    [ValidatePattern('^https?://[^\s]+$')]
    [string]$TimestampUrl,
    [string]$File = (Join-Path $PSScriptRoot '..\build\better_planner.exe'),
    [string]$SignTool
)
$ErrorActionPreference = 'Stop'
if (-not $SignTool) {
    $found = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($found) { $SignTool = $found.Source }
    else {
        $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
        $SignTool = Get-ChildItem -LiteralPath $kits -Filter signtool.exe -Recurse |
            Where-Object { $_.Directory.Name -eq 'x64' } |
            Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
    }
}
if (-not $SignTool) { throw 'Install the Windows SDK signing tools first.' }
$target = (Resolve-Path -LiteralPath $File).Path
& $SignTool sign /sha1 $CertificateThumbprint /fd SHA256 /tr $TimestampUrl /td SHA256 $target
if ($LASTEXITCODE -ne 0) { throw 'Signing failed. Check the certificate, private key provider and timestamp service.' }
& $SignTool verify /pa /all /v $target
if ($LASTEXITCODE -ne 0) { throw 'Signature verification failed.' }
Write-Host "Signed and verified: $target"
