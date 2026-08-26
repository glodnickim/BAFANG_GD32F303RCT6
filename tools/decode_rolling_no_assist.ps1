<#
.SYNOPSIS
    Decode FW-122 ROLLING_NO_ASSIST CAN frames to analyzer-ready CSV.

.DESCRIPTION
    Layout dispatch is exclusively by the schema byte in the 0x10248 metadata
    header. Schema 1 (legacy, three data fragments / 24 bytes actually on wire)
    remains supported. Schema 2 requires its exact metadata and six data
    fragments / 44 logical bytes. Schema 3 (FW-122.1) reuses schema 2's six data
    fragments / same 7-frame-per-sample wire cost, but DATA 5 bytes 4/5 (zero
    padding in schema 2) become real fields: pwm_cutoff_progress and
    hall_timeout_progress, and status_flags bit 0x08 (RNA_STATUS_PWM_CUTOFF_ACTIVE)
    becomes meaningful. Unknown or inconsistent layouts fail closed; the decoder
    never infers a layout from frame count.

    Accepted line forms include the project sniffer form:
        ID:80010248 Len:8 Data:02 2A 06 07 03 2C FA 08
    and a compact EFID followed by eight bytes.
#>
param(
    [Parameter(Mandatory=$true)][string]$InputFile,
    [string]$OutputFile,
    [switch]$RequireCompleteCapture
)

if (-not $OutputFile) {
    $OutputFile = [System.IO.Path]::ChangeExtension($InputFile, ".csv")
}

$EFID_HEADER    = [uint32]0x00010248
$EFID_DATA_BASE = [uint32]0x00010249
$caseNames = @{ 0 = 'NONE'; 1 = 'A-PAS'; 2 = 'B-BRIDGE'; 3 = 'C-FOC' }

function Read-Int16BE([byte]$High, [byte]$Low) {
    $unsigned = ([int]$High -shl 8) -bor [int]$Low
    if ($unsigned -ge 0x8000) { return $unsigned - 0x10000 }
    return $unsigned
}

function Read-UInt16BE([byte]$High, [byte]$Low) {
    return ([int]$High -shl 8) -bor [int]$Low
}

function Read-Int8([byte]$Value) {
    if ($Value -ge 0x80) { return [int]$Value - 0x100 }
    return [int]$Value
}

function Case-Name([int]$CaseID) {
    if ($caseNames.ContainsKey($CaseID)) { return $caseNames[$CaseID] }
    return "UNKNOWN($CaseID)"
}

# Parse only the recorder's reserved EFID interval. Six data EFIDs are needed
# for schema 2; schema-specific acceptance is enforced during assembly below.
$frames = @()
foreach ($line in (Get-Content -LiteralPath $InputFile)) {
    $line = $line.Trim()
    if ($line -eq '' -or $line.StartsWith('#')) { continue }
    $efid = $null
    $hexBytes = @()
    if ($line -match 'ID:800(1024[89A-Ea-e]).*?Data:((?:[0-9A-Fa-f]{2}\s*){8})') {
        $efid = [uint32]::Parse($Matches[1], [System.Globalization.NumberStyles]::HexNumber)
        $hexBytes = @($Matches[2].Trim() -split '\s+')
    } elseif ($line -match '^\s*(?:0x)?(?:000)?(1024[89A-Ea-e])\s+(.*)$') {
        $efid = [uint32]::Parse($Matches[1], [System.Globalization.NumberStyles]::HexNumber)
        $hexBytes = @([regex]::Matches($Matches[2], '(?i)(?<![0-9A-F])[0-9A-F]{2}(?![0-9A-F])') |
            ForEach-Object { $_.Value } | Select-Object -Last 8)
    }
    if ($null -eq $efid -or $hexBytes.Count -ne 8) { continue }
    $data = @($hexBytes | ForEach-Object {
        [byte]::Parse($_, [System.Globalization.NumberStyles]::HexNumber)
    })
    $frames += @{ EFID = $efid; Data = $data }
}

$samples = @()
$current = $null

