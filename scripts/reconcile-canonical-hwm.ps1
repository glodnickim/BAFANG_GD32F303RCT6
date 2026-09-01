param(
    [Parameter(Mandatory=$true)]
    [ValidatePattern('^0\.\d{4}$')]
    [string]$AuthoritativeVersion,
    [Parameter(Mandatory=$true)]
    [ValidateNotNullOrEmpty()]
    [string]$Evidence,
    [string]$StateRoot = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
Import-Module (Join-Path $PSScriptRoot "build-version-allocator.psm1") -Force
$authoritativeHwm = [int]$AuthoritativeVersion.Substring(2)
$result = Sync-EbicsCanonicalHwm -RepoRoot $repoRoot -StateRoot $StateRoot -AuthoritativeHwm $authoritativeHwm -Evidence $Evidence

Write-Host "PERSISTENT STATE ROOT: $($result.Root)"
Write-Host "PERSISTENT HWM BEFORE: 0.$($result.PreviousHwm.ToString('D4'))"
Write-Host "AUTHORITATIVE ISSUED HWM: 0.$($result.Hwm.ToString('D4'))"
Write-Host "HWM MIGRATION: $(if ($result.Changed) { 'PASS' } else { 'NOT REQUIRED' })"
Write-Host "ATOMIC LOCK: $($result.Atomic)"
Write-Host "NEW VERSION RESERVED: NO"
