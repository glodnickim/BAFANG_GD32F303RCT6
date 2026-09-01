$ErrorActionPreference = 'Stop'
$module = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'scripts\build-version-allocator.psm1'
$root = Join-Path $env:TEMP ('ebics-version-test-' + [guid]::NewGuid().ToString('N'))
Import-Module $module -Force
try {
    $repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $gitRepo = $repo.Replace('\', '/')
    $commonGit = @(& git -c "safe.directory=$gitRepo" -C $repo rev-parse --path-format=absolute --git-common-dir)
    $expectedDefaultRoot = Join-Path (Split-Path -Parent (Split-Path -Parent $commonGit[0].Trim())) '.ebics-version-state'
    if ((Get-EbicsVersionStateRoot $repo) -ne $expectedDefaultRoot) { throw 'linked worktree default does not resolve shared state root' }
    Initialize-EbicsVersionState $root -AllowInitialMigration | Out-Null
    $a = Reserve-EbicsCanonicalVersion 'C:\worktree-A' $root 1
    $b = Reserve-EbicsCanonicalVersion 'C:\worktree-B' $root 2
    if ($a.Versions[0] -ne '0.0460' -or $b.Versions[0] -ne '0.0461' -or $b.Versions[1] -ne '0.0462') { throw 'sequential/pair allocation failed' }
    if ((Get-EbicsCanonicalHwm 'C:\worktree-C' $root) -ne '0.0462') { throw 'shared HWM failed' }
    $jobs = 1..4 | ForEach-Object { Start-Job -ArgumentList $module,$root -ScriptBlock { param($m,$r) Import-Module $m -Force; (Reserve-EbicsCanonicalVersion 'C:\other-worktree' $r 1).Versions[0] } }
    $v = @($jobs | Wait-Job | Receive-Job); $jobs | Remove-Job
    if (($v | Sort-Object -Unique).Count -ne 4) { throw 'duplicate allocation under concurrency' }

    # A controlled HWM reconciliation raises an existing stale state under the same lock but
    # never allocates a firmware version.  Lowering HWM is forbidden.
    $syncRoot = "$root-sync"
    Initialize-EbicsVersionState $syncRoot -AllowInitialMigration | Out-Null
    $sync = Sync-EbicsCanonicalHwm 'C:\worktree-sync' $syncRoot 470 'host-test authoritative issued evidence'
    if (-not $sync.Changed -or $sync.PreviousHwm -ne 459 -or $sync.Hwm -ne 470 -or $sync.NewVersionReserved) { throw 'controlled HWM reconciliation failed' }
    if ((Get-EbicsCanonicalHwm 'C:\worktree-sync-check' $syncRoot) -ne '0.0470') { throw 'reconciled HWM was not persistent' }
    $postSync = Reserve-EbicsCanonicalVersion 'C:\worktree-sync-reserve' $syncRoot 1
    if ($postSync.Versions[0] -ne '0.0471') { throw 'reservation after HWM reconciliation was not monotonic' }
    $syncState = Get-Content -LiteralPath (Join-Path $syncRoot 'M820_BL820.json') -Raw | ConvertFrom-Json
    if ($syncState.hwm_reconciled_from -ne 459 -or $syncState.hwm_reconciliation_evidence -ne 'host-test authoritative issued evidence') { throw 'reconciliation evidence was not retained after reservation' }
    $failed = $false
    try { Sync-EbicsCanonicalHwm 'C:\worktree-sync' $syncRoot 469 'attempted downgrade' | Out-Null } catch { $failed = $true }
    if (-not $failed) { throw 'HWM downgrade was accepted' }
    Remove-Item -LiteralPath $syncRoot -Recurse -Force

    # Missing, malformed and semantically invalid state must fail closed; AUTO may not recreate it.
    $state = Join-Path $root 'M820_BL820.json'
    foreach ($bad in @('', '{', '{"schema":1,"target":"M820_BL820"}', '{"schema":2,"target":"M820_BL820","hwm":500}', '{"schema":1,"target":"OTHER","hwm":500}', '{"schema":1,"target":"M820_BL820","hwm":458}')) {
        Set-Content -LiteralPath $state -Value $bad -Encoding UTF8
        $failed = $false
        try { Reserve-EbicsCanonicalVersion 'C:\worktree-invalid' $root 1 | Out-Null } catch { $failed = $true }
        if (-not $failed) { throw "invalid state was accepted: $bad" }
    }
    Remove-Item -LiteralPath $state -Force
    $failed = $false
    try { Reserve-EbicsCanonicalVersion 'C:\worktree-missing' $root 1 | Out-Null } catch { $failed = $true }
    if (-not $failed) { throw 'missing state was recreated by AUTO path' }

    # A held lock is a hard stop, never a parallel allocation.  A .local file is ignored.
    $lockRoot = "$root-lock"
    Initialize-EbicsVersionState $lockRoot -AllowInitialMigration | Out-Null
    New-Item -ItemType File -Path (Join-Path $lockRoot '.local') -Force | Out-Null
    $lock = New-Item -ItemType Directory -Path (Join-Path $lockRoot 'M820_BL820.lock')
    $failed = $false
    try { Reserve-EbicsCanonicalVersion 'C:\worktree-locked' $lockRoot 1 -LockTimeoutSeconds 1 | Out-Null } catch { $failed = $true }
    Remove-Item -LiteralPath $lock.FullName -Force
    Remove-Item -LiteralPath $lockRoot -Recurse -Force
    if (-not $failed) { throw 'existing lock did not stop allocation' }

    # A state that cannot be written is a hard stop, never an implicit local-counter fallback.
    $readonlyRoot = "$root-readonly"
    Initialize-EbicsVersionState $readonlyRoot -AllowInitialMigration | Out-Null
    $readonlyState = Get-Item -LiteralPath (Join-Path $readonlyRoot 'M820_BL820.json')
    $readonlyState.IsReadOnly = $true
    $failed = $false
    try { Reserve-EbicsCanonicalVersion 'C:\worktree-readonly' $readonlyRoot 1 | Out-Null } catch { $failed = $true }
    $readonlyState.IsReadOnly = $false
    Remove-Item -LiteralPath $readonlyRoot -Recurse -Force
    if (-not $failed) { throw 'unwritable state did not stop allocation' }

    # The post-build gate must reject a deliberately mismatched published identity.
    $identity = Join-Path $root 'identity'
    New-Item -ItemType Directory -Force -Path $identity | Out-Null
    $header = Join-Path $identity 'build_version.h'; $manifest = Join-Path $identity 'manifest.json'; $artifact = Join-Path $identity '0.0999_M820_BL820.bin'
    Set-Content -LiteralPath $header -Value '#define EBICS_BUILD_VERSION "0.0999"' -Encoding ascii
    '{"version":"0.0999"}' | Set-Content -LiteralPath $manifest -Encoding UTF8
    New-Item -ItemType File -Path $artifact | Out-Null
    Test-EbicsVersionIdentity '0.0999' $header $manifest $artifact | Out-Null
    '{"version":"0.0998"}' | Set-Content -LiteralPath $manifest -Encoding UTF8
    $failed = $false
    try { Test-EbicsVersionIdentity '0.0999' $header $manifest $artifact | Out-Null } catch { $failed = $true }
    if (-not $failed) { throw 'identity mismatch was published' }
    Write-Output 'build_version_allocator_host: PASS'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath "$root-sync" -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath "$root-readonly" -Recurse -Force -ErrorAction SilentlyContinue
}
