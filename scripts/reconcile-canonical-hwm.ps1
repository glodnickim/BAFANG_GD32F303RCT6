param(
    [Parameter(Mandatory=$true)]
    # Three decimals since 0.498: the HMI shows exactly three, so a four-digit number could not be
    # read off the bike at all. Older releases were issued as 0.0xxx and are the SAME counter -
    # 0.0496 is written 0.496 in this notation. See documentation/BUILD_FIRMWARE_PL.md.
    [ValidatePattern('^\d+\.\d{3}$')]
    [string]$AuthoritativeVersion,
    [Parameter(Mandatory=$true)]
    [ValidateNotNullOrEmpty()]
    [string]$Evidence,
    [string]$StateRoot = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
Import-Module (Join-Path $PSScriptRoot "build-version-allocator.psm1") -Force
# Parsed from both halves, not by chopping the first two characters: that trick only worked while
# every version started with "0." and would read 1.000 as zero the moment the counter passes 999.
$versionParts = $AuthoritativeVersion.Split('.')
$authoritativeHwm = ([int]$versionParts[0] * 1000) + [int]$versionParts[1]
$result = Sync-EbicsCanonicalHwm -RepoRoot $repoRoot -StateRoot $StateRoot -AuthoritativeHwm $authoritativeHwm -Evidence $Evidence

Write-Host "PERSISTENT STATE ROOT: $($result.Root)"
Write-Host "PERSISTENT HWM BEFORE: $(Format-EbicsVersion $result.PreviousHwm)"
Write-Host "AUTHORITATIVE ISSUED HWM: $(Format-EbicsVersion $result.Hwm)"
Write-Host "HWM MIGRATION: $(if ($result.Changed) { 'PASS' } else { 'NOT REQUIRED' })"
Write-Host "ATOMIC LOCK: $($result.Atomic)"
Write-Host "NEW VERSION RESERVED: NO"