function Complete-CurrentSample($Sample) {
    if ($null -eq $Sample) { return $null }
    if ($Sample.Frags.Count -ne $Sample.ExpectedFragments) {
        $message = "Incomplete schema $($Sample.Schema) sample: $($Sample.Frags.Count)/$($Sample.ExpectedFragments) data fragments"
        if ($RequireCompleteCapture) { throw $message }
        Write-Warning "Skipping $message"
        return $null
    }
    for ($i = 0; $i -lt $Sample.ExpectedFragments; $i++) {
        if (-not $Sample.Frags.ContainsKey($i)) {
            $message = "Schema $($Sample.Schema) sample missing data fragment $i"
            if ($RequireCompleteCapture) { throw $message }
            Write-Warning "Skipping $message"
            return $null
        }
    }
    return $Sample
}

foreach ($f in $frames) {
    if ($f.EFID -eq $EFID_HEADER) {
        $complete = Complete-CurrentSample $current
        if ($null -ne $complete) { $samples += $complete }

        $schema = [int]$f.Data[0]
        if ($schema -eq 1) {
            $expectedFragments = 3
            $logicalBytes = 24 # actual schema-1 wire payload, not the old 28-B RAM struct
        } elseif ($schema -eq 2) {
            if ($f.Data[2] -ne 6 -or $f.Data[5] -ne 44 -or
                $f.Data[6] -ne 250 -or $f.Data[7] -ne 8) {
                throw "Invalid schema-2 header metadata: expected fragments=6, bytes=44, Hz=250, confirm=8"
            }
            $expectedFragments = 6
            $logicalBytes = 44
        } elseif ($schema -eq 3) {
            if ($f.Data[2] -ne 6 -or $f.Data[5] -ne 48 -or
                $f.Data[6] -ne 250 -or $f.Data[7] -ne 8) {
                throw "Invalid schema-3 header metadata: expected fragments=6, bytes=48, Hz=250, confirm=8"
            }
            $expectedFragments = 6
            $logicalBytes = 48
        } else {
            throw "Unsupported ROLLING_NO_ASSIST schema $schema"
        }

        $current = @{
            Schema = $schema
            SessionID = [int]$f.Data[1]
            CaptureID = [int]$f.Data[3]
            TriggerCase = [int]$f.Data[4]
            ExpectedFragments = $expectedFragments
            LogicalBytes = $logicalBytes
            Frags = @{}
        }
        continue
    }

    if ($null -eq $current) {
        if ($RequireCompleteCapture) { throw "DATA frame 0x$('{0:X}' -f $f.EFID) arrived before its HEADER" }
        continue
    }
    $fragIdx = [int]($f.EFID - $EFID_DATA_BASE)
    if ($fragIdx -ge 0 -and $fragIdx -lt $current.ExpectedFragments) {
        if ($current.Frags.ContainsKey($fragIdx)) {
            throw "Duplicate data fragment $fragIdx in schema $($current.Schema) sample"
        }
        $current.Frags[$fragIdx] = $f.Data
    }
}
$complete = Complete-CurrentSample $current
if ($null -ne $complete) { $samples += $complete }

