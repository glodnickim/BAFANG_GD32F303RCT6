param([string]$StateRoot = "", [string]$OutputDir = ".build", [ValidateSet("debug","release")][string]$Profile="debug")
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Import-Module (Join-Path $PSScriptRoot 'build-version-allocator.psm1') -Force
$pair=Reserve-EbicsCanonicalVersion $root $StateRoot 2
Write-Host "PAIR RESERVATION: NORMAL=$($pair.Versions[0]) DIAG=$($pair.Versions[1]) (atomic=$($pair.Atomic))"
$normalArgs = @('-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'build-firmware.ps1'),'-Target','M820_BL820','-Profile',$Profile,'-Variant','normal','-BuildMode','Reserved','-Version',$pair.Versions[0],'-OutputDir',$OutputDir)
if($StateRoot){ $normalArgs += @('-VersionStateRoot',$StateRoot) }
& powershell @normalArgs
if($LASTEXITCODE -ne 0){ throw 'NORMAL failed; both pair numbers remain consumed.' }
$diagArgs = @('-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'build-firmware.ps1'),'-Target','M820_BL820','-Profile',$Profile,'-Variant','diagnostic','-BuildMode','Reserved','-Version',$pair.Versions[1],'-OutputDir',$OutputDir)
if($StateRoot){ $diagArgs += @('-VersionStateRoot',$StateRoot) }
& powershell @diagArgs
if($LASTEXITCODE -ne 0){ throw 'DIAG failed; both pair numbers remain consumed.' }
