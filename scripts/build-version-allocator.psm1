Set-StrictMode -Version Latest

function Get-EbicsVersionStateRoot {
    param([string]$RepoRoot, [string]$StateRoot = "")
    if ($StateRoot) { return [IO.Path]::GetFullPath($StateRoot) }
    if ($env:EBICS_VERSION_STATE_ROOT) { return [IO.Path]::GetFullPath($env:EBICS_VERSION_STATE_ROOT) }
    # Resolve the common .git directory, not this worktree's directory.  A linked worktree
    # otherwise places its default state under .work and silently forks the allocator.
    $gitRepo = [IO.Path]::GetFullPath($RepoRoot).Replace("\", "/")
    $commonGit = @(& git -c "safe.directory=$gitRepo" -C $RepoRoot rev-parse --path-format=absolute --git-common-dir 2>$null)
    if ($LASTEXITCODE -eq 0 -and $commonGit.Count -eq 1 -and $commonGit[0]) {
        $mainRepoRoot = Split-Path -Parent ([IO.Path]::GetFullPath($commonGit[0].Trim()))
        return (Join-Path (Split-Path -Parent $mainRepoRoot) ".ebics-version-state")
    }
    # Fallback only for a non-Git fixture; production repositories must resolve via common .git.
    return (Join-Path (Split-Path -Parent $RepoRoot) ".ebics-version-state")
}

function Enter-EbicsVersionLock {
    param([string]$Root, [int]$TimeoutSeconds = 20)
    $lock = Join-Path $Root "M820_BL820.lock"
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ($true) {
        try { New-Item -ItemType Directory -Path $lock -ErrorAction Stop | Out-Null; return $lock }
        catch [System.IO.IOException] {
            if ([DateTime]::UtcNow -ge $deadline) { throw "Timed out waiting for global version lock: $lock" }
            Start-Sleep -Milliseconds 50
        }
    }
}
function Exit-EbicsVersionLock { param([string]$Lock) Remove-Item -LiteralPath $Lock -Force -ErrorAction SilentlyContinue }

function Initialize-EbicsVersionState {
    param([string]$Root, [switch]$AllowInitialMigration, [int]$InitialHwm = 459)
    New-Item -ItemType Directory -Force -Path $Root | Out-Null
    $path = Join-Path $Root "M820_BL820.json"
    $marker = Join-Path $Root "M820_BL820.migration.json"
    if (-not (Test-Path -LiteralPath $path)) {
        if (-not $AllowInitialMigration) { throw "Global allocator state is missing: $path. AUTO stops; use an audited one-time migration, never a numeric fallback." }
        if (Test-Path -LiteralPath $marker) { throw "Migration marker exists but state is missing: $path. AUTO stops; recover HWM from authoritative release evidence." }
        if ($InitialHwm -lt 459) { throw "Initial migration HWM is below proven canonical 0.0459." }
        # Migration HWM: QS-1 DIAG 0.0459. 0.0412/0.0413 are explicitly noncanonical tests.
        @{ schema = 1; target = "M820_BL820"; initial_hwm = $InitialHwm; migrated_from = "canonical evidence: QS-1 DIAG 0.0459"; migrated_utc = [DateTime]::UtcNow.ToString("o") } |
            ConvertTo-Json | Set-Content -LiteralPath $marker -Encoding UTF8
        @{ schema = 1; target = "M820_BL820"; hwm = $InitialHwm; migrated_from = "canonical evidence: QS-1 DIAG 0.0459"; updated_utc = [DateTime]::UtcNow.ToString("o") } |
            ConvertTo-Json | Set-Content -LiteralPath $path -Encoding UTF8
    }
    return $path
}

function Get-EbicsVersionState {
    param([string]$Root)
    $path = Join-Path $Root "M820_BL820.json"
    if (-not (Test-Path -LiteralPath $path)) { throw "Global allocator state is missing: $path. AUTO stops before reservation." }
    try { $s = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json } catch { throw "Global version state is unreadable: $path" }
    $hwm = 0
    try { $hwm = [int]$s.hwm } catch { throw "Global version state has nonnumeric HWM: $path" }
    if ($s.schema -ne 1 -or $s.target -ne "M820_BL820" -or $hwm -lt 459) {
        throw "Global version state is invalid or below migrated HWM: $path"
    }
    return $s
}

function Sync-EbicsCanonicalHwm {
    param(
        [string]$RepoRoot,
        [string]$StateRoot = "",
        [ValidateRange(459,9999)][int]$AuthoritativeHwm,
        [ValidateNotNullOrEmpty()][string]$Evidence,
        [ValidateRange(1,120)][int]$LockTimeoutSeconds = 20
    )
    $root = Get-EbicsVersionStateRoot $RepoRoot $StateRoot
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    $lock = Enter-EbicsVersionLock $root $LockTimeoutSeconds
    try {
        $state = Get-EbicsVersionState $root
        $previous = [int]$state.hwm
        if ($AuthoritativeHwm -lt $previous) {
            throw "Refusing to lower global canonical HWM: state is 0.$($previous.ToString('D4')), authoritative evidence requests 0.$($AuthoritativeHwm.ToString('D4'))."
        }
        if ($AuthoritativeHwm -eq $previous) {
            return [pscustomobject]@{ Root=$root; PreviousHwm=$previous; Hwm=$previous; Changed=$false; Atomic=$true; NewVersionReserved=$false }
        }
        [ordered]@{
            schema = 1
            target = "M820_BL820"
            hwm = $AuthoritativeHwm
            migrated_from = $state.migrated_from
            hwm_reconciled_from = $previous
            hwm_reconciled_utc = [DateTime]::UtcNow.ToString("o")
            hwm_reconciliation_evidence = $Evidence
            updated_utc = [DateTime]::UtcNow.ToString("o")
        } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root "M820_BL820.json") -Encoding UTF8
        return [pscustomobject]@{ Root=$root; PreviousHwm=$previous; Hwm=$AuthoritativeHwm; Changed=$true; Atomic=$true; NewVersionReserved=$false }
    } finally { Exit-EbicsVersionLock $lock }
}