$rows = @()
$index = 0
foreach ($s in $samples) {
    $d = $s.Frags
    $tickAbs = ([uint32]$d[0][0] -shl 24) -bor ([uint32]$d[0][1] -shl 16) -bor
               ([uint32]$d[0][2] -shl 8) -bor [uint32]$d[0][3]
    $flags = [int]$d[0][4]

    # Defaults are deliberately null: schema 1 must expose its true omissions,
    # not plausible-looking zeroes for signals that were never transmitted.
    $statusFlags = $null; $moe = $null; $currentCal = $null
    $neutralActive = $null; $neutralCounter = $null
    $permissionBits = $null; $reasonBits = $null; $debugFlags = $null
    $iqRequest = $null; $iqAfterLatch = $null; $iqPreRamp = $null
    $loadCentikg = $null; $angleHall = $null; $angleAbsolute = $null
    $angleHallDeg = $null; $angleAbsoluteDeg = $null; $rotorDirection = $null
    $motorVoltageUtilization = $null
    # FW-122.1 (schema 3 only): D2 raw evidence - see the wire-format comment in
    # inc/rolling_no_assist_diag.h. Left null for schema 1/2, which never transmitted them.
    $pwmCutoffActive = $null; $pwmCutoffProgress = $null; $hallTimeoutProgress = $null

    if ($s.Schema -eq 1) {
        $caseId = [int]$d[0][5]
        $bridgeLifecycle = [int]$d[0][6]
        $hall = [int]$d[0][7]
        $iqSetpoint = Read-Int16BE $d[1][0] $d[1][1]
        $iqBeforePu = Read-Int16BE $d[1][2] $d[1][3]
        $iqActualRaw = Read-Int16BE $d[1][4] $d[1][5]
        $piQInt = Read-Int16BE $d[1][6] $d[1][7]
        $piDInt = Read-Int16BE $d[2][0] $d[2][1]
        $erps = Read-UInt16BE $d[2][2] $d[2][3]
        $rpm = Read-Int16BE $d[2][4] $d[2][5]
        $loadThreshold = Read-UInt16BE $d[2][6] $d[2][7]
    } else {
        $statusFlags = [int]$d[0][5]
        $caseId = [int]$d[0][6]
        $bridgeLifecycle = [int]$d[0][7]
        $hall = [int]$d[1][0]
        $permissionBits = [int]$d[1][1]
        $reasonBits = [int]$d[1][2]
        $debugFlags = [int]$d[1][3]
        $iqBeforePu = Read-Int16BE $d[1][4] $d[1][5]
        $iqRequest = Read-Int16BE $d[1][6] $d[1][7]
        $iqAfterLatch = Read-Int16BE $d[2][0] $d[2][1]
        $iqPreRamp = Read-Int16BE $d[2][2] $d[2][3]
        $iqSetpoint = Read-Int16BE $d[2][4] $d[2][5]
        $iqActualRaw = Read-Int16BE $d[2][6] $d[2][7]
        $piQInt = Read-Int16BE $d[3][0] $d[3][1]
        $piDInt = Read-Int16BE $d[3][2] $d[3][3]
        $erps = Read-UInt16BE $d[3][4] $d[3][5]
        $rpm = Read-Int16BE $d[3][6] $d[3][7]
        $loadCentikg = Read-UInt16BE $d[4][0] $d[4][1]
        $loadThreshold = Read-UInt16BE $d[4][2] $d[4][3]
        $angleHall = Read-UInt16BE $d[4][4] $d[4][5]
        $angleAbsolute = Read-UInt16BE $d[4][6] $d[4][7]
        $angleHallDeg = [math]::Round($angleHall * 360.0 / 65536.0, 6)
        $angleAbsoluteDeg = [math]::Round($angleAbsolute * 360.0 / 65536.0, 6)
        $rotorDirection = Read-Int8 $d[5][0]
        $neutralCounter = [int]$d[5][1]
        $motorVoltageUtilization = Read-UInt16BE $d[5][2] $d[5][3]
        if ($s.Schema -eq 3) {
            $pwmCutoffProgress = [int]$d[5][4]
            $hallTimeoutProgress = [int]$d[5][5]
            if ($d[5][6] -ne 0 -or $d[5][7] -ne 0) {
                throw "Invalid schema-3 padding: fragment 5 bytes 6..7 must be zero"
            }
            $pwmCutoffActive = [int](($statusFlags -band 0x08) -ne 0)
        } elseif ($d[5][4] -ne 0 -or $d[5][5] -ne 0 -or $d[5][6] -ne 0 -or $d[5][7] -ne 0) {
            throw "Invalid schema-2 padding: fragment 5 bytes 4..7 must be zero"
        }
        $moe = [int](($statusFlags -band 0x01) -ne 0)
        $currentCal = [int](($statusFlags -band 0x02) -ne 0)
        $neutralActive = [int](($statusFlags -band 0x04) -ne 0)
    }

    $caseA = [int](($flags -band 0x01) -ne 0)
    $caseB = [int](($flags -band 0x02) -ne 0)
    $caseC = [int](($flags -band 0x04) -ne 0)
    $riderActive = [int](($flags -band 0x08) -ne 0)
    $permission = [int](($flags -band 0x10) -ne 0)
    $loadMet = [int](($flags -band 0x20) -ne 0)
    $noDemand = [int](($flags -band 0x40) -ne 0)
    $pwmOn = [int](($flags -band 0x80) -ne 0)

    $rows += [pscustomobject][ordered]@{
        sample_index = $index
        schema_version = $s.Schema
        session_id = $s.SessionID
        capture_id = $s.CaptureID
        capture_trigger_case_id = $s.TriggerCase
        capture_trigger_case = Case-Name $s.TriggerCase
        case_id = $caseId
        trigger_case = Case-Name $caseId
        timestamp = $tickAbs
        ticks_ms = [math]::Round($tickAbs / 4.0, 3)
        flags = $flags
        status_flags = $statusFlags
        cases_A_B_C = "$caseA$caseB$caseC"
        rider_active = $riderActive
        permission = $permission
        load_met = $loadMet
        no_demand = $noDemand
        pwm_on = $pwmOn
        hardware_moe = $moe
        bridge_lifecycle = $bridgeLifecycle
        hall_state = $hall
        permission_bits = $permissionBits
        reason_bits = $reasonBits
        debug_flags = $debugFlags
        current_cal_foc_allowed = $currentCal
        neutral_dwell_active = $neutralActive
        neutral_dwell_counter = $neutralCounter
        iq_before_pu = $iqBeforePu
        iq_request = $iqRequest
        iq_after_latch_floor = $iqAfterLatch
        iq_pre_ramp = $iqPreRamp
        iq_setpoint = $iqSetpoint
        final_iq = $iqSetpoint
        iq_actual_magnitude = [math]::Abs($iqActualRaw)
        iq_actual_raw = $iqActualRaw
        pi_q_int = $piQInt
        pi_d_int = $piDInt
        erps = $erps
        rpm = $rpm
        load_centikg = $loadCentikg
        load_threshold = $loadThreshold
        angle_hall = $angleHall
        angle_hall_deg = $angleHallDeg
        angle_absolute = $angleAbsolute
        angle_absolute_deg = $angleAbsoluteDeg
        rotor_direction = $rotorDirection
        motor_voltage_utilization = $motorVoltageUtilization
        pwm_cutoff_active = $pwmCutoffActive
        pwm_cutoff_progress = $pwmCutoffProgress
        hall_timeout_progress = $hallTimeoutProgress
        # FW-122.1: D2 raw evidence, derived here (not on the wire, not a firmware verdict) -
        # exactly the two facts FW-124 section 6 identified: a positive final_iq observed while
        # the soft-cutoff owns CCR. See the Trace Analyzer for the full CASE B subreason logic
        # and the CUTOFF_ABORTED_BY_REDEMAND transition marker across consecutive samples.
        d2_redemand_during_cutoff = if ($null -ne $pwmCutoffActive) {
            [int]($pwmCutoffActive -eq 1 -and $iqSetpoint -gt 0)
        } else { $null }
    }
    $index++
}

