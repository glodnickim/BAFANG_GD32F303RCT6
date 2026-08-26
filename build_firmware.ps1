# DEPRECATED THIN COMPATIBILITY WRAPPER
#
# This script is deprecated. Use the canonical build engine directly:
#
#   scripts\build-firmware.ps1 -Target M820_BL820 -Profile debug -Variant normal
#   scripts\build-firmware.ps1 -Target M820_BL820 -Profile debug -Variant diagnostic
#
# This wrapper exists only for backward compatibility with historical documentation
# and will be removed in a future release.

param(
    [string]$OutputDir = ".build",
    [string]$Toolchain = "",
    [string]$ArtifactName = "0.002",
    [string]$BootloaderMode = "820",
    [switch]$CanDiagnostics
)

Write-Host ""
Write-Host "WARNING: build_firmware.ps1 is deprecated." -ForegroundColor Yellow
Write-Host "  Canonical M820_BL820 build engine: scripts\build-firmware.ps1" -ForegroundColor Yellow
Write-Host ""

$variant = if ($CanDiagnostics.IsPresent) { "diagnostic" } else { "normal" }
$version = $ArtifactName

$canonicalScript = Join-Path $PSScriptRoot "scripts\build-firmware.ps1"
if (-not (Test-Path -LiteralPath $canonicalScript)) {
    throw "Canonical build script not found: $canonicalScript"
}

& $canonicalScript `
    -Target M820_BL820 `
    -Profile debug `
    -Variant $variant `
    -Version $version `
    -OutputDir $OutputDir `
    -Toolchain $Toolchain