function Reserve-EbicsCanonicalVersion {
    param(
        [string]$RepoRoot,
        [string]$StateRoot = "",
        [ValidateRange(1,2)][int]$Count = 1,
        [ValidateRange(1,120)][int]$LockTimeoutSeconds = 20
    )
    $root = Get-EbicsVersionStateRoot $RepoRoot $StateRoot
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    $lock = Enter-EbicsVersionLock $root $LockTimeoutSeconds
    try {
        $state = Get-EbicsVersionState $root
        $old = [int]$state.hwm; $numbers = @()
        1..$Count | ForEach-Object { $numbers += (++$old) }
        $nextState = [ordered]@{ schema = 1; target = "M820_BL820"; hwm = $old; migrated_from = $state.migrated_from; updated_utc = [DateTime]::UtcNow.ToString("o") }
        foreach ($name in @("hwm_reconciled_from", "hwm_reconciled_utc", "hwm_reconciliation_evidence")) {
            if ($state.PSObject.Properties.Name -contains $name) { $nextState[$name] = $state.$name }
        }
        $nextState | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root "M820_BL820.json") -Encoding UTF8
        return [pscustomobject]@{ Root=$root; PreviousHwm=[int]$state.hwm; Versions=@($numbers | ForEach-Object { '0.{0:D4}' -f $_ }); Atomic=$true }
    } finally { Exit-EbicsVersionLock $lock }
}

function Get-EbicsCanonicalHwm { param([string]$RepoRoot,[string]$StateRoot="") $s=Get-EbicsVersionState (Get-EbicsVersionStateRoot $RepoRoot $StateRoot); return ('0.{0:D4}' -f [int]$s.hwm) }

function Test-EbicsVersionIdentity {
    param([string]$Version, [string]$HeaderPath, [string]$ManifestPath, [string]$ArtifactPath, [string]$Variant = "normal")
    $headerText = Get-Content -LiteralPath $HeaderPath -Raw
    try { $manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json }
    catch { throw "VERSION IDENTITY FAIL: manifest is unreadable: $ManifestPath" }
    $suffix = if ($Variant -eq "diagnostic") { "_DIAG" } else { "" }
    $artifactName = (Get-Item -LiteralPath $ArtifactPath).Name
    if ($headerText -notmatch ('#define\s+EBICS_BUILD_VERSION\s+"' + [regex]::Escape($Version) + '"') -or
        $manifest.version -ne $Version -or $artifactName -notlike ("$Version`_*$suffix.bin")) {
        throw "VERSION IDENTITY FAIL: header, manifest or artifact name disagrees with reserved version $Version. Reservation remains consumed."
    }
    return $true
}
Export-ModuleMember -Function Get-EbicsVersionStateRoot,Initialize-EbicsVersionState,Sync-EbicsCanonicalHwm,Reserve-EbicsCanonicalVersion,Get-EbicsCanonicalHwm,Test-EbicsVersionIdentity