$rows | Export-Csv -LiteralPath $OutputFile -NoTypeInformation -Encoding UTF8
Write-Host "Decoded $($rows.Count) samples to $OutputFile"
if ($rows.Count -gt 0) {
    Write-Host "Schema: $($samples[0].Schema) | Session: $($samples[0].SessionID) | Capture: $($samples[0].CaptureID)"
}

if ($RequireCompleteCapture) {
    $expectedSamples = 256
    $expectedFrames = 256 * 7
    if ($frames.Count -ne $expectedFrames) {
        throw "Incomplete transport: expected $expectedFrames ROLLING_NO_ASSIST frames, received $($frames.Count)"
    }
    if ($samples.Count -ne $expectedSamples -or $rows.Count -ne $expectedSamples) {
        throw "Incomplete transport: expected $expectedSamples complete samples, decoded $($rows.Count)"
    }

    $first = $samples[0]
    $currentSchema = 3
    if ($first.Schema -ne $currentSchema) {
        throw "Transport proof requires schema $currentSchema, got schema $($first.Schema)"
    }
    for ($i = 0; $i -lt $expectedSamples; $i++) {
        $sample = $samples[$i]
        if ($sample.Schema -ne $currentSchema -or $sample.SessionID -ne $first.SessionID -or
            $sample.CaptureID -ne $first.CaptureID -or $sample.TriggerCase -ne $first.TriggerCase) {
            throw "Transport mixes captures at sample $i"
        }
        if ([int]$rows[$i].sample_index -ne $i) {
            throw "Non-continuous decoded sample index at row $i"
        }
        if ($i -gt 0) {
            $previous = [uint32]$rows[$i - 1].timestamp
            $currentTick = [uint32]$rows[$i].timestamp
            if ([uint32]($currentTick - $previous) -ne 16) {
                throw "Non-continuous sample timestamps at sample ${i}: expected 16 ticks / 4 ms"
            }
        }
    }
    Write-Host "Transport PASS: $expectedSamples samples, $expectedFrames frames, indices 0..255 continuous, no missing fragments."
}
